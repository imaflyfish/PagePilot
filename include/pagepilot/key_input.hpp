#pragma once
#include <pagepilot/core.hpp>
namespace pagepilot {
struct KeyEvent {
  JsonDoc pressed;
  JsonDoc released;
};
// Validate the whole chord before any input. Press in vector order, release in
// reverse order; each release is also suitable for partial-failure cleanup.
std::vector<KeyEvent> plan_keyboard(const std::string &combination);
} // namespace pagepilot
