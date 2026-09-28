#include <algorithm>
#include <atomic>
#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <pagepilot/cdp_channel.hpp>
#include <deque>
#include <future>
#include <map>
#include <mutex>
#include <thread>
namespace pagepilot {
namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
using Tcp = asio::ip::tcp;
namespace {
void validate_port(unsigned port, MsDuration timeout) {
  if (!port || port > 65535 || timeout.count() < 1 || timeout.count() > 60000)
    throw BridgeError("invalid loopback port or connection deadline");
}
} // namespace
JsonDoc discover_browser(unsigned port, MsDuration timeout) {
  cancellation_point();
  validate_port(port, timeout);
  asio::io_context loop;
  beast::tcp_stream socket(loop);
  beast::flat_buffer buffer;
  http::request<http::empty_body> request{http::verb::get, "/json/version", 11};
  request.set(http::field::host, "127.0.0.1:" + std::to_string(port));
  request.set(http::field::user_agent, "PagePilot/1.0");
  http::response_parser<http::string_body> response;
  response.body_limit(1024 * 1024);
  boost::system::error_code failure;
  bool complete = false;
  socket.expires_after(timeout);
  socket.async_connect(Tcp::endpoint(asio::ip::make_address("127.0.0.1"),
                                     static_cast<unsigned short>(port)),
                       [&](auto error) {
                         if (error) {
                           failure = error;
                           return;
                         }
                         http::async_write(
                             socket, request, [&](auto written, std::size_t) {
                               if (written) {
                                 failure = written;
                                 return;
                               }
                               http::async_read(socket, buffer, response,
                                                [&](auto read, std::size_t) {
                                                  failure = read;
                                                  complete = !read;
                                                });
                             });
                       });
  do {
    cancellation_point();
    loop.run_for(MsDuration(10));
  } while (!loop.stopped());
  cancellation_point();
  if (!complete || failure)
    throw BridgeError("Chrome discovery failed: " + failure.message());
  if (response.get().result() != http::status::ok)
    throw BridgeError("Chrome discovery returned a non-200 response");
  return parse_message(response.get().body(), 1024 * 1024);
}
struct CdpChannel::Engine {
  asio::io_context loop;
  websocket::stream<beast::tcp_stream> socket{loop};
  beast::flat_buffer inbound;
  std::deque<std::shared_ptr<std::string>> outbound;
  struct PendingCall {
    std::shared_ptr<std::promise<JsonDoc>> result;
    std::string session;
  };
  std::map<std::uint64_t, PendingCall> pending;
  struct BufferedEvent {
    JsonDoc value;
    std::size_t bytes;
  };
  std::deque<BufferedEvent> events;
  std::size_t event_bytes = 0, outbound_bytes = 0;
  std::mutex event_mutex;
  std::thread runner;
  std::atomic<bool> live{false};
  std::atomic<std::uint64_t> next{1};
  std::shared_ptr<std::promise<void>> opening =
      std::make_shared<std::promise<void>>();
  bool opening_set = false, stopped = false;
  std::string host, path;
  void fail(const std::string &message) {
    if (stopped)
      return;
    stopped = true;
    live = false;
    auto failure = std::make_exception_ptr(BridgeError(message));
    if (!opening_set) {
      opening_set = true;
      opening->set_exception(failure);
    }
    for (auto &[id, call] : pending) {
      (void)id;
      call.result->set_exception(failure);
    }
    pending.clear();
    boost::system::error_code ignored;
    beast::get_lowest_layer(socket).socket().cancel(ignored);
    beast::get_lowest_layer(socket).socket().close(ignored);
  }
  void read_next() {
    socket.async_read(inbound, [this](auto error, std::size_t) {
      if (error) {
        fail("DevTools read failed: " + error.message());
        return;
      }
      try {
        if (!socket.got_text())
          throw BridgeError("DevTools requires text WebSocket messages");
        const auto payload = beast::buffers_to_string(inbound.data());
        auto message = decode_cdp_message(payload);
        inbound.consume(inbound.size());
        if (message.contains("id")) {
          const auto id = message.at("id").get<std::uint64_t>();
          auto found = pending.find(id);
          if (found != pending.end()) {
            // Invalid-session errors can omit sessionId. A supplied session,
            // however, must belong to this request and cannot redirect a reply.
            if (message.contains("sessionId") &&
                message.at("sessionId") != found->second.session)
              throw BridgeError("DevTools response session does not match its request");
            if (message.contains("error"))
              found->second.result->set_exception(std::make_exception_ptr(
                  WireFailure(message.at("error"))));
            else
              found->second.result->set_value(message.at("result"));
            pending.erase(found);
          }
        } else if (message.contains("method")) {
          std::lock_guard lock(event_mutex);
          if (events.size() >= 10000 || payload.size() > 8 * 1024 * 1024 ||
              event_bytes > 8 * 1024 * 1024 - payload.size()) {
            fail("DevTools event queue exceeded its limit");
            return;
          }
          event_bytes += payload.size();
          events.push_back({std::move(message), payload.size()});
        }
      } catch (const std::exception &exception) {
        fail(exception.what());
        return;
      }
      if (!stopped)
        read_next();
    });
  }
  void write_next() {
    if (outbound.empty() || stopped)
      return;
    auto bytes = outbound.front();
    socket.async_write(asio::buffer(*bytes),
                       [this, bytes](auto error, std::size_t) {
                         if (error) {
                           fail("DevTools write failed: " + error.message());
                           return;
                         }
                         outbound_bytes -= outbound.front()->size();
                         outbound.pop_front();
                         write_next();
                       });
  }
  void start(unsigned port, MsDuration timeout, const std::string &endpoint) {
    const auto prefix = "ws://127.0.0.1:" + std::to_string(port);
    const auto alternate = "ws://localhost:" + std::to_string(port);
    auto length = endpoint.starts_with(prefix + "/")      ? prefix.size()
                  : endpoint.starts_with(alternate + "/") ? alternate.size()
                                                          : 0;
    if (!length || endpoint.size() > 8192)
      throw BridgeError(
          "DevTools endpoint must use the discovered loopback port");
    host = "127.0.0.1:" + std::to_string(port);
    path = endpoint.substr(length);
    if (std::any_of(path.begin(), path.end(), [](unsigned char value) {
          return value <= 32 || value >= 127 || value == '#';
        }))
      throw BridgeError("invalid DevTools endpoint path");
    auto ready = opening->get_future();
    beast::get_lowest_layer(socket).expires_after(timeout);
    beast::get_lowest_layer(socket).async_connect(
        Tcp::endpoint(asio::ip::make_address("127.0.0.1"),
                      static_cast<unsigned short>(port)),
        [this, timeout](auto error) {
          if (error) {
            fail("DevTools connection failed: " + error.message());
            return;
          }
          beast::get_lowest_layer(socket).expires_never();
          socket.set_option(websocket::stream_base::timeout{
              timeout, websocket::stream_base::none(), false});
          socket.read_message_max(32 * 1024 * 1024);
          socket.text(true);
          socket.async_handshake(host, path, [this](auto upgraded) {
            if (upgraded) {
              fail("DevTools handshake failed: " + upgraded.message());
              return;
            }
            live = true;
            opening_set = true;
            opening->set_value();
            read_next();
          });
        });
    runner = std::thread([this] {
      try {
        loop.run();
      } catch (const std::exception &e) {
        fail(e.what());
      }
    });
    try {
      while (ready.wait_for(MsDuration(10)) != std::future_status::ready)
        cancellation_point();
      cancellation_point();
      ready.get();
    } catch (...) {
      stop();
      throw;
    }
  }
  void stop() {
    if (!runner.joinable())
      return;
    asio::post(loop, [this] {
      fail("DevTools connection closed");
      // Beast may retain a handshake timer after the TCP socket is cancelled.
      // Pending callers have been failed; teardown must not wait for that
      // timer.
      loop.stop();
    });
    runner.join();
  }
  ~Engine() { stop(); }
};
CdpChannel::CdpChannel(unsigned port, MsDuration timeout)
    : engine_(std::make_unique<Engine>()) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  auto descriptor = discover_browser(port, timeout);
  if (!descriptor.is_object() || !descriptor.contains("webSocketDebuggerUrl") ||
      !descriptor.at("webSocketDebuggerUrl").is_string())
    throw BridgeError("Chrome discovery requires a WebSocket endpoint string");
  const auto left = std::chrono::duration_cast<MsDuration>(
      deadline - std::chrono::steady_clock::now());
  if (left.count() < 1)
    throw BridgeError("DevTools discovery exhausted the connection deadline");
  engine_->start(port, left,
                 descriptor.at("webSocketDebuggerUrl").get<std::string>());
}
CdpChannel::~CdpChannel() = default;
bool CdpChannel::connected() const {
  return engine_ && engine_->live.load();
}
void CdpChannel::disconnect() {
  if (engine_)
    engine_->stop();
}
JsonDoc CdpChannel::call(const std::string &method, const JsonDoc &parameters,
                           const std::string &session, MsDuration timeout,
                           std::function<void()> progress) {
  cancellation_point();
  if (!connected())
    throw BridgeError("DevTools connection is closed");
  if (timeout.count() < 1 || timeout.count() > 60000 || method.empty() ||
      !parameters.is_object())
    throw BridgeError("invalid DevTools call");
  auto response = std::make_shared<std::promise<JsonDoc>>();
  auto future = response->get_future();
  const auto id = engine_->next.fetch_add(1);
  if (id > 9007199254740991ULL)
    throw BridgeError("DevTools request identity space exhausted");
  JsonDoc message = {{"id", id}, {"method", method}, {"params", parameters}};
  if (!session.empty())
    message["sessionId"] = session;
  auto encoded = std::make_shared<std::string>(message.dump());
  if (encoded->size() > 16 * 1024 * 1024)
    throw BridgeError("DevTools request exceeds size limit");
  asio::post(engine_->loop, [engine = engine_.get(), id, response, encoded, session] {
    if (engine->stopped) {
      response->set_exception(std::make_exception_ptr(
          BridgeError("DevTools disconnected before send")));
      return;
    }
    if (engine->pending.size() >= 1024 || engine->outbound.size() >= 1024 ||
        engine->outbound_bytes > 64 * 1024 * 1024 - encoded->size()) {
      response->set_exception(std::make_exception_ptr(
          BridgeError("too many pending DevTools requests")));
      return;
    }
    engine->pending.emplace(id, Engine::PendingCall{response, session});
    engine->outbound_bytes += encoded->size();
    engine->outbound.push_back(encoded);
    if (engine->outbound.size() == 1)
      engine->write_next();
  });
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  try {
    while (true) {
      cancellation_point();
      const auto left = std::chrono::duration_cast<MsDuration>(
          deadline - std::chrono::steady_clock::now());
      if (future.wait_for(
              std::max(MsDuration(0), std::min(left, MsDuration(10)))) ==
          std::future_status::ready)
        return future.get();
      if (std::chrono::steady_clock::now() >= deadline)
        throw DeadlineReached("DevTools request timed out: " + method);
      if (progress)
        progress();
    }
  } catch (...) {
    asio::post(engine_->loop,
               [engine = engine_.get(), id] { engine->pending.erase(id); });
    throw;
  }
}
std::vector<JsonDoc> CdpChannel::drain_events() {
  std::lock_guard lock(engine_->event_mutex);
  std::vector<JsonDoc> result;
  result.reserve(engine_->events.size());
  while (!engine_->events.empty()) {
    engine_->event_bytes -= engine_->events.front().bytes;
    result.push_back(std::move(engine_->events.front().value));
    engine_->events.pop_front();
  }
  return result;
}
} // namespace pagepilot
