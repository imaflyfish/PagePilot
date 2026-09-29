#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <pagepilot/mcp_transport.hpp>
#include <thread>
namespace pagepilot {
namespace {
bool identifier(const JsonDoc &value) {
  return value.is_string() || value.is_number_integer() ||
         value.is_number_unsigned();
}
JsonDoc failure(const JsonDoc &id, const std::string &message) {
  return {{"jsonrpc", "2.0"},
          {"id", id},
          {"error", {{"code", -32001}, {"message", message}}}};
}
} // namespace
struct RequestPump::Engine {
  struct Job {
    JsonDoc message;
    std::string identity;
    std::stop_source cancellation;
    std::size_t bytes = 0;
    bool cancellable = true;
  };
  StdioTransport endpoint;
  Sink sink;
  mutable std::mutex mutex;
  std::mutex output_mutex;
  std::condition_variable available;
  std::deque<std::shared_ptr<Job>> queue;
  std::map<std::string, std::shared_ptr<Job>> pending;
  std::size_t count = 0, bytes = 0, maximum_count, maximum_bytes;
  bool closing = false, aborting = false;
  std::exception_ptr fault;
  std::thread worker;
  Engine(StdioTransport::Handler handler, Sink output, bool compatibility,
         std::size_t limit, std::size_t byte_limit)
      : endpoint(std::move(handler), compatibility), sink(std::move(output)),
        maximum_count(limit), maximum_bytes(byte_limit) {
    if (!maximum_count || !maximum_bytes || !sink)
      throw BridgeError("Invalid MCP input queue limits or sink");
    worker = std::thread([this] { run(); });
  }
  void emit(const JsonDoc &value) {
    std::lock_guard lock(output_mutex);
    sink(value);
  }
  void run() noexcept {
    try {
      while (true) {
        std::shared_ptr<Job> job;
        {
          std::unique_lock lock(mutex);
          available.wait(lock, [&] { return closing || !queue.empty(); });
          if (aborting || (closing && queue.empty()))
            return;
          job = std::move(queue.front());
          queue.pop_front();
        }
        std::optional<JsonDoc> response;
        {
          CancelScope scope(job->cancellation.get_token());
          try {
            cancellation_point();
            response = endpoint.receive(job->message);
            cancellation_point();
          } catch (const RequestAborted &) {
          }
        }
        {
          std::lock_guard lock(mutex);
          if (job->cancellation.stop_requested() || aborting)
            response.reset();
          if (!job->identity.empty())
            pending.erase(job->identity);
          --count;
          bytes -= job->bytes;
        }
        if (response)
          emit(*response);
      }
    } catch (...) {
      std::lock_guard lock(mutex);
      fault = std::current_exception();
      closing = true;
    }
  }
  void abort() noexcept {
    {
      std::lock_guard lock(mutex);
      aborting = closing = true;
      for (auto &[id, job] : pending) {
        (void)id;
        job->cancellation.request_stop();
      }
      for (auto &job : queue)
        job->cancellation.request_stop();
      queue.clear();
    }
    available.notify_all();
    if (worker.joinable())
      worker.join();
  }
  ~Engine() { abort(); }
};
RequestPump::RequestPump(StdioTransport::Handler handler, Sink sink,
                         bool compatibility, std::size_t maximum_requests,
                         std::size_t maximum_bytes)
    : engine_(std::make_unique<Engine>(std::move(handler), std::move(sink),
                                       compatibility, maximum_requests,
                                       maximum_bytes)) {}
RequestPump::~RequestPump() = default;
void RequestPump::check_failure() const {
  std::lock_guard lock(engine_->mutex);
  if (engine_->fault)
    std::rethrow_exception(engine_->fault);
}
void RequestPump::submit(const std::string &line) {
  check_failure();
  JsonDoc message;
  try {
    message = parse_message(line);
  } catch (const std::exception &) {
    engine_->emit(engine_->endpoint.parse_failure());
    return;
  }
  if (message.is_object() && !message.contains("id") &&
      message.value("method", JsonDoc()) == "notifications/cancelled") {
    const auto args = message.value("params", JsonDoc());
    if (message.value("jsonrpc", JsonDoc()) == "2.0" && args.is_object() &&
        args.contains("requestId") && identifier(args.at("requestId")) &&
        (!args.contains("reason") || args.at("reason").is_string())) {
      std::lock_guard lock(engine_->mutex);
      const auto found = engine_->pending.find(args.at("requestId").dump());
      if (found != engine_->pending.end() && found->second->cancellable)
        found->second->cancellation.request_stop();
    }
    return; // Malformed, unknown, completed and initialize cancellations are
            // ignored.
  }
  auto job = std::make_shared<Engine::Job>();
  job->message = std::move(message);
  job->bytes = line.size();
  if (job->message.is_object() && job->message.contains("id") &&
      identifier(job->message.at("id")))
    job->identity = job->message.at("id").dump();
  job->cancellable = !job->message.is_object() ||
                     job->message.value("method", JsonDoc()) != "initialize";
  std::optional<JsonDoc> refused;
  {
    std::lock_guard lock(engine_->mutex);
    if (engine_->closing)
      throw BridgeError("MCP input queue is closed");
    if (!job->identity.empty() && engine_->pending.contains(job->identity))
      refused = failure(job->message.at("id"), "Request ID is already pending");
    else if (engine_->count >= engine_->maximum_count ||
             job->bytes > engine_->maximum_bytes - engine_->bytes) {
      if (job->identity.empty())
        throw BridgeError("MCP notification queue exceeds size limit");
      refused = failure(job->message.at("id"),
                        "MCP pending input exceeds queue limit");
    } else {
      if (!job->identity.empty())
        engine_->pending.emplace(job->identity, job);
      ++engine_->count;
      engine_->bytes += job->bytes;
      engine_->queue.push_back(std::move(job));
    }
  }
  if (refused)
    engine_->emit(*refused);
  else
    engine_->available.notify_one();
}
void RequestPump::finish() {
  {
    std::lock_guard lock(engine_->mutex);
    engine_->closing = true;
  }
  engine_->available.notify_all();
  if (engine_->worker.joinable())
    engine_->worker.join();
  check_failure();
}
} // namespace pagepilot
