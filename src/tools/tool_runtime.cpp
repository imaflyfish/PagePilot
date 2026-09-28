#include <pagepilot/node_tools.hpp>
#include <pagepilot/tool_runtime.hpp>
#include <pagepilot/step_sequence.hpp>
#include <array>
#include <cmath>
#include <iostream>
#include <thread>
namespace pagepilot {
namespace {
class RuntimeDeadlineGuard {
public:
  RuntimeDeadlineGuard(BrowserSession &browser, MsDuration limit)
      : browser_(browser) {
    const auto until = browser.bounded_deadline(limit);
    prior_ = browser_.exchange_deadline(until);
  }
  ~RuntimeDeadlineGuard() { browser_.exchange_deadline(prior_); }

private:
  BrowserSession &browser_;
  BrowserSession::Deadline prior_;
};
} // namespace
MsDuration ToolRuntime::deadline(const JsonDoc &arguments,
                                     int fallback) const {
  const auto value = arguments.value("timeout", double(fallback));
  if (!std::isfinite(value) || value < 1 || value > 60000)
    throw BridgeError("timeout must be between 1 and 60000 milliseconds");
  return MsDuration(static_cast<int>(value));
}
MsDuration ToolRuntime::allowance(const std::string &operation,
                                      const JsonDoc &arguments) const {
  if (StepSequence::supports(operation)) {
    const double duration = arguments.value("timeout", 60000.0);
    if (!std::isfinite(duration) || duration < 1 || duration > 600000)
      throw BridgeError(
          "Workflow timeout must be between 1 and 600000 milliseconds");
    return MsDuration(static_cast<int>(duration));
  }
  if (operation == "wait" &&
      (arguments.contains("ms") ||
       arguments.value("type", std::string()) == "time" ||
       (!arguments.contains("type") && !arguments.contains("selector") &&
        !arguments.contains("text")))) {
    const auto duration =
        arguments.value("ms", arguments.value("timeout", 1000.0));
    if (!std::isfinite(duration) || duration < 0 || duration > 60000)
      throw BridgeError(
          "Wait duration must be between 0 and 60000 milliseconds");
    return MsDuration(static_cast<int>(duration) + 100);
  }
  int fallback =
      DomTools::supports(operation) && operation != "wait" ? quick_ : normal_;
  if (operation == "navigate" || operation == "reload" ||
      operation == "go_back" || operation == "go_forward" ||
      operation == "screenshot" || operation == "upload_file" ||
      operation == "new_tab" || operation == "eval")
    fallback = long_;
  return deadline(arguments, fallback);
}
JsonDoc ToolRuntime::invoke(const ToolInvocation &invocation) {
  cancellation_point();
  if (!invocation_depth_)
    dispatched_ = 0;
  if (++dispatched_ > 1024)
    throw ActionLimitReached(
        "Workflow exceeds 1024 dispatches; earlier actions may have completed");
  if (StepSequence::supports(invocation.operation) && invocation_depth_ > 5)
    throw ActionLimitReached(
        "Workflow is nested too deeply (maximum 5 nested levels)");
  struct ActiveCall {
    unsigned &depth;
    explicit ActiveCall(unsigned &value) : depth(value) { ++depth; }
    ~ActiveCall() { --depth; }
  } active(invocation_depth_);
  const auto begin = std::chrono::steady_clock::now();
  bool success = false;
  auto finish = [&] {
    auto &metric = metrics_[invocation.operation];
    ++metric.count;
    if (!success)
      ++metric.failures;
    metric.duration +=
        static_cast<std::uint64_t>(std::chrono::duration_cast<MsDuration>(
                                       std::chrono::steady_clock::now() - begin)
                                       .count());
    if (diagnostic_)
      std::cerr << JsonDoc({{"component", "PagePilot"},
                         {"operation", invocation.operation},
                         {"success", success},
                         {"elapsed_ms",
                          std::chrono::duration_cast<MsDuration>(
                              std::chrono::steady_clock::now() - begin)
                              .count()}})
                       .dump()
                << '\n';
  };
  try {
    RuntimeDeadlineGuard window(browser_,
                         allowance(invocation.operation, invocation.arguments));
    auto result = execute(invocation);
    cancellation_point();
    success = true;
    finish();
    return result;
  } catch (const RequestAborted &) {
    if (invocation_depth_ == 1)
      browser_.cancel_pointer();
    finish();
    throw;
  } catch (...) {
    finish();
    throw;
  }
}
JsonDoc ToolRuntime::execute(const ToolInvocation &invocation) {
  const auto &operation = invocation.operation;
  const auto &arguments = invocation.arguments;
  if (StepSequence::supports(operation)) {
    StepSequence sequence(
        browser_, catalog_, [this](const auto &child) { return invoke(child); },
        invocation.allow_legacy);
    return sequence.run(invocation);
  }
  if (operation == "upload_file" || operation == "screenshot") {
    FileTools files(browser_, paths_,
                      browser_.time_left(allowance(operation, arguments)));
    return operation == "upload_file" ? files.upload(arguments)
                                      : files.screenshot(arguments);
  }
  if (DomTools::supports(operation)) {
    DomTools actions(browser_,
                       browser_.time_left(allowance(operation, arguments)));
    return actions.execute(operation, arguments);
  }
  if (operation == "status" || operation == "health_check") {
    std::string failure;
    try {
      browser_.connect();
    } catch (const BridgeError &error) {
      failure = error.what();
    }
    auto result = browser_.status();
    if (!failure.empty())
      result["connectionError"] = failure;
    if (operation == "health_check")
      return {{"browser", result.at("connected")},
              {"page", result.at("hasPage")},
              {"tabs", result.at("tabCount")},
              {"healthy", result.at("connected").get<bool>() &&
                              result.at("hasPage").get<bool>()}};
    return result;
  }
  if (operation == "get_config")
    return {
        {"timeouts", {{"fast", quick_}, {"default", normal_}, {"long", long_}}},
        {"cdpPort", port_},
        {"debug", diagnostic_},
        {"inFrame", !browser_.frames().empty()}};
  if (operation == "set_config") {
    auto quick = quick_, normal = normal_, long_value = long_;
    JsonDoc changes = JsonDoc::array();
    for (const auto &[key, target] :
         std::array<std::pair<const char *, int *>, 3>{{
             {"fast_timeout", &quick}, {"default_timeout", &normal},
             {"long_timeout", &long_value}}})
      if (arguments.contains(key)) {
        const auto value = arguments.at(key).get<double>();
        if (value < 1 || value > 60000)
          throw BridgeError("configured timeouts must be between 1 and 60000");
        *target = static_cast<int>(value);
        changes.push_back(key);
      }
    quick_ = quick;
    normal_ = normal;
    long_ = long_value;
    return {
        {"updated", !changes.empty()},
        {"changes", changes},
        {"current", {{"fast", quick_}, {"default", normal_}, {"long", long_}}}};
  }
  if (operation == "set_debug") {
    diagnostic_ = arguments.at("enabled");
    return {{"debug", diagnostic_}};
  }
  if (operation == "request_stats") {
    Metric total;
    JsonDoc rows = JsonDoc::object();
    for (const auto &[name, metric] : metrics_) {
      total.count += metric.count;
      total.failures += metric.failures;
      total.duration += metric.duration;
      rows[name] = {
          {"count", metric.count},
          {"errors", metric.failures},
          {"avgTime", metric.count ? metric.duration / metric.count : 0}};
    }
    return {{"total", total.count},
            {"success", total.count - total.failures},
            {"errors", total.failures},
            {"avgTime", total.count ? total.duration / total.count : 0},
            {"byTool", rows}};
  }
  if (operation == "cleanup") {
    const auto prior = execute({"request_stats", JsonDoc::object()});
    const auto logs = browser_.console_messages(0, true);
    metrics_.clear();
    if (invocation.legacy_name)
      return {{"cleaned", true},
              {"before", {{"consoleLogs", logs.at("total")},
                          {"requestStats", prior.at("total")}}},
              {"after", {{"consoleLogs", 0}, {"requestStats", 0}}}};
    return {{"cleaned", true}, {"before", prior}};
  }
  if (operation == "reconnect") {
    browser_.disconnect();
    browser_.connect();
    return {{"reconnected", true}, {"tabCount", browser_.tabs().at("count")}};
  }
  if (operation == "list_tabs")
    return browser_.tabs();
  if (operation == "new_tab") {
    if (!invocation.legacy_name)
      return browser_.create_tab(
          arguments.value("url", std::string("about:blank")));
    auto result = browser_.create_tab();
    if (arguments.contains("url"))
      result["url"] = browser_.navigate(arguments.at("url"), "load",
                                        deadline(arguments, long_)).at("finalUrl");
    return result;
  }
  if (operation == "switch_tab")
    return browser_.activate_tab(arguments.at("index").get<std::size_t>());
  if (operation == "close_tab") {
    if (invocation.legacy_name) {
      const auto count = browser_.tabs().at("count").get<std::size_t>();
      if (!count)
        throw BridgeError("no tab to close");
      const auto index = arguments.value("index", count - 1);
      auto result = browser_.close_tab(index);
      if (!result.at("closed").get<bool>())
        throw BridgeError("browser rejected closing the tab");
      result["closed"] = index;
      return result;
    }
    return browser_.close_tab(
        arguments.contains("index")
            ? std::optional<std::size_t>(
                  arguments.at("index").get<std::size_t>())
            : std::nullopt);
  }
  if (operation == "navigate") {
    auto result = browser_.navigate(arguments.at("url"),
                             arguments.value("wait_until", std::string("load")),
                             deadline(arguments, long_));
    if (invocation.legacy_name)
      result["hasDialog"] = browser_.evaluate(
          R"JS((()=>{const e=document.querySelector('[role="dialog"],[class*="modal"],[class*="dialog"],[class*="popup"]');if(!e)return false;const r=e.getBoundingClientRect();return r.width>0&&r.height>0&&getComputedStyle(e).visibility!=='hidden'})())JS");
    return result;
  }
  if (operation == "reload")
    return browser_.reload(deadline(arguments, long_));
  if (operation == "go_back" || operation == "go_forward")
    return browser_.history(operation == "go_back" ? -1 : 1,
                            deadline(arguments, long_));
  if (operation == "stop_loading") {
    if (invocation.legacy_name)
      browser_.evaluate("window.stop()");
    else
      browser_.page_call("Page.stopLoading");
    return {{"stopped", true}};
  }
  if (operation == "eval")
    return {{"result", invocation.legacy_name
                           ? browser_.evaluate_legacy(arguments.at("script"),
                                                      deadline(arguments, long_))
                           : browser_.evaluate(arguments.at("script"),
                                                deadline(arguments, long_))}};
  if (operation == "cookies")
    return browser_.manage_cookies(arguments);
  if (operation == "storage")
    return browser_.manage_storage(arguments);
  if (operation == "dialog")
    return browser_.arm_dialog(arguments);
  if (operation == "snapshot")
    return browser_.accessibility_snapshot();
  if (operation == "console_logs")
    return browser_.console_messages(arguments.value("limit", std::size_t{50}),
                                     arguments.value("clear", false));
  if (operation == "get_page") {
    const auto kind = arguments.value("type", std::string("info"));
    if (invocation.legacy_name && kind != "source" && kind != "text") {
      browser_.connect();
      const auto state = browser_.status();
      if (kind == "url" || kind == "title")
        return {{kind, state.at(kind)}};
      if (kind == "viewport")
        return {{"viewport", nullptr}};
      if (kind == "info")
        return {{"url", state.at("url")}, {"title", state.at("title")},
                {"viewport", nullptr},
                {"inFrame", !browser_.frames().empty()}};
      throw BridgeError("unknown page read type");
    }
    if (kind == "url")
      return {{"url", browser_.evaluate("location.href")}};
    if (kind == "title")
      return {{"title", browser_.evaluate("document.title")}};
    if (kind == "source")
      return {
          {"source", browser_.evaluate(
                         "((document.doctype?new XMLSerializer().serializeToString(document.doctype):'')+document.documentElement.outerHTML).slice(0,50000)")}};
    if (kind == "text")
      return {{"text", browser_.evaluate(
                           "(document.body?.innerText || '').slice(0,20000)")}};
    if (kind == "viewport")
      return {{"viewport",
               browser_.evaluate("({width:innerWidth,height:innerHeight})")}};
    if (kind == "info") {
      auto result =
          browser_.evaluate("({url:location.href,title:document.title,viewport:"
                            "{width:innerWidth,height:innerHeight}})");
      result["inFrame"] = !browser_.frames().empty();
      return result;
    }
    throw BridgeError("unknown page read type");
  }
  if (operation == "exit_frame") {
    if (browser_.frames().empty())
      return {{"exited", false}, {"depth", 0}, {"reason", "Not in iframe"}};
    browser_.frames().pop_back();
    return {{"exited", true}, {"depth", browser_.frames().size()}};
  }
  if (operation == "enter_frame") {
    DomTools elements(browser_,
                        browser_.time_left(deadline(arguments, normal_)));
    auto owner = elements.locate({{"selector", arguments.at("selector")}});
    auto result = browser_.enter_frame(
        owner.identity(), browser_.time_left(deadline(arguments, normal_)));
    result["entered"] = arguments.at("selector");
    return result;
  }
  if (operation == "list_frames")
    return browser_.list_frames();
  if (operation == "exit_all_frames") {
    const auto count = browser_.frames().size();
    browser_.frames().clear();
    return {{"exited", count}};
  }
  throw BridgeError("Action implementation is not yet available: " + operation);
}
} // namespace pagepilot
