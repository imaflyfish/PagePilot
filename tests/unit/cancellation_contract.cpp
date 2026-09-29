#include "../support/checks.hpp"
#include <atomic>
#include <condition_variable>
#include <future>
#include <iostream>
#include <mutex>
#include <pagepilot/mcp_transport.hpp>
#include <pagepilot/tool_runtime.hpp>
#include <thread>
using namespace pagepilot;
using namespace pagepilot_tests;
struct Inbox {
  std::mutex mutex;
  std::condition_variable changed;
  std::vector<JsonDoc> messages;
  void accept(const JsonDoc &message) {
    std::lock_guard lock(mutex);
    messages.push_back(message);
    changed.notify_all();
  }
  std::optional<JsonDoc> wait(const JsonDoc &id,
                              MsDuration limit = MsDuration(1500)) {
    std::unique_lock lock(mutex);
    auto found = [&] {
      return std::find_if(messages.begin(), messages.end(),
                          [&](const auto &m) { return m.at("id") == id; });
    };
    if (!changed.wait_for(lock, limit,
                          [&] { return found() != messages.end(); }))
      return {};
    return *found();
  }
  std::size_t count(const JsonDoc &id) {
    std::lock_guard lock(mutex);
    return static_cast<std::size_t>(
        std::count_if(messages.begin(), messages.end(),
                      [&](const auto &m) { return m.at("id") == id; }));
  }
};
JsonDoc request(JsonDoc id, std::string method,
                JsonDoc params = JsonDoc::object()) {
  return {{"jsonrpc", "2.0"},
          {"id", std::move(id)},
          {"method", std::move(method)},
          {"params", std::move(params)}};
}
JsonDoc action(JsonDoc id, std::string name, JsonDoc args = JsonDoc::object()) {
  return request(std::move(id), "tools/call",
                 {{"name", std::move(name)}, {"arguments", std::move(args)}});
}
JsonDoc cancel(JsonDoc id) {
  return {{"jsonrpc", "2.0"},
          {"method", "notifications/cancelled"},
          {"params", {{"requestId", std::move(id)}}}};
}
JsonDoc initialize() {
  return request(
      "init", "initialize",
      {{"protocolVersion", "2025-11-25"},
       {"capabilities", JsonDoc::object()},
       {"clientInfo", {{"name", "queue-contract"}, {"version", "1"}}}});
}
void ready(RequestPump &queue, Inbox &inbox) {
  queue.submit(initialize().dump());
  if (!inbox.wait("init"))
    throw BridgeError("Initialization stalled");
  queue.submit(
      JsonDoc({{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}})
          .dump());
  queue.submit(request("ready", "ping").dump());
  if (!inbox.wait("ready"))
    throw BridgeError("Readiness stalled");
}
int main() {
  try {
    {
      std::stop_source source;
      source.request_stop();
      CancelScope scope(source.get_token());
      bool stopped = false;
      try {
        cancellation_point();
      } catch (const RequestAborted &) {
        stopped = true;
      }
      check(stopped, "already-cancelled scope rejects work");
      {
        CancelScope cleanup;
        bool allowed = true;
        try {
          cancellation_point();
        } catch (...) {
          allowed = false;
        }
        check(allowed, "bounded cleanup can mask cancellation");
      }
      stopped = false;
      try {
        cancellation_point();
      } catch (const RequestAborted &) {
        stopped = true;
      }
      check(stopped, "cleanup restores original cancellation scope");
    }
    cancellation_point();
    {
      Inbox inbox;
      std::promise<void> started;
      auto running = started.get_future();
      std::atomic<unsigned> mutations = 0;
      RequestPump queue(
          [&](const ToolInvocation &call) {
            if (call.operation == "wait") {
              started.set_value();
              interruptible_pause(MsDuration(60000));
            } else
              ++mutations;
            return JsonDoc{{"done", true}};
          },
          [&](const JsonDoc &message) { inbox.accept(message); });
      ready(queue, inbox);
      queue.submit(action(1, "page_wait", {{"ms", 60000}}).dump());
      check(running.wait_for(MsDuration(1500)) == std::future_status::ready,
            "execution worker starts real blocking operation");
      queue.submit(
          action("queued", "browser_debug", {{"enabled", true}}).dump());
      queue.submit(cancel("queued").dump());
      queue.submit(cancel("1").dump());
      queue.submit(cancel("unknown").dump());
      auto malformed = cancel(1);
      malformed["params"]["reason"] = false;
      queue.submit(malformed.dump());
      malformed = cancel(1);
      malformed["jsonrpc"] = "invalid";
      queue.submit(malformed.dump());
      malformed = cancel(true);
      queue.submit(malformed.dump());
      check(!inbox.wait(1, MsDuration(60)),
            "wrong-type, unknown and malformed cancellations do not stop "
            "current request");
      const auto begin = std::chrono::steady_clock::now();
      queue.submit(cancel(1).dump());
      queue.submit(
          action("after", "browser_debug", {{"enabled", false}}).dump());
      check(inbox.wait("after").has_value(),
            "following request runs after cancellation");
      check(std::chrono::steady_clock::now() - begin < MsDuration(1000),
            "long pause is interrupted promptly");
      queue.submit(cancel(1).dump());
      queue.submit(cancel("after").dump());
      queue.finish();
      check(inbox.count(1) == 0 && inbox.count("queued") == 0,
            "cancelled running and queued requests produce no response");
      check(mutations == 1, "cancelled queued mutation never reaches handler");
      check(inbox.count("after") == 1,
            "late and repeated cancellation cannot remove completed response");
    }
    {
      Inbox inbox;
      std::promise<void> blocked;
      auto signal = blocked.get_future();
      std::promise<void> release;
      auto gate = release.get_future();
      RequestPump queue(
          [](const ToolInvocation &) { return JsonDoc::object(); },
          [&](const JsonDoc &message) {
            if (message.at("id") == "gate") {
              blocked.set_value();
              gate.wait();
            }
            inbox.accept(message);
          });
      queue.submit(request("gate", "ping").dump());
      if (signal.wait_for(MsDuration(1500)) != std::future_status::ready) {
        release.set_value();
        throw BridgeError("Gate did not run");
      }
      queue.submit(initialize().dump());
      queue.submit(cancel("init").dump());
      release.set_value();
      queue.finish();
      check(inbox.wait("init")->contains("result"),
            "initialize cannot be cancelled while queued");
    }
    {
      Inbox inbox;
      std::promise<void> started;
      auto running = started.get_future();
      std::atomic<unsigned> mutations = 0;
      RequestPump queue(
          [&](const ToolInvocation &call) {
            if (call.operation == "wait") {
              started.set_value();
              interruptible_pause(MsDuration(60000));
            } else
              ++mutations;
            return JsonDoc{{"done", true}};
          },
          [&](const JsonDoc &m) { inbox.accept(m); }, false, 2, 2048);
      ready(queue, inbox);
      queue.submit(action("long", "page_wait", {{"ms", 60000}}).dump());
      if (running.wait_for(MsDuration(1500)) != std::future_status::ready)
        throw BridgeError("Limited queue did not start");
      queue.submit(action("long", "browser_debug", {{"enabled", true}}).dump());
      check(inbox.wait("long")->at("error").at("message") ==
                "Request ID is already pending",
            "duplicate pending ID is refused before dispatch");
      queue.submit(
          action("large", "browser_debug",
                 {{"enabled", true}, {"unused", std::string(3000, 'x')}})
              .dump());
      check(inbox.wait("large")->at("error").at("code") == -32001,
            "encoded pending input has a byte limit");
      queue.submit(
          action("second", "browser_debug", {{"enabled", false}}).dump());
      queue.submit(
          action("overflow", "browser_debug", {{"enabled", true}}).dump());
      check(inbox.wait("overflow")->at("error").at("code") == -32001,
            "pending request count has a limit");
      queue.submit(cancel("long").dump());
      queue.finish();
      check(mutations == 1 && inbox.count("second") == 1,
            "accepted queued operation survives input overflow refusals");
      check(inbox.count("long") == 1 && !inbox.wait("long")->contains("result"),
            "duplicate refusal does not produce a late result for cancelled "
            "original");
    }
    {
      Inbox inbox;
      std::promise<void> started;
      auto running = started.get_future();
      auto queue = std::make_unique<RequestPump>(
          [&](const ToolInvocation &) {
            started.set_value();
            interruptible_pause(MsDuration(60000));
            return JsonDoc::object();
          },
          [&](const JsonDoc &m) { inbox.accept(m); });
      ready(*queue, inbox);
      queue->submit(action("abandon", "page_wait", {{"ms", 60000}}).dump());
      if (running.wait_for(MsDuration(1500)) != std::future_status::ready)
        throw BridgeError("Abandoned request did not start");
      const auto begin = std::chrono::steady_clock::now();
      queue.reset();
      check(
          std::chrono::steady_clock::now() - begin < MsDuration(1000) &&
              inbox.count("abandon") == 0,
          "queue destruction cancels and joins active worker without response");
    }
    {
      Inbox inbox;
      std::vector<int> order;
      RequestPump queue(
          [&](const ToolInvocation &call) {
            order.push_back(call.arguments.at("ms"));
            return JsonDoc{{"order", order.size()}};
          },
          [&](const JsonDoc &m) { inbox.accept(m); });
      ready(queue, inbox);
      for (int i = 1; i <= 4; ++i)
        queue.submit(action(i, "page_wait", {{"ms", i}}).dump());
      queue.finish();
      check(order == std::vector<int>({1, 2, 3, 4}) && inbox.count(4) == 1,
            "ordinary EOF finish drains queued actions in original order");
    }
    {
      RequestPump queue(
          [](const ToolInvocation &) { return JsonDoc::object(); },
          [](const JsonDoc &) { throw BridgeError("owned output failure"); });
      queue.submit(request(1, "ping").dump());
      bool reported = false;
      try {
        queue.finish();
      } catch (const BridgeError &e) {
        reported = std::string(e.what()) == "owned output failure";
      }
      check(reported,
            "output failure reaches session owner without worker termination");
    }
    {
      std::stop_source source;
      std::promise<void> started;
      auto ready = started.get_future();
      ToolRuntime runtime(1);
      ToolCatalog catalog;
      std::jthread signal([&] {
        ready.wait();
        std::this_thread::sleep_for(MsDuration(30));
        source.request_stop();
      });
      auto compose = [&](const std::string &name, JsonDoc args) {
        return JsonDoc{{"tool", name}, {"args", std::move(args)}};
      };
      auto wait = compose("page_wait", {{"ms", 5000}});
      auto change = compose("browser_configure", {{"fast_timeout", 9999}});
      bool stopped = false;
      {
        CancelScope scope(source.get_token());
        started.set_value();
        try {
          runtime.invoke(catalog.resolve(
              "workflow_batch",
              {{"actions",
                JsonDoc::array(
                    {compose("workflow_retry",
                             {{"tool", "workflow_steps"},
                              {"args",
                               {{"steps", JsonDoc::array({wait, change})},
                                {"max_step_retries", 10}}},
                              {"max_retries", 10},
                              {"delay_ms", 0}}),
                     change})}}));
        } catch (const RequestAborted &) {
          stopped = true;
        }
      }
      check(stopped, "cancellation escapes nested steps, retry and batch "
                     "rather than becoming a retryable row failure");
      auto settings = runtime.invoke(
          catalog.resolve("browser_settings", JsonDoc::object()));
      check(settings.at("timeouts").at("fast") == 3000,
            "cancelled workflow does not execute subsequent mutations");
      auto stats =
          runtime.invoke(catalog.resolve("browser_metrics", JsonDoc::object()));
      check(stats.at("byTool").at("wait").at("count") == 1,
            "cancelled child is not retried by either wrapper");
    }
    std::cout << passed << " cancellation contract checks passed; " << failed
              << " failed\n";
    return failed ? 1 : 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
