#include <pagepilot/session.hpp>
#include <cmath>

namespace pagepilot {
namespace {
int button_mask(const std::string &button) {
  if (button == "left")
    return 1;
  if (button == "right")
    return 2;
  if (button == "middle")
    return 4;
  if (button == "none")
    return 0;
  throw BridgeError("Unknown mouse button");
}
std::string held_button(int buttons) {
  return (buttons & 1)   ? "left"
         : (buttons & 2) ? "right"
         : (buttons & 4) ? "middle"
                         : "none";
}
} // namespace
JsonDoc BrowserSession::pointer_position() {
  current_session();
  const auto &state = pointers_[current_];
  return {{"x", state.x}, {"y", state.y}, {"buttons", state.buttons}};
}
void BrowserSession::cancel_pointer() noexcept {
  CancelScope mask;
  for (auto &[page, state] : pointers_) {
    (void)page;
    auto cleanup = [&](const std::string &method, const JsonDoc &parameters) {
      try {
        if (connected() && !state.session.empty())
          channel_->call(method, parameters, state.session, MsDuration(100));
      } catch (...) {
      }
    };
    if (!state.drag.is_null())
      cleanup("Input.dispatchDragEvent", {{"type", "dragCancel"},
                                          {"x", state.x},
                                          {"y", state.y},
                                          {"data", state.drag}});
    for (const auto *button : {"left", "right", "middle"})
      if (state.buttons & button_mask(button)) {
        state.buttons &= ~button_mask(button);
        cleanup("Input.dispatchMouseEvent", {{"type", "mouseReleased"},
                                             {"x", state.x},
                                             {"y", state.y},
                                             {"button", button},
                                             {"buttons", state.buttons},
                                             {"clickCount", 1}});
      }
    if (state.intercept)
      cleanup("Input.setInterceptDrags", {{"enabled", false}});
    state.intercept = false;
    state.entered = false;
    state.drag = nullptr;
  }
}
void BrowserSession::dispatch_pointer(const std::string &type, double x,
                                        double y, const std::string &button,
                                        int count, MsDuration timeout,
                                        const std::string &expected_target) {
  if (!std::isfinite(x) || !std::isfinite(y) || std::abs(x) > 10000000 ||
      std::abs(y) > 10000000)
    throw BridgeError("Pointer coordinates are outside supported limits");
  const auto session = current_session();
  if (!expected_target.empty() && current_ != expected_target)
    throw BridgeError("Click target page closed; pointer input cannot move to another tab");
  auto &state = pointers_[current_];
  state.session = session;
  const auto mask = button_mask(button);
  try {
    if (type == "mousePressed") {
      if (mask == 1 && !state.intercept) {
        send("Input.setInterceptDrags", {{"enabled", true}}, session, timeout);
        state.intercept = true;
      }
      state.buttons |= mask;
    } else if (type == "mouseReleased")
      state.buttons &= ~mask;
    state.x = x;
    state.y = y;
    auto drag_event = [&](const std::string &kind) {
      send("Input.dispatchDragEvent",
           {{"type", kind}, {"x", x}, {"y", y}, {"data", state.drag}}, session,
           timeout);
      pump();
    };
    if (!state.drag.is_null() && type == "mouseReleased" && button == "left") {
      drag_event("drop");
      state.drag = nullptr;
      state.entered = false;
    } else if (state.drag.is_null() || type != "mouseMoved") {
      send("Input.dispatchMouseEvent",
           {{"type", type},
            {"x", x},
            {"y", y},
            {"button",
             type == "mouseMoved" ? held_button(state.buttons) : button},
            {"buttons", state.buttons},
            {"clickCount", count}},
           session, timeout);
      pump();
    }
    if (type == "mouseMoved" && !state.drag.is_null()) {
      if (!state.entered) {
        drag_event("dragEnter");
        state.entered = true;
      }
      drag_event("dragOver");
    }
    if (type == "mouseReleased" && button == "left" && state.intercept) {
      send("Input.setInterceptDrags", {{"enabled", false}}, session, timeout);
      state.intercept = false;
    }
  } catch (...) {
    cancel_pointer();
    throw;
  }
}
} // namespace pagepilot
