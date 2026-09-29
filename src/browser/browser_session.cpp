#include <algorithm>
#include <charconv>
#include <embedded_resources.hpp>
#include <pagepilot/session.hpp>
#include <regex>
#include <thread>
namespace pagepilot {
namespace {
class PageContextLost : public BridgeError {
public:
  PageContextLost()
      : BridgeError("Page execution context changed while awaiting script; "
                    "prior side effects may have occurred") {}
};
MsDuration remaining(std::chrono::steady_clock::time_point deadline) {
  auto value = std::chrono::ceil<MsDuration>(deadline -
                                             std::chrono::steady_clock::now());
  if (value.count() < 1)
    throw DeadlineReached("browser operation timed out");
  return value;
}
} // namespace
BrowserSession::PageScope::PageScope(BrowserSession &browser)
    : browser_(browser), previous_(browser.bound_target_) {
  if (!previous_)
    browser_.bound_target_ = &target_;
}
BrowserSession::PageScope::~PageScope() { browser_.bound_target_ = previous_; }
BrowserSession::Deadline BrowserSession::exchange_deadline(Deadline limit) {
  const auto prior = deadline_;
  deadline_ = limit;
  return prior;
}
std::chrono::steady_clock::time_point
BrowserSession::bounded_deadline(MsDuration requested) const {
  cancellation_point();
  const auto now = std::chrono::steady_clock::now();
  if (deadline_ && now >= *deadline_)
    throw DeadlineReached("browser operation timed out");
  return deadline_ ? std::min(now + requested, *deadline_) : now + requested;
}
MsDuration BrowserSession::time_left(MsDuration requested) const {
  cancellation_point();
  return deadline_ ? std::min(requested, remaining(*deadline_)) : requested;
}
JsonDoc BrowserSession::send(const std::string &method,
                             const JsonDoc &parameters,
                             const std::string &session, MsDuration timeout,
                             std::function<void()> observe) {
  const auto owner = session;
  try {
    return channel_->call(method, parameters, owner, time_left(timeout),
                          [this, &observe] {
                            pump();
                            if (observe)
                              observe();
                          });
  } catch (const WireFailure &failure) {
    if (failure.code == -32001 && !owner.empty()) {
      documents_.erase(owner);
      session_roots_.erase(owner);
      session_pages_.erase(owner);
      std::erase_if(sessions_,
                    [&](const auto &entry) { return entry.second == owner; });
      std::erase_if(frame_sessions_,
                    [&](const auto &entry) { return entry.second == owner; });
    }
    throw;
  }
}
std::string BrowserSession::context_session() const {
  if (!frames_.empty())
    return frames_.back().session;
  const auto current = sessions_.find(current_);
  if (current == sessions_.end())
    throw BridgeError("There is no current execution session");
  return current->second;
}
void BrowserSession::release_object(const std::string &session,
                                    const std::string &identity) noexcept {
  CancelScope cleanup;
  try {
    if (connected())
      channel_->call("Runtime.releaseObject", {{"objectId", identity}}, session,
                     MsDuration(100));
  } catch (...) {
  }
}
void BrowserSession::release_input(const std::string &method,
                                   const JsonDoc &parameters) noexcept {
  CancelScope cleanup;
  try {
    const auto active = sessions_.find(current_);
    if (connected() && active != sessions_.end())
      channel_->call(method, parameters, active->second, MsDuration(100));
  } catch (...) {
  }
}
bool BrowserSession::connected() const {
  return channel_ && channel_->connected();
}
void BrowserSession::disconnect() {
  cancel_pointer();
  pointers_.clear();
  while (!input_watches_.empty())
    release_input_signal(input_watches_.begin()->first);
  channel_.reset();
  sessions_.clear();
  frame_sessions_.clear();
  session_roots_.clear();
  session_pages_.clear();
  dialog_rules_.clear();
  documents_.clear();
  targets_.clear();
  frames_.clear();
  requests_.clear();
  changed_.clear();
  current_.clear();
  context_selected_ = false;
}
void BrowserSession::connect() {
  if (connected())
    return;
  disconnect();
  try {
    channel_ = std::make_unique<CdpChannel>(port_, time_left(MsDuration(5000)));
    send("Target.setDiscoverTargets", {{"discover", true}});
    refresh();
    if (targets_.empty()) {
      const auto created =
          send("Target.createTarget", {{"url", "about:blank"}});
      current_ = created.at("targetId");
      refresh();
    }
    attach();
  } catch (...) {
    disconnect();
    throw;
  }
}
void BrowserSession::refresh() {
  if (!connected())
    throw BridgeError("browser is disconnected");
  pump();
  auto result = send("Target.getTargets");
  std::vector<JsonDoc> valid;
  for (const auto &target : result.at("targetInfos")) {
    if (target.value("type", std::string()) != "page")
      continue;
    const auto scope = target.value("browserContextId", std::string());
    if (!context_selected_) {
      context_ = scope;
      context_selected_ = true;
    }
    if (scope == context_)
      valid.push_back(target);
  }
  // Preserve tab indices while targets survive; newly seen tabs append.
  std::vector<JsonDoc> ordered;
  for (const auto &known : targets_)
    for (const auto &target : valid)
      if (known.at("targetId") == target.at("targetId"))
        ordered.push_back(target);
  for (const auto &target : valid)
    if (std::none_of(ordered.begin(), ordered.end(), [&](const auto &known) {
          return known.at("targetId") == target.at("targetId");
        }))
      ordered.push_back(target);
  targets_ = std::move(ordered);
  auto absent = [&](const std::string &page) {
    return std::none_of(
        targets_.begin(), targets_.end(),
        [&](const auto &target) { return target.at("targetId") == page; });
  };
  std::erase_if(pointers_,
                [&](const auto &entry) { return absent(entry.first); });
  std::erase_if(dialog_rules_,
                [&](const auto &entry) { return absent(entry.first); });
  std::erase_if(session_pages_,
                [&](const auto &entry) { return absent(entry.second); });
  if (std::none_of(targets_.begin(), targets_.end(), [&](const auto &target) {
        return target.at("targetId") == current_;
      })) {
    current_ = targets_.empty()
                   ? ""
                   : targets_.front().at("targetId").get<std::string>();
    frames_.clear();
  }
  for (auto iterator = sessions_.begin(); iterator != sessions_.end();) {
    if (std::none_of(targets_.begin(), targets_.end(), [&](const auto &target) {
          return target.at("targetId") == iterator->first;
        }))
      iterator = sessions_.erase(iterator);
    else
      ++iterator;
  }
}
void BrowserSession::attach() {
  if (current_.empty())
    throw BridgeError("there is no active browser tab");
  if (sessions_.contains(current_))
    return;
  auto attached = send("Target.attachToTarget",
                       {{"targetId", current_}, {"flatten", true}});
  const auto session = attached.at("sessionId").get<std::string>();
  sessions_[current_] = session;
  session_pages_[session] = current_;
  enable_or_discard(session);
}
DeadlineScope::DeadlineScope(BrowserSession &browser, MsDuration allowance)
    : browser_(browser),
      prior_(browser.exchange_deadline(browser.bounded_deadline(allowance))) {}
DeadlineScope::DeadlineScope(BrowserSession &browser,
                             BrowserSession::Deadline deadline)
    : browser_(browser), prior_(browser.exchange_deadline(deadline)) {}
DeadlineScope::~DeadlineScope() { browser_.exchange_deadline(prior_); }
void BrowserSession::enable_or_discard(const std::string &session) {
  try {
    enable_session(session);
  } catch (...) {
    discard_session(session);
    throw;
  }
}
void BrowserSession::discard_session(const std::string &session) noexcept {
  CancelScope cleanup;
  try {
    if (connected())
      channel_->call("Target.detachFromTarget", {{"sessionId", session}}, {},
                     MsDuration(100));
  } catch (...) {
  }
  std::erase_if(sessions_,
                [&](const auto &entry) { return entry.second == session; });
  std::erase_if(frame_sessions_,
                [&](const auto &entry) { return entry.second == session; });
  session_pages_.erase(session);
  session_roots_.erase(session);
  documents_.erase(session);
  requests_.erase(session);
  changed_.erase(session);
}
void BrowserSession::enable_session(const std::string &session) {
  for (const auto *method : {"Page.enable", "Runtime.enable", "Network.enable",
                             "DOM.enable", "Log.enable"})
    send(method, JsonDoc::object(), session);
  send("Page.setLifecycleEventsEnabled", {{"enabled", true}}, session);
  session_roots_[session] =
      send("Page.getFrameTree", JsonDoc::object(), session)
          .at("frameTree")
          .at("frame")
          .at("id");
  changed_[session] = std::chrono::steady_clock::now();
  pump();
}
std::string BrowserSession::current_session() {
  connect();
  refresh();
  if (bound_target_) {
    if (bound_target_->empty())
      *bound_target_ = current_;
    else if (*bound_target_ != current_)
      throw BridgeError(
          "Selected page closed during action; remaining commands "
          "cannot move to another tab");
  }
  attach();
  return sessions_.at(current_);
}
JsonDoc BrowserSession::browser_call(const std::string &method,
                                     const JsonDoc &parameters,
                                     MsDuration timeout) {
  connect();
  auto result = send(method, parameters, {}, timeout);
  pump();
  return result;
}
JsonDoc BrowserSession::page_call(const std::string &method,
                                  const JsonDoc &parameters,
                                  MsDuration timeout) {
  const auto session = current_session();
  auto result = send(method, parameters, session, timeout);
  pump();
  return result;
}
JsonDoc BrowserSession::evaluate(const std::string &expression,
                                 MsDuration timeout, bool by_value) {
  return run_script(expression, timeout, by_value, false);
}
bool BrowserSession::test_condition(const std::string &expression,
                                    MsDuration timeout) {
  const auto script = "Promise.resolve((" + expression +
                      ")).then(v=>typeof v==='function'?v():v).then(Boolean)";
  return run_script(script, timeout, true, true).get<bool>();
}
JsonDoc BrowserSession::run_script(const std::string &expression,
                                   MsDuration timeout, bool by_value,
                                   bool retry_replaced) {
  const auto end = bounded_deadline(timeout);
  std::string pinned_target;
  while (true) {
    std::string attempted_session, attempted_context, attempted_frame;
    try {
      timeout = std::min(MsDuration(maximum_timeout_ms), remaining(end));
      auto session = current_session();
      if (pinned_target.empty())
        pinned_target = current_;
      else if (pinned_target != current_)
        throw BridgeError("Selected page closed while awaiting script; "
                          "execution cannot move to another tab");
      prepare_frames();
      JsonDoc parameters = {{"expression", expression},
                            {"returnByValue", by_value},
                            {"awaitPromise", true},
                            {"userGesture", true},
                            {"timeout", timeout.count()}};
      if (!frames_.empty()) {
        session = frames_.back().session;
        attempted_context = frames_.back().unique_context;
        attempted_frame = frames_.back().frame;
      } else if (session_roots_.contains(session)) {
        attempted_frame = session_roots_.at(session);
        const auto known = documents_.find(session);
        if (known != documents_.end() &&
            known->second.contains(attempted_frame))
          attempted_context = known->second.at(attempted_frame)
                                  .value("uniqueId", std::string());
      }
      if (!attempted_context.empty())
        parameters["uniqueContextId"] = attempted_context;
      attempted_session = session;
      auto response =
          send("Runtime.evaluate", parameters, session, timeout, [&] {
            if (attempted_context.empty())
              return;
            const auto known = documents_.find(attempted_session);
            if (known == documents_.end() ||
                !known->second.contains(attempted_frame) ||
                known->second.at(attempted_frame)
                        .value("uniqueId", std::string()) != attempted_context)
              throw PageContextLost();
          });
      pump();
      if (response.contains("exceptionDetails")) {
        const auto &failure = response.at("exceptionDetails");
        throw BridgeError(
            "Page script failed: " +
            failure.value("exception", JsonDoc::object())
                .value(
                    "description",
                    failure.value("text", std::string("unknown exception"))));
      }
      const auto &result = response.at("result");
      if (!by_value)
        return result;
      if (result.contains("unserializableValue"))
        return {{"unserializable", result.at("unserializableValue")}};
      return result.value("value", JsonDoc(nullptr));
    } catch (const PageContextLost &) {
      // A condition is already a polling operation. Arbitrary evaluation must
      // report lost context without replaying a script that may have executed.
      if (!retry_replaced)
        throw;
      pump();
      interruptible_pause(std::min(remaining(end), MsDuration(2)));
    } catch (const WireFailure &failure) {
      if (retry_replaced && failure.code == -32000 &&
          (failure.description == "Inspected target navigated or closed" ||
           failure.description == "Execution context was destroyed.")) {
        pump();
        interruptible_pause(std::min(remaining(end), MsDuration(2)));
        continue;
      }
      // These two lookup failures occur before script execution. A document
      // can disappear between prepare_frames and Runtime.evaluate. Discard
      // only the rejected identity; a newly published replacement stays live.
      const bool missing_context =
          !attempted_context.empty() && failure.code == -32602 &&
          failure.description == "uniqueContextId not found";
      if (failure.code != -32001 && !missing_context)
        throw;
      if (missing_context) {
        auto &contexts = documents_[attempted_session];
        std::erase_if(contexts, [&](const auto &entry) {
          return entry.second.value("uniqueId", std::string()) ==
                 attempted_context;
        });
      }
      pump();
      interruptible_pause(std::min(remaining(end), MsDuration(2)));
    }
  }
}
JsonDoc BrowserSession::evaluate_legacy(std::string expression,
                                        MsDuration timeout) {
  const auto first = expression.find_first_not_of(" \t\r\n\f\v");
  expression = first == std::string::npos ? "" : expression.substr(first);
  // Anonymous function expressions need parentheses, but are never invoked.
  static const std::regex declaration(R"(^(async)?\s*function(\s|\())");
  if (std::regex_search(expression, declaration))
    expression = "(" + expression + ")";
  return evaluate(std::string(resources::legacy_value) + "(" +
                      JsonDoc(expression).dump() + ")",
                  timeout);
}
JsonDoc BrowserSession::context_call(const std::string &method,
                                     const JsonDoc &parameters,
                                     MsDuration timeout) {
  auto session = current_session();
  prepare_frames();
  if (!frames_.empty())
    session = frames_.back().session;
  auto result = send(method, parameters, session, timeout);
  pump();
  return result;
}
JsonDoc BrowserSession::session_call(const std::string &session,
                                     const std::string &method,
                                     const JsonDoc &parameters,
                                     MsDuration timeout) {
  if (!connected() || session.empty())
    throw BridgeError("Remote object session is no longer available");
  auto result = send(method, parameters, session, timeout);
  pump();
  return result;
}
void BrowserSession::pump() {
  cancellation_point();
  if (!channel_)
    return;
  for (const auto &event : channel_->drain_events()) {
    const auto method = event.at("method").get<std::string>();
    const auto session = event.value("sessionId", std::string());
    const auto parameters = event.value("params", JsonDoc::object());
    if (method == "Input.dragIntercepted") {
      if (session_pages_.contains(session)) {
        const auto page = session_pages_.at(session);
        if (pointers_.contains(page) && pointers_.at(page).intercept)
          pointers_.at(page).drag = parameters.at("data");
      }
    } else if (method == "Page.javascriptDialogOpening") {
      if (!session_pages_.contains(session))
        continue;
      const auto page = session_pages_.at(session);
      const auto rule = dialog_rules_.contains(page)
                            ? dialog_rules_.at(page)
                            : JsonDoc{{"accept", false}};
      dialog_rules_.erase(page);
      const auto route =
          sessions_.contains(page) ? sessions_.at(page) : session;
      try {
        channel_->call("Page.handleJavaScriptDialog", rule, route,
                       MsDuration(1000));
      } catch (const WireFailure &failure) {
        // Chrome may expose the same modal through both page and OOP sessions.
        if (failure.description != "No dialog is showing")
          throw;
      }
    } else if (method == "Runtime.bindingCalled") {
      const auto name = parameters.at("name").get<std::string>();
      if (input_watches_.contains(name)) {
        auto &watch = input_watches_.at(name);
        if (watch.session == session &&
            parameters.at("executionContextId") == watch.context) {
          const auto payload = parameters.at("payload").get<std::string>();
          if (payload == "received")
            watch.received = true;
          else if (payload == "settled") {
            watch.received = true;
            watch.settled = true;
          }
        }
      }
    } else if (method == "Runtime.consoleAPICalled") {
      JsonDoc values = JsonDoc::array();
      std::string text;
      bool first_argument = true;
      for (const auto &argument : parameters.value("args", JsonDoc::array())) {
        values.push_back(argument.value(
            "value", JsonDoc(argument.value("description", std::string()))));
        std::string part;
        if (argument.contains("value")) {
          const auto &value = argument.at("value");
          part = value.is_string() ? value.get<std::string>() : value.dump();
        } else if (argument.contains("unserializableValue")) {
          part = argument.at("unserializableValue").get<std::string>();
        } else if (argument.value("type", std::string()) == "undefined") {
          part = "undefined";
        } else if (argument.value("description", std::string()) == "Object" &&
                   argument.contains("preview")) {
          part = "{";
          for (const auto &field :
               argument.at("preview").value("properties", JsonDoc::array())) {
            if (part.size() > 1)
              part += ", ";
            part += field.value("name", std::string()) + ": " +
                    field.value("value", std::string("undefined"));
          }
          part += "}";
        } else if (argument.value("subtype", std::string()) == "array" &&
                   argument.contains("preview")) {
          std::map<std::uint64_t, std::string> entries;
          for (const auto &field :
               argument.at("preview").value("properties", JsonDoc::array())) {
            const auto key = field.value("name", std::string());
            std::uint64_t index = 0;
            const auto parsed =
                std::from_chars(key.data(), key.data() + key.size(), index);
            if (parsed.ec == std::errc() &&
                parsed.ptr == key.data() + key.size())
              entries[index] = field.value("value", std::string("undefined"));
          }
          part = "[";
          std::uint64_t next = 0;
          for (const auto &[index, value] : entries) {
            if (part.size() > 1)
              part += ", ";
            if (index > next) {
              const auto gap = index - next;
              part += gap == 1 ? "empty, "
                               : "empty x " + std::to_string(gap) + ", ";
            }
            part += value;
            next = index + 1;
          }
          part += "]";
        } else {
          part = argument.value("description", std::string());
        }
        if (!first_argument)
          text += ' ';
        first_argument = false;
        text += part;
      }
      console_.push_back({{"type", parameters.value("type", std::string())},
                          {"args", values},
                          {"text", text},
                          {"timestamp", parameters.value("timestamp", 0.0)},
                          {"session", session}});
      while (console_.size() > 100)
        console_.pop_front();
    } else if (method == "Log.entryAdded") {
      const auto &entry = parameters.at("entry");
      for (const auto &argument : entry.value("args", JsonDoc::array()))
        if (argument.contains("objectId"))
          release_object(session, argument.at("objectId").get<std::string>());
      if (entry.value("source", std::string()) != "worker") {
        console_.push_back({{"type", entry.value("level", std::string())},
                            {"args", JsonDoc::array()},
                            {"text", entry.value("text", std::string())},
                            {"timestamp", entry.value("timestamp", 0.0)},
                            {"session", session},
                            {"source", entry.value("source", std::string())}});
        while (console_.size() > 100)
          console_.pop_front();
      }
    } else if (method == "Network.requestWillBeSent") {
      requests_[session][parameters.at("requestId").get<std::string>()] = {
          parameters.value("frameId", std::string()),
          parameters.value("loaderId", std::string())};
      changed_[session] = std::chrono::steady_clock::now();
    } else if (method == "Network.loadingFinished" ||
               method == "Network.loadingFailed") {
      requests_[session].erase(parameters.at("requestId").get<std::string>());
      changed_[session] = std::chrono::steady_clock::now();
    } else if (method == "Page.frameNavigated") {
      const auto &frame = parameters.at("frame");
      const auto identity = frame.at("id").get<std::string>();
      const auto loader = frame.at("loaderId").get<std::string>();
      const bool root = session_roots_.contains(session) &&
                        session_roots_.at(session) == identity;
      // Chrome may never send loadingFinished for an unread fetch body in a
      // discarded document. It cannot keep the replacement document busy.
      std::erase_if(requests_[session], [&](const auto &request) {
        return (root || request.second.frame == identity) &&
               request.second.loader != loader;
      });
      changed_[session] = std::chrono::steady_clock::now();
    } else if (method == "Runtime.executionContextCreated") {
      const auto context = parameters.at("context");
      const auto auxiliary = context.value("auxData", JsonDoc::object());
      if (auxiliary.value("isDefault", false) && auxiliary.contains("frameId"))
        documents_[session][auxiliary.at("frameId").get<std::string>()] =
            context;
    } else if (method == "Runtime.executionContextDestroyed") {
      const auto identity =
          parameters.value("executionContextId", std::int64_t{-1});
      const auto unique =
          parameters.value("executionContextUniqueId", std::string());
      std::erase_if(documents_[session], [&](const auto &entry) {
        return entry.second.at("id") == identity ||
               (!unique.empty() &&
                entry.second.value("uniqueId", std::string()) == unique);
      });
    } else if (method == "Runtime.executionContextsCleared") {
      documents_.erase(session);
    } else if (method == "Page.frameDetached") {
      const auto frame = parameters.at("frameId").get<std::string>();
      documents_[session].erase(frame);
      std::erase_if(requests_[session], [&](const auto &request) {
        return request.second.frame == frame;
      });
    } else if (method == "Target.detachedFromTarget") {
      const auto detached = parameters.value("sessionId", std::string());
      std::erase_if(sessions_,
                    [&](const auto &item) { return item.second == detached; });
      std::erase_if(frame_sessions_,
                    [&](const auto &item) { return item.second == detached; });
      documents_.erase(detached);
      session_roots_.erase(detached);
      session_pages_.erase(detached);
      requests_.erase(detached);
      changed_.erase(detached);
    } else if (method == "Target.targetDestroyed") {
      const auto target = parameters.at("targetId").get<std::string>();
      dialog_rules_.erase(target);
      if (frame_sessions_.contains(target)) {
        const auto ended = frame_sessions_.at(target);
        documents_.erase(ended);
        session_roots_.erase(ended);
        session_pages_.erase(ended);
        frame_sessions_.erase(target);
      }
    }
  }
}
JsonDoc BrowserSession::tabs() {
  connect();
  refresh();
  JsonDoc rows = JsonDoc::array();
  for (std::size_t i = 0; i < targets_.size(); ++i)
    rows.push_back({{"index", i},
                    {"id", targets_[i].at("targetId")},
                    {"url", targets_[i].at("url")},
                    {"title", targets_[i].at("title")},
                    {"active", targets_[i].at("targetId") == current_},
                    {"current", targets_[i].at("targetId") == current_}});
  return {{"tabs", rows}, {"count", rows.size()}};
}
JsonDoc BrowserSession::status() {
  if (!connected())
    return {{"connected", false}, {"hasPage", false}, {"url", nullptr},
            {"title", nullptr},   {"inFrame", false}, {"tabCount", 0},
            {"frameDepth", 0}};
  const auto listing = tabs();
  JsonDoc result = {{"connected", true},
                    {"hasPage", !current_.empty()},
                    {"url", nullptr},
                    {"title", nullptr},
                    {"tabCount", listing.at("count")},
                    {"inFrame", !frames_.empty()},
                    {"frameDepth", frames_.size()}};
  for (const auto &target : targets_)
    if (target.at("targetId") == current_) {
      result["url"] = target.at("url");
      result["title"] = target.at("title");
    }
  return result;
}
JsonDoc BrowserSession::create_tab(const std::string &url) {
  connect();
  JsonDoc parameters = context_parameters();
  parameters["url"] = url;
  auto created = send("Target.createTarget", parameters);
  current_ = created.at("targetId");
  frames_.clear();
  refresh();
  attach();
  const auto position =
      std::find_if(targets_.begin(), targets_.end(), [&](const auto &target) {
        return target.at("targetId") == current_;
      });
  if (position == targets_.end())
    throw BridgeError("new browser tab disappeared before attachment");
  return {{"created", true},
          {"index", std::distance(targets_.begin(), position)},
          {"target", current_},
          {"url", url}};
}
JsonDoc BrowserSession::context_parameters() {
  if (context_.empty())
    return JsonDoc::object();
  const auto inventory = send("Target.getBrowserContexts");
  // Some Chrome versions expose the default profile's internal ID in target
  // metadata but reject that ID in Storage commands. Omit it only when Chrome
  // explicitly identifies it as the default. Missing/disposed private contexts
  // retain their ID and fail, rather than gaining access to the default
  // profile.
  if (inventory.value("defaultBrowserContextId", std::string()) == context_)
    return JsonDoc::object();
  return {{"browserContextId", context_}};
}
JsonDoc BrowserSession::activate_tab(std::size_t index) {
  connect();
  refresh();
  if (index >= targets_.size())
    throw BridgeError("tab index is outside current tab list");
  current_ = targets_[index].at("targetId");
  frames_.clear();
  attach();
  send("Target.activateTarget", {{"targetId", current_}});
  return {{"switched", index},
          {"target", current_},
          {"url", targets_[index].at("url")}};
}
JsonDoc BrowserSession::close_tab(std::optional<std::size_t> index) {
  connect();
  refresh();
  std::string target = current_;
  if (index) {
    if (*index >= targets_.size())
      throw BridgeError("tab index is outside current tab list");
    target = targets_[*index].at("targetId");
  }
  if (target.empty())
    throw BridgeError("no tab to close");
  const auto end = bounded_deadline(MsDuration(10000));
  DeadlineScope window(*this, end);
  auto response = send("Target.closeTarget", {{"targetId", target}});
  // Chrome may acknowledge the request before removing the target. Report the
  // completed inventory, not the transient list immediately after the reply.
  while (true) {
    refresh();
    if (!response.value("success", false) ||
        std::none_of(targets_.begin(), targets_.end(), [&](const auto &entry) {
          return entry.at("targetId") == target;
        }))
      break;
    interruptible_pause(std::min(remaining(end), MsDuration(2)));
  }
  return {{"closed", response.value("success", false)},
          {"target", target},
          {"remaining", targets_.size()}};
}
void BrowserSession::wait_ready(const std::string &readiness,
                                MsDuration timeout) {
  if (readiness != "load" && readiness != "domcontentloaded" &&
      readiness != "networkidle")
    throw BridgeError("unknown page readiness state");
  const auto deadline = bounded_deadline(timeout);
  DeadlineScope window(*this, deadline);
  const auto session = current_session();
  const auto target = current_;
  while (true) {
    pump();
    current_session();
    if (current_ != target)
      throw BridgeError("Selected page closed while awaiting readiness");
    const auto time = remaining(deadline);
    try {
      auto state =
          evaluate("document.readyState", std::min(time, MsDuration(1000)));
      const bool loaded = readiness == "domcontentloaded"
                              ? state == "interactive" || state == "complete"
                              : state == "complete";
      if (current_ != target)
        throw BridgeError("Selected page closed while awaiting readiness");
      if (loaded && (readiness != "networkidle" ||
                     (requests_[session].empty() &&
                      std::chrono::steady_clock::now() - changed_[session] >=
                          MsDuration(500))))
        return;
    } catch (const BridgeError &) {
      if (!connected() || current_ != target)
        throw;
    }
    interruptible_pause(std::min(remaining(deadline), MsDuration(20)));
  }
}
JsonDoc BrowserSession::navigate(const std::string &url,
                                 const std::string &readiness,
                                 MsDuration timeout) {
  if (readiness != "load" && readiness != "domcontentloaded" &&
      readiness != "networkidle")
    throw BridgeError("unknown page readiness state");
  frames_.clear();
  const auto deadline = bounded_deadline(timeout);
  DeadlineScope window(*this, deadline);
  const auto session = current_session();
  const auto previous =
      send("Page.getFrameTree", JsonDoc::object(), session, remaining(deadline))
          .at("frameTree")
          .at("frame");
  auto response =
      send("Page.navigate", {{"url", url}}, session, remaining(deadline));
  if (response.contains("errorText"))
    throw BridgeError("Navigation failed: " +
                      response.at("errorText").get<std::string>());
  if (response.value("isDownload", false))
    throw BridgeError(
        "Navigation started a download without committing a page");
  // Same-document navigation has no new loader; bind its probe to the old
  // document. A normal navigation must observe the exact returned loader.
  const auto page = await_navigation(
      session,
      response.value("loaderId", previous.at("loaderId").get<std::string>()),
      {}, {}, readiness, deadline);
  return {{"navigated", url},
          {"finalUrl", page.at("url")},
          {"title", page.at("title")},
          {"redirected", page.at("url") != url},
          {"inFrame", false}};
}
JsonDoc BrowserSession::reload(MsDuration timeout) {
  frames_.clear();
  const auto deadline = bounded_deadline(timeout);
  DeadlineScope window(*this, deadline);
  const auto session = current_session();
  const auto previous =
      send("Page.getFrameTree", JsonDoc::object(), session, remaining(deadline))
          .at("frameTree")
          .at("frame")
          .at("loaderId")
          .get<std::string>();
  send("Page.reload", {{"loaderId", previous}}, session, remaining(deadline));
  const auto page =
      await_navigation(session, {}, previous, {}, "load", deadline);
  return {{"reloaded", true}, {"url", page.at("url")}};
}
JsonDoc BrowserSession::history(int direction, MsDuration timeout) {
  frames_.clear();
  const auto deadline = bounded_deadline(timeout);
  DeadlineScope window(*this, deadline);
  const auto session = current_session();
  const auto record = send("Page.getNavigationHistory", JsonDoc::object(),
                           session, remaining(deadline));
  const auto index = record.at("currentIndex").get<int>() + direction;
  if (index < 0 || index >= static_cast<int>(record.at("entries").size()))
    return {{"moved", false},
            {"url", evaluate("location.href", remaining(deadline))}};
  const auto desired =
      record.at("entries")[static_cast<std::size_t>(index)].at("id").get<int>();
  send("Page.navigateToHistoryEntry", {{"entryId", desired}}, session,
       remaining(deadline));
  const auto page =
      await_navigation(session, {}, {}, desired, "load", deadline);
  return {{"moved", true}, {"url", page.at("url")}};
}
JsonDoc BrowserSession::await_navigation(
    const std::string &session, const std::string &expected_loader,
    const std::string &replaced_loader, std::optional<int> history_entry,
    const std::string &readiness,
    std::chrono::steady_clock::time_point deadline) {
  while (true) {
    pump();
    const auto frame = send("Page.getFrameTree", JsonDoc::object(), session,
                            remaining(deadline))
                           .at("frameTree")
                           .at("frame");
    const auto loader = frame.at("loaderId").get<std::string>();
    bool committed = (expected_loader.empty() || loader == expected_loader) &&
                     (replaced_loader.empty() || loader != replaced_loader);
    if (history_entry) {
      const auto history = send("Page.getNavigationHistory", JsonDoc::object(),
                                session, remaining(deadline));
      const auto position = history.at("currentIndex").get<int>();
      committed =
          committed && position >= 0 &&
          static_cast<std::size_t>(position) < history.at("entries").size() &&
          history.at("entries")[static_cast<std::size_t>(position)].at("id") ==
              *history_entry;
    }
    pump();
    const auto identity = frame.at("id").get<std::string>();
    if (committed && documents_[session].contains(identity)) {
      const auto unique =
          documents_[session].at(identity).value("uniqueId", std::string());
      if (!unique.empty()) {
        try {
          const auto probe =
              send("Runtime.evaluate",
                   {{"expression", "({state:document.readyState,url:location."
                                   "href,title:document.title})"},
                    {"uniqueContextId", unique},
                    {"returnByValue", true}},
                   session, remaining(deadline));
          if (probe.contains("exceptionDetails"))
            throw BridgeError("Cannot inspect the navigation document");
          const auto value = probe.at("result").at("value");
          const bool ready = readiness == "domcontentloaded"
                                 ? value.at("state") == "interactive" ||
                                       value.at("state") == "complete"
                                 : value.at("state") == "complete";
          // Verify the probe still belongs to this committed document. A
          // concurrent navigation must not turn an old readyState into success.
          const auto after = send("Page.getFrameTree", JsonDoc::object(),
                                  session, remaining(deadline))
                                 .at("frameTree")
                                 .at("frame");
          pump();
          bool quiet = true;
          if (readiness == "networkidle") {
            const auto now = std::chrono::steady_clock::now();
            const auto page = session_pages_.at(session);
            for (const auto &[route, owner] : session_pages_)
              if (owner == page && (!requests_[route].empty() ||
                                    now - changed_[route] < MsDuration(500)))
                quiet = false;
          }
          if (ready && quiet && after.at("loaderId") == loader &&
              after.at("id") == identity &&
              documents_[session].contains(identity) &&
              documents_[session].at(identity).value("uniqueId",
                                                     std::string()) == unique)
            return value;
        } catch (const WireFailure &failure) {
          // This read-only probe may race a context replacement. No page
          // action is repeated; only the next bounded observation is retried.
          if (!(failure.code == -32602 &&
                failure.description == "uniqueContextId not found") &&
              !(failure.code == -32000 &&
                failure.description == "Cannot find context with specified id"))
            throw;
        }
      }
    }
    interruptible_pause(std::min(remaining(deadline), MsDuration(20)));
  }
}
JsonDoc BrowserSession::console_messages(std::size_t maximum, bool clear) {
  pump();
  JsonDoc values = JsonDoc::array();
  const auto total = console_.size();
  const auto count = std::min(maximum, console_.size());
  for (std::size_t i = console_.size() - count; i < console_.size(); ++i)
    values.push_back(console_[i]);
  if (clear)
    console_.clear();
  return {{"logs", values}, {"count", values.size()}, {"total", total}};
}
} // namespace pagepilot
