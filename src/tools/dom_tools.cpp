#include "embedded_resources.hpp"
#include "input_ack.hpp"
#include <algorithm>
#include <array>
#include <pagepilot/node_tools.hpp>
#include <cmath>
#include <limits>
#include <regex>
#include <set>
#include <thread>
namespace pagepilot {
namespace {
std::string script(const JsonDoc &arguments) {
  return std::string("(") + resources::dom + ").run(" + arguments.dump() + ")";
}
JsonDoc remote_value(const JsonDoc &response) {
  if (response.contains("exceptionDetails")) {
    const auto &error = response.at("exceptionDetails");
    throw BridgeError(
        error.value("exception", JsonDoc::object())
            .value("description",
                   error.value("text", std::string("DOM operation failed"))));
  }
  return response.at("result").value("value", JsonDoc(nullptr));
}
void require(const JsonDoc &arguments, const std::string &key) {
  if (!arguments.contains(key) || arguments.at(key).is_null() ||
      (arguments.at(key).is_string() &&
       arguments.at(key).get_ref<const std::string &>().empty()))
    throw BridgeError("Missing required argument: " + key);
}
JsonDoc target(const JsonDoc &arguments) {
  JsonDoc result = arguments;
  if (arguments.contains("text") && !arguments.contains("selector"))
    result["targetText"] = true;
  return result;
}
std::vector<std::string> characters(const std::string &text) {
  std::vector<std::string> result;
  for (std::size_t i = 0; i < text.size();) {
    const auto first = static_cast<unsigned char>(text[i]);
    const std::size_t width = first < 0x80   ? 1
                              : first < 0xe0 ? 2
                              : first < 0xf0 ? 3
                                             : 4;
    if (i + width > text.size())
      throw BridgeError("Invalid UTF-8 input");
    result.push_back(text.substr(i, width));
    i += width;
  }
  return result;
}
} // namespace
StepClock::StepClock(MsDuration allowance)
    : end_(std::chrono::steady_clock::now() + allowance) {}
MsDuration StepClock::remaining() const {
  cancellation_point();
  const auto left = std::chrono::duration_cast<MsDuration>(
      end_ - std::chrono::steady_clock::now());
  if (left.count() < 1)
    throw DeadlineReached("Action deadline exceeded");
  return std::min(left, MsDuration(60000));
}
void StepClock::pause(MsDuration duration) const {
  if (duration.count() < 0 ||
      std::chrono::steady_clock::now() + duration > end_)
    throw DeadlineReached(
        "Requested delay exceeds the remaining action deadline");
  interruptible_pause(duration);
}
NodeLease::NodeLease(BrowserSession &browser, std::string identity)
    : browser_(&browser), identity_(std::move(identity)),
      session_(browser.context_session()) {}
NodeLease::NodeLease(NodeLease &&other) noexcept
    : browser_(other.browser_), identity_(std::move(other.identity_)),
      session_(std::move(other.session_)) {
  other.browser_ = nullptr;
}
NodeLease::~NodeLease() {
  if (browser_ && !identity_.empty()) {
    browser_->release_object(session_, identity_);
  }
}
JsonDoc NodeLease::call(const JsonDoc &arguments, MsDuration timeout) {
  const auto function =
      std::string("function(a){return (") + resources::dom + ").on(this,a)}";
  return remote_value(browser_->session_call(
      session_, "Runtime.callFunctionOn",
      {{"objectId", identity_},
       {"functionDeclaration", function},
       {"arguments", JsonDoc::array({{{"value", arguments}}})},
       {"returnByValue", true},
       {"awaitPromise", true},
       {"userGesture", true}},
      timeout));
}
bool DomTools::supports(const std::string &operation) {
  static const std::set<std::string> names = {
      "click",     "type",     "fill",  "fill_form", "get",       "check",
      "assert",    "find",     "count", "focus",     "blur",      "hover",
      "select",    "checkbox", "wait",  "scroll",    "press_key", "hotkey",
      "highlight", "mouse",    "drag"};
  return names.contains(operation);
}
JsonDoc DomTools::query(JsonDoc arguments, bool by_value) {
  const auto selector = arguments.value("selector", std::string());
  if (selector.starts_with("role=")) {
    static const std::regex shape(
        R"role(^role=([\w-]+)(?:\[name=(?:"([^"]*)"|'([^']*)'|([^\]]+))\])?$)role");
    std::smatch matched;
    if (!std::regex_match(selector, matched, shape))
      throw BridgeError(
          "Unsupported role selector; use role=button[name=\"Name\"]");
    JsonDoc parameters = JsonDoc::object();
    if (!browser_.frames().empty())
      parameters["frameId"] = browser_.frames().back().frame;
    const auto tree = browser_.context_call("Accessibility.getFullAXTree",
                                            parameters, clock_.remaining());
    JsonDoc candidates = JsonDoc::array();
    for (const auto &node : tree.at("nodes")) {
      if (node.value("ignored", false) || !node.contains("backendDOMNodeId") ||
          node.value("role", JsonDoc::object()).value("value", std::string()) !=
              matched[1].str())
        continue;
      candidates.push_back(
          {{"id", node.at("backendDOMNodeId")},
           {"name",
            node.value("name", JsonDoc::object()).value("value", std::string())}});
    }
    // Chrome supplies computed roles/names, including aria-labelledby, hidden
    // descendants, image alternatives and shadow trees. JS only normalizes
    // text.
    const auto wanted = matched[2].matched   ? JsonDoc(matched[2].str())
                        : matched[3].matched ? JsonDoc(matched[3].str())
                        : matched[4].matched ? JsonDoc(matched[4].str())
                                             : JsonDoc(nullptr);
    const auto identifiers =
        browser_.evaluate("(()=>{const wanted=" + wanted.dump() +
                              ";const norm=s=>s.replace(/\\s+/g,' "
                              "').trim().toLowerCase();return " +
                              candidates.dump() +
                              ".filter(n=>wanted===null||norm(n.name).includes("
                              "norm(wanted))).map(n=>n.id)})()",
                          clock_.remaining());
    const auto operation = arguments.value("operation", std::string("locate"));
    if (operation == "count")
      return {{"count", identifiers.size()}};
    auto resolve = [&](std::size_t index) {
      JsonDoc request = {{"backendNodeId", identifiers.at(index)}};
      if (!browser_.frames().empty() && browser_.frames().back().context)
        request["executionContextId"] = *browser_.frames().back().context;
      return browser_
          .context_call("DOM.resolveNode", request, clock_.remaining())
          .at("object");
    };
    if (operation == "find") {
      JsonDoc elements = JsonDoc::array();
      const auto limit = std::min(identifiers.size(),
                                  arguments.value("limit", std::size_t{10}));
      for (std::size_t i = 0; i < limit; ++i) {
        auto remote = resolve(i);
        NodeLease lease(browser_, remote.at("objectId"));
        auto row = lease.call({{"operation", "summary"}}, clock_.remaining());
        row["index"] = i;
        elements.push_back(std::move(row));
      }
      return {{"found", elements.size()},
              {"total", identifiers.size()},
              {"elements", elements}};
    }
    const auto index = arguments.value("index", std::size_t{0});
    if (index >= identifiers.size()) {
      if (operation == "locate")
        return {{"type", "object"}, {"subtype", "null"}, {"value", nullptr}};
      if (operation == "state") {
        const auto state = arguments.at("state").get<std::string>();
        return {{"result", state == "hidden" || state == "detached"},
                {"count", identifiers.size()}};
      }
      throw BridgeError("Role selector did not match an element");
    }
    auto remote = resolve(index);
    if (operation == "locate" && !by_value)
      return remote;
    NodeLease lease(browser_, remote.at("objectId"));
    auto result = lease.call(arguments, clock_.remaining());
    if (operation == "state")
      return {{"result", result}, {"count", identifiers.size()}};
    return result;
  }
  return browser_.evaluate(script(arguments), clock_.remaining(), by_value);
}
NodeLease DomTools::locate(JsonDoc arguments, bool visible, bool enabled,
                                bool editable) {
  BrowserSession::PageScope page(browser_);
  arguments["operation"] = "locate";
  while (true) {
    const auto remote = query(arguments, false);
    if (remote.contains("objectId")) {
      NodeLease lease(browser_, remote.at("objectId"));
      if (!visible || lease.call({{"operation", "ready"},
                                  {"enabled", enabled},
                                  {"editable", editable}},
                                 clock_.remaining()) == true)
        return lease;
    }
    clock_.pause(std::min(clock_.remaining(), MsDuration(20)));
  }
}
JsonDoc DomTools::point(NodeLease &element, bool scroll) {
  if (scroll) {
    browser_.reveal_frames(clock_.remaining());
    element.call({{"operation", "scroll"}}, clock_.remaining());
    browser_.settle_layout(clock_.remaining());
  }
  while (true) {
    auto result = element.call({{"operation", "point"}}, clock_.remaining());
    result = browser_.project_point(std::move(result), clock_.remaining());
    if (result.at("hit") == true && result.at("width").get<double>() > 0 &&
        result.at("height").get<double>() > 0)
      return result;
    clock_.pause(std::min(clock_.remaining(), MsDuration(20)));
  }
}
void DomTools::pointer(const std::string &type, double x, double y,
                         const std::string &button, int count) {
  try {
    browser_.dispatch_pointer(type, x, y, button, count, clock_.remaining(), pointer_target_);
  } catch (...) {
    browser_.cancel_pointer();
    throw;
  }
}
void DomTools::press(const std::string &combination) {
  browser_.press_keyboard(combination, clock_.remaining());
}
JsonDoc DomTools::enter_text(const JsonDoc &arguments, bool filling) {
  auto choice = arguments;
  choice.erase("text");
  // The type index addresses all visible inputs in the selected document.
  // Labels and placeholders select their first match and take precedence.
  if (!choice.value("label", std::string()).empty()) {
    choice.erase("placeholder");
    choice.erase("selector");
    choice.erase("index");
  } else if (!choice.value("placeholder", std::string()).empty()) {
    choice.erase("label");
    choice.erase("selector");
    choice.erase("index");
  } else if (choice.contains("index")) {
    choice.erase("label");
    choice.erase("placeholder");
    choice.erase("selector");
    choice["inputIndex"] = true;
  } else {
    choice.erase("label");
    choice.erase("placeholder");
    if (choice.value("selector", std::string()).empty())
      throw BridgeError(
          "Text input requires a selector, label, placeholder or index");
  }
  if (arguments.value("if_exists", false)) {
    choice["operation"] = "state";
    choice["state"] = "exists";
    if (query(choice).at("result") == false)
      return {{"typed", false}, {"reason", "Element does not exist"}};
  }
  auto element = locate(choice, true, true, true);
  element.call({{"operation", "scroll"}}, clock_.remaining());
  const auto text = arguments.at("text").get<std::string>();
  const auto ready =
      element.call({{"operation", "prepare_input"},
                    {"clear", filling || arguments.value("clear", true)},
                    {"text", text}},
                   clock_.remaining());
  const auto mode = filling ? std::string("fast")
                            : arguments.value("mode", std::string("fast"));
  if (ready.at("assigned") == false) {
    if (mode == "fast") {
      // Empty replacement still needs a deletion key; insertText("") is a
      // no-op.
      if (text.empty() && (filling || arguments.value("clear", true)))
        press("Backspace");
      else if (text.empty()) {
      } // Appending an empty string leaves the value intact.
      else
        browser_.page_call("Input.insertText", {{"text", text}},
                           clock_.remaining());
    } else {
      const auto scalars = characters(text);
      const auto delay = MsDuration(static_cast<int>(
          arguments.value("delay", mode == "slow" ? 50.0 : 80.0)));
      if (text.empty() && (filling || arguments.value("clear", true)))
        press("Backspace");
      for (std::size_t i = 0; i < scalars.size(); ++i) {
        browser_.page_call("Input.insertText", {{"text", scalars[i]}},
                           clock_.remaining());
        if (i + 1 < scalars.size())
          clock_.pause(delay);
      }
    }
  }
  const auto value =
      element
          .call({{"operation", "read"}, {"type", "value"}}, clock_.remaining())
          .value("value", JsonDoc(nullptr));
  if (arguments.value("press_enter", false))
    press("Enter");
  std::string preview;
  for (const auto &scalar : characters(text)) {
    if (preview.size() + scalar.size() > 50)
      break;
    preview += scalar;
  }
  if (filling)
    return {{"filled", arguments.at("selector")},
            {"text", preview},
            {"currentValue", value},
            {"inFrame", !browser_.frames().empty()}};
  return {{"typed", true},
          {"length", characters(text).size()},
          {"mode", mode},
          {"currentValue", value},
          {"inFrame", !browser_.frames().empty()}};
}
JsonDoc DomTools::read(const JsonDoc &arguments) {
  if (arguments.value("type", std::string()) == "count") {
    auto options = arguments;
    options["operation"] = "count";
    return query(options);
  }
  try {
    auto element = locate(arguments);
    auto options = arguments;
    options["operation"] = "read";
    auto result = element.call(options, clock_.remaining());
    const auto kind = arguments.value("type", std::string());
    if (!browser_.frames().empty() && result.contains("x") &&
        (kind == "position" || kind == "dimensions" ||
         kind == "bounding_box")) {
      double left = std::numeric_limits<double>::infinity(), top = left;
      double right = -left, bottom = -left;
      const double x = result.at("x"), y = result.at("y"),
                   width = result.at("width"), height = result.at("height");
      for (const auto &[dx, dy] : std::array<std::pair<double, double>, 4>{
               {{0, 0}, {width, 0}, {width, height}, {0, height}}}) {
        const auto point = browser_.project_point(
            {{"x", x + dx}, {"y", y + dy}}, clock_.remaining(), false);
        left = std::min(left, point.at("x").get<double>());
        right = std::max(right, point.at("x").get<double>());
        top = std::min(top, point.at("y").get<double>());
        bottom = std::max(bottom, point.at("y").get<double>());
      }
      result = {{"x", left},
                {"y", top},
                {"width", right - left},
                {"height", bottom - top}};
    }
    return result;
  } catch (const BridgeError &error) {
    if (arguments.value("type", std::string()) == "text")
      return {{"text", arguments.value("fallback", std::string())},
              {"error", error.what()}};
    throw;
  }
}
JsonDoc DomTools::check(const JsonDoc &arguments) {
  auto options = target(arguments);
  if (arguments.contains("class_name")) {
    auto element = locate(options);
    options["operation"] = "classes";
    return element.call(options, clock_.remaining());
  }
  const auto state = arguments.value("state", std::string("exists"));
  options["operation"] = "state";
  options["state"] = state;
  auto facts = query(options);
  if (state == "in_viewport" && facts.at("result") == true &&
      !browser_.frames().empty()) {
    auto element = locate(options);
    const auto bounds = element.call(
        {{"operation", "read"}, {"type", "bounding_box"}}, clock_.remaining());
    const double x = bounds.at("x"), y = bounds.at("y"),
                 width = bounds.at("width"), height = bounds.at("height");
    for (const auto &[dx, dy] : std::array<std::pair<double, double>, 4>{
             {{0, 0}, {width, 0}, {width, height}, {0, height}}})
      if (browser_
              .project_point({{"x", x + dx}, {"y", y + dy}}, clock_.remaining(),
                             false)
              .at("withinViewport") == false)
        facts["result"] = false;
  }
  JsonDoc result = {{state, facts.at("result")}};
  if (state == "exists")
    result["count"] = facts.at("count");
  if (arguments.contains("text") && !arguments.contains("selector"))
    result["text"] = arguments.at("text");
  return result;
}
JsonDoc DomTools::wait(const JsonDoc &arguments) {
  const auto kind = arguments.value(
      "type", std::string(arguments.contains("selector") ? "element"
                          : arguments.contains("text")   ? "text"
                                                         : "time"));
  if (arguments.contains("ms") || kind == "time") {
    const auto amount =
        arguments.value("ms", arguments.value("timeout", 1000.0));
    if (!std::isfinite(amount) || amount < 0 || amount > 60000)
      throw BridgeError(
          "Wait duration must be between 0 and 60000 milliseconds");
    clock_.pause(MsDuration(static_cast<int>(amount)));
    return {{"waited", amount}};
  }
  if (kind == "load" || kind == "domcontentloaded" || kind == "networkidle" ||
      kind == "network_idle") {
    browser_.wait_ready(kind == "network_idle" ? "networkidle" : kind,
                        clock_.remaining());
    return kind == "network_idle" ? JsonDoc{{"idle", true}}
                                  : JsonDoc{{"loaded", kind}};
  }
  JsonDoc options = target(arguments);
  if (kind == "element" || kind == "gone" || kind == "hidden" ||
      kind == "text") {
    require(arguments, kind == "text" ? "text" : "selector");
    options["operation"] = "state";
    options["state"] = (kind == "gone" || kind == "hidden")
                           ? "hidden"
                           : arguments.value("state", std::string("visible"));
    if (kind == "text") {
      options["targetText"] = true;
      options.erase("selector");
    }
    while (query(options).at("result") != true)
      clock_.pause(std::min(clock_.remaining(), MsDuration(20)));
    if (kind == "gone" || kind == "hidden")
      return {{"gone", true}, {"selector", arguments.at("selector")}};
    return kind == "text"
               ? JsonDoc{{"found", true}, {"text", arguments.at("text")}}
               : JsonDoc{{"found", true}, {"selector", arguments.at("selector")}};
  }
  if (kind == "function") {
    require(arguments, "expression");
    const auto expression = arguments.at("expression").get<std::string>();
    while (!browser_.test_condition(expression, clock_.remaining()))
      clock_.pause(std::min(clock_.remaining(), MsDuration(20)));
    return {{"condition", true}};
  }
  if (kind == "url") {
    require(arguments, "pattern");
    // Glob matching is evaluated in the browser with its execution timeout;
    // callers cannot inject code through a pattern string.
    const auto literal = arguments.at("pattern").dump();
    const auto expression =
        "(()=>{const p=" + literal +
        ";const "
        "escaped=p.replace(/[.+?^${}()|[\\]\\\\]/g,'\\\\$&').replace(/\\*/"
        "g,'.*');return p.includes('*')?new "
        "RegExp('^'+escaped+'$').test(location.href):location.href.includes(p)}"
        ")()";
    while (browser_.evaluate(expression, clock_.remaining()) != true)
      clock_.pause(std::min(clock_.remaining(), MsDuration(20)));
    return {{"matched", true},
            {"url", browser_.evaluate("location.href", clock_.remaining())}};
  }
  throw BridgeError("Unknown wait condition: " + kind);
}
JsonDoc DomTools::assert_fact(const JsonDoc &arguments) {
  const auto kind = arguments.at("type").get<std::string>();
  try {
    if (kind == "visible" || kind == "hidden" || kind == "text") {
      auto options = arguments;
      options["type"] = kind == "text" ? "text" : "element";
      options["state"] = kind == "hidden" ? "hidden" : "visible";
      wait(options);
      return {{"passed", true}, {"type", kind}};
    }
    if (kind == "element") {
      require(arguments, "state");
      const auto facts = check(arguments);
      const auto state = arguments.at("state").get<std::string>();
      if (!facts.value(state, false))
        throw BridgeError("Element state assertion failed: " + state);
      return {{"passed", true}, {"type", kind}, {"state", state}};
    }
    if (kind == "count") {
      require(arguments, "selector");
      require(arguments, "expected");
      auto options = arguments;
      options["operation"] = "count";
      const auto count = query(options).at("count").get<double>();
      const auto expected = arguments.at("expected").get<double>();
      const auto compare = arguments.value("operator", std::string("=="));
      const bool matches = compare == "=="   ? count == expected
                           : compare == "!=" ? count != expected
                           : compare == ">"  ? count > expected
                           : compare == ">=" ? count >= expected
                           : compare == "<"
                               ? count < expected
                               : compare == "<=" && count <= expected;
      return {{"passed", matches},
              {"type", kind},
              {"count", count},
              {"expected", expected},
              {"operator", compare}};
    }
    if (kind == "url") {
      require(arguments, "pattern");
      const auto expression =
          "(()=>{const p=" + arguments.at("pattern").dump() +
          ";let matched=location.href.includes(p);try{matched ||= new "
          "RegExp(p).test(location.href)}catch{}return "
          "{matched,url:location.href}})()";
      const auto result = browser_.evaluate(expression, clock_.remaining());
      return {{"passed", result.at("matched")},
              {"type", kind},
              {"url", result.at("url")}};
    }
    throw BridgeError("Unknown assertion type: " + kind);
  } catch (const BridgeError &error) {
    return {{"passed", false}, {"type", kind}, {"error", error.what()}};
  }
}
JsonDoc DomTools::scroll(const JsonDoc &arguments) {
  if (arguments.value("load_more", false)) {
    const auto maximum = arguments.value("max_scrolls", 10.0);
    if (maximum < 0 || maximum > 100 || std::trunc(maximum) != maximum)
      throw BridgeError("max_scrolls must be an integer between 0 and 100");
    JsonDoc prior;
    int count = 0;
    for (; count < maximum; ++count) {
      const auto height = browser_.evaluate(
          "document.documentElement.scrollHeight", clock_.remaining());
      if (height == prior)
        break;
      prior = height;
      browser_.evaluate("window.scrollTo({top:document.documentElement."
                        "scrollHeight,behavior:'instant'})",
                        clock_.remaining());
      clock_.pause(MsDuration(500));
    }
    return {{"scrolls", count}};
  }
  if (arguments.contains("within")) {
    auto element = locate({{"selector", arguments.at("within")}});
    const auto delta = arguments.value("by", JsonDoc::object());
    const auto x = delta.value("x", 0.0), y = delta.value("y", 0.0);
    element.call({{"operation", "scroll_by"}, {"x", x}, {"y", y}},
                 clock_.remaining());
    return {{"scrolled", arguments.at("within")}, {"by", {{"x", x}, {"y", y}}}};
  }
  if (arguments.contains("to") || arguments.contains("text")) {
    const auto destination = arguments.value("to", std::string());
    if (destination == "top" || destination == "bottom" ||
        destination == "center") {
      browser_.evaluate(
          "window.scrollTo({left:0,top:" +
              std::string(destination == "top" ? "0"
                          : destination == "center"
                              ? "document.documentElement.scrollHeight/2"
                              : "document.documentElement.scrollHeight") +
              ",behavior:'instant'})",
          clock_.remaining());
    } else {
      auto options =
          arguments.contains("text")
              ? JsonDoc{{"text", arguments.at("text")}, {"targetText", true}}
              : JsonDoc{{"selector", destination}};
      auto element = locate(options);
      element.call({{"operation", "scroll"}}, clock_.remaining());
    }
    return {{"scrolled", arguments.contains("text") ? arguments.at("text")
                                                    : JsonDoc(destination)}};
  }
  JsonDoc coordinates =
      arguments.value("position", arguments.value("by", JsonDoc::object()));
  if (arguments.contains("direction")) {
    const auto direction = arguments.at("direction").get<std::string>();
    const auto amount = arguments.value("amount", 300.0);
    coordinates = {{"x", direction == "left"    ? -amount
                         : direction == "right" ? amount
                                                : 0},
                   {"y", direction == "up"     ? -amount
                         : direction == "down" ? amount
                                               : 0}};
  }
  if (coordinates.empty())
    return {{"scrolled", false}};
  const auto x = coordinates.value("x", 0.0), y = coordinates.value("y", 0.0);
  browser_.evaluate(std::string(arguments.contains("position")
                                    ? "window.scrollTo"
                                    : "window.scrollBy") +
                        "({left:" + JsonDoc(x).dump() + ",top:" + JsonDoc(y).dump() +
                        ",behavior:'instant'})",
                    clock_.remaining());
  return {{"scrolled", coordinates}};
}
JsonDoc DomTools::fill_form(const JsonDoc &arguments) {
  JsonDoc results = JsonDoc::array();
  std::size_t successes = 0;
  const auto &fields = arguments.at("fields");
  if (fields.size() > 100)
    throw BridgeError("A form may contain at most 100 field assignments");
  for (const auto &[name, value] : fields.items()) {
    try {
      auto element = locate({{"field", name}}, true, true, true);
      const auto text =
          value.is_string() ? value.get<std::string>() : value.dump();
      const auto ready = element.call(
          {{"operation", "prepare_input"}, {"clear", true}, {"text", text}},
          clock_.remaining());
      if (ready.at("assigned") == false) {
        if (text.empty())
          press("Backspace");
        else
          browser_.page_call("Input.insertText", {{"text", text}},
                             clock_.remaining());
      }
      results.push_back({{"field", name}, {"success", true}});
      ++successes;
    } catch (const BridgeError &error) {
      results.push_back(
          {{"field", name}, {"success", false}, {"error", error.what()}});
    }
  }
  JsonDoc result = {{"filled", successes},
                 {"total", fields.size()},
                 {"results", results},
                 {"inFrame", !browser_.frames().empty()}};
  if (arguments.value("submit", false)) {
    // Report submit failures instead of claiming a form was submitted.
    try {
      result["submitted"] =
          click({{"selector", "button[type=submit],input[type=submit]"}})
              .at("clicked");
    } catch (const BridgeError &error) {
      result["submitted"] = false;
      result["submitError"] = error.what();
    }
  }
  return result;
}
JsonDoc DomTools::mouse(const JsonDoc &arguments) {
  const auto action = arguments.at("action").get<std::string>();
  const auto button = arguments.value("button", std::string("left"));
  const auto prior = browser_.pointer_position();
  const auto start_x = prior.at("x").get<double>();
  const auto start_y = prior.at("y").get<double>();
  if (action == "down" || action == "up") {
    pointer(action == "down" ? "mousePressed" : "mouseReleased", start_x,
            start_y, button, 1);
    return {{action, button}};
  }
  const auto x = arguments.value("x", 0.0), y = arguments.value("y", 0.0);
  const auto steps = std::max(1, arguments.value("steps", 1));
  for (int i = 1; i <= steps; ++i)
    pointer("mouseMoved", start_x + (x - start_x) * i / steps,
            start_y + (y - start_y) * i / steps);
  if (action == "click") {
    try {
      pointer("mousePressed", x, y, button, 1);
      pointer("mouseReleased", x, y, button, 1);
    } catch (...) {
      browser_.cancel_pointer();
      throw;
    }
  }
  return {{action == "click" ? "clicked" : "moved", {{"x", x}, {"y", y}}}};
}
JsonDoc DomTools::drag(const JsonDoc &arguments) {
  std::optional<NodeLease> source, destination;
  JsonDoc from = {{"x", arguments.value("from_x", 0.0)},
               {"y", arguments.value("from_y", 0.0)}};
  if (arguments.contains("from_selector")) {
    source.emplace(
        locate({{"selector", arguments.at("from_selector")}}, true, true));
    from = point(*source);
  }
  JsonDoc to;
  if (arguments.contains("to_selector")) {
    destination.emplace(
        locate({{"selector", arguments.at("to_selector")}}, true));
    to = point(*destination);
    // Revealing the destination may scroll the source. Use its current root
    // coordinates, requiring both endpoints to remain actionable together.
    if (source)
      from = point(*source, false);
  } else if (arguments.contains("offset_x") || arguments.contains("offset_y"))
    to = {{"x", from.at("x").get<double>() + arguments.value("offset_x", 0.0)},
          {"y", from.at("y").get<double>() + arguments.value("offset_y", 0.0)}};
  else
    to = {{"x", arguments.value("to_x", from.at("x").get<double>())},
          {"y", arguments.value("to_y", from.at("y").get<double>())}};
  const double sx = from.at("x"), sy = from.at("y"), ex = to.at("x"),
               ey = to.at("y");
  // Validate the entire requested path before pressing a button.
  for (const auto coordinate : {sx, sy, ex, ey})
    if (!std::isfinite(coordinate) || std::abs(coordinate) > 10000000)
      throw BridgeError("Drag coordinates are outside supported limits");
  if (browser_.pointer_position().at("buttons") != 0)
    throw BridgeError("Release held mouse buttons before starting a drag");
  try {
    pointer("mouseMoved", sx, sy);
    pointer("mousePressed", sx, sy, "left", 1);
    for (int i = 1; i <= 16; ++i) {
      pointer("mouseMoved", sx + (ex - sx) * i / 16, sy + (ey - sy) * i / 16);
      clock_.pause(MsDuration(8));
    }
    pointer("mouseReleased", ex, ey, "left", 1);
    browser_.settle_layout(clock_.remaining());
  } catch (...) {
    browser_.cancel_pointer();
    throw;
  }
  return {{"dragged", true},
          {"from", {{"x", sx}, {"y", sy}}},
          {"to", {{"x", ex}, {"y", ey}}}};
}
JsonDoc DomTools::execute(const std::string &operation, const JsonDoc &arguments) {
  BrowserSession::PageScope page(browser_);
  if (operation == "mouse")
    return mouse(arguments);
  if (operation == "drag")
    return drag(arguments);
  if (operation == "click")
    return click(arguments);
  if (operation == "type" || operation == "fill")
    return enter_text(arguments, operation == "fill");
  if (operation == "fill_form")
    return fill_form(arguments);
  if (operation == "get")
    return read(arguments);
  if (operation == "check")
    return check(arguments);
  if (operation == "assert")
    return assert_fact(arguments);
  if (operation == "wait")
    return wait(arguments);
  if (operation == "scroll")
    return scroll(arguments);
  if (operation == "highlight") {
    auto element = locate({{"selector", arguments.at("selector")}});
    auto options = arguments;
    options["operation"] = "highlight";
    element.call(options, clock_.remaining());
    return {{"highlighted", arguments.at("selector")}};
  }
  if (operation == "count" || operation == "find") {
    auto options = target(arguments);
    if (operation == "find" && arguments.contains("text")) {
      options["targetText"] = true;
      options.erase("selector");
    }
    options["operation"] = operation;
    if (operation == "find" && arguments.contains("attribute"))
      options["searchAttribute"] = true;
    return query(options);
  }
  if (operation == "press_key" || operation == "hotkey") {
    auto combination =
        arguments.at(operation == "hotkey" ? "keys" : "key").get<std::string>();
    if (arguments.contains("modifiers")) {
      std::string prefix;
      for (const auto &modifier : arguments.at("modifiers"))
        prefix += modifier.get<std::string>() + "+";
      combination = prefix + combination;
    }
    press(combination);
    return {{"pressed", combination}};
  }
  if (operation == "focus" || operation == "blur") {
    auto element = locate(arguments);
    if (element.call({{"operation", operation}}, clock_.remaining()) != true)
      throw BridgeError("Element did not receive focus");
    return {{operation == "focus" ? "focused" : "blurred",
             arguments.at("selector")}};
  }
  if (operation == "hover") {
    auto element = locate(arguments, true);
    const auto at = point(element);
    InputAck acknowledgement(browser_, element.identity(), "mousemove", 0,
                                clock_.remaining());
    pointer("mouseMoved", at.at("x"), at.at("y"));
    acknowledgement.finish(clock_.remaining());
    return {{"hovered", arguments.at("selector")}};
  }
  if (operation == "select") {
    if (!arguments.contains("value") && !arguments.contains("index") &&
        !arguments.contains("text"))
      throw BridgeError("Select requires a value, index, or text");
    auto element = locate({{"selector", arguments.at("selector")}}, true, true);
    auto options = arguments;
    options["operation"] = "select";
    return element.call(options, clock_.remaining());
  }
  if (operation == "checkbox") {
    auto element = locate(arguments, true, true);
    element.call({{"operation", "checkbox_kind"}}, clock_.remaining());
    const auto prior = element
                           .call({{"operation", "state"}, {"state", "checked"}},
                                 clock_.remaining())
                           .get<bool>();
    const auto desired = arguments.value("checked", !prior);
    if (prior != desired) {
      const auto at = point(element);
      const auto x = at.at("x").get<double>(), y = at.at("y").get<double>();
      InputAck acknowledgement(browser_, element.identity(), "click", 1,
                                  clock_.remaining());
      pointer("mouseMoved", x, y);
      pointer("mousePressed", x, y, "left", 1);
      pointer("mouseReleased", x, y, "left", 1);
      acknowledgement.finish(clock_.remaining());
    }
    if (element.call({{"operation", "state"}, {"state", "checked"}},
                     clock_.remaining()) != desired)
      throw BridgeError("Checkbox did not reach the requested state");
    return {{"checked", desired}};
  }
  throw BridgeError("Unsupported DOM action: " + operation);
}
} // namespace pagepilot
