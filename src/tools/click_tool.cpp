#include "input_ack.hpp"
#include <algorithm>
#include <pagepilot/node_tools.hpp>
#include <cmath>
namespace pagepilot {
namespace {
bool same_position(const JsonDoc &before, const JsonDoc &after) {
  if (!after.value("attached", false) || !after.value("ready", false) ||
      !after.value("hit", false))
    return false;
  for (const auto *axis : {"x", "y", "width", "height"})
    if (std::abs(before.at(axis).get<double>() - after.at(axis).get<double>()) >
        0.1)
      return false;
  return true;
}
} // namespace
JsonDoc DomTools::click(const JsonDoc &arguments) {
  const auto mode = arguments.value("mode", std::string("fast"));
  const auto kind = arguments.value("type", std::string("single"));
  const auto button = kind == "right" ? "right" : "left";
  const auto count = kind == "double" ? 2 : kind == "triple" ? 3 : 1;
  const bool coordinates = arguments.contains("x") && arguments.contains("y");
  auto choice = arguments;
  if (!coordinates) {
    if (!choice.value("text", std::string()).empty()) {
      choice["targetText"] = true;
      choice.erase("selector");
    } else {
      choice.erase("text");
      if (choice.value("selector", std::string()).empty())
        throw BridgeError(
            "Click requires a selector, text, or both coordinates");
    }
  }
  browser_.current_session();
  pointer_target_ = browser_.current_target();
  if (!coordinates && arguments.value("if_exists", false)) {
    auto present = choice;
    present["operation"] = "state";
    present["state"] = "exists";
    if (query(present).at("result") == false)
      return {{"clicked", false}, {"reason", "Element does not exist"}};
  }
  if (!arguments.value("hover_first", std::string()).empty()) {
    auto prior = locate({{"selector", arguments.at("hover_first")}}, true);
    const auto position = point(prior);
    pointer("mouseMoved", position.at("x"), position.at("y"));
  }
  auto move_to = [&](const JsonDoc &position) {
    const double x = position.at("x"), y = position.at("y");
    const auto origin = browser_.pointer_position();
    const double start_x = origin.at("x"), start_y = origin.at("y");
    const int steps = mode == "human" ? 12 : mode == "smart" ? 3 : 1;
    for (int i = 1; i <= steps; ++i) {
      pointer("mouseMoved", start_x + (x - start_x) * double(i) / steps,
              start_y + (y - start_y) * double(i) / steps);
      if (steps > 1)
        clock_.pause(MsDuration(mode == "human" ? 8 : 4));
    }
  };
  JsonDoc at;
  std::optional<NodeLease> element;
  std::optional<InputAck> acknowledgement;
  auto sample = [&] {
    auto result =
        element->call({{"operation", "click_probe"}}, clock_.remaining());
    if (result.value("ready", false))
      result = browser_.project_point(std::move(result), clock_.remaining());
    return result;
  };
  if (coordinates) {
    at = {{"x", arguments.at("x")}, {"y", arguments.at("y")}};
    move_to(at);
  } else {
    bool scrolled = false;
    while (true) {
      acknowledgement.reset();
      if (!element) {
        element.emplace(locate(choice));
        scrolled = false;
      }
      const auto before = sample();
      if (!before.value("attached", false)) {
        element.reset();
        continue;
      }
      if (!before.value("ready", false)) {
        clock_.pause(std::min(clock_.remaining(), MsDuration(20)));
        continue;
      }
      if (!scrolled) {
        browser_.reveal_frames(clock_.remaining());
        element->call({{"operation", "scroll"}}, clock_.remaining());
        scrolled = true;
      }
      browser_.settle_layout(clock_.remaining());
      const auto settled = sample();
      if (!same_position(before, settled))
        continue;
      move_to(settled);
      acknowledgement.emplace(browser_, element->identity(),
                              kind == "right"    ? "contextmenu"
                              : kind == "double" ? "dblclick"
                                                 : "click",
                              kind == "right" ? 0 : count, clock_.remaining());
      const auto confirmed = sample();
      if (!same_position(settled, confirmed))
        continue;
      at = confirmed;
      break;
    }
  }
  const double x = at.at("x"), y = at.at("y");
  // After any press is attempted, never reacquire or replay the click.
  for (int i = 1; i <= count; ++i) {
    if (i > 1 && element && !same_position(at, sample()))
      throw BridgeError("Click target changed after an earlier press; input was "
                       "not replayed");
    pointer("mousePressed", x, y, button, i);
    try {
      if (kind == "long")
        clock_.pause(
            MsDuration(static_cast<int>(arguments.value("duration", 500.0))));
      pointer("mouseReleased", x, y, button, i);
    } catch (...) {
      browser_.cancel_pointer();
      throw;
    }
  }
  if (acknowledgement)
    acknowledgement->finish(clock_.remaining());
  if (arguments.contains("wait_after"))
    browser_.wait_ready(arguments.at("wait_after"), clock_.remaining());
  JsonDoc result = {{"clicked", true},
                 {"mode", mode},
                 {"type", kind},
                 {"inFrame", !browser_.frames().empty()}};
  if (at.contains("text"))
    result["elementText"] = at.at("text");
  try {
    const auto page = browser_.page_call(
        "Runtime.evaluate",
        {{"expression", "({url:location.href,title:document.title})"},
         {"returnByValue", true}},
        std::min(clock_.remaining(), MsDuration(500)));
    if (browser_.current_target() == pointer_target_ &&
        !page.contains("exceptionDetails"))
      result.update(page.at("result").at("value"));
  } catch (const BridgeError &) {
  }
  return result;
}
} // namespace pagepilot
