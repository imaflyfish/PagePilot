#pragma once
#include <pagepilot/core.hpp>
#include <string>

// Aborting assertions: the first failed expectation throws, so the program
// stops instead of driving a browser that is already in an unexpected state.
// Programs that should report every failure use checks.hpp instead.
//
// Each program keeps its own summary line and exit status, because each reports
// a different suite.
namespace pagepilot_aborting_tests {
inline unsigned checks = 0;

inline void check(bool condition, const std::string &label) {
  if (!condition)
    throw pagepilot::BridgeError(label);
  ++checks;
}

// Passes when the action throws a BridgeError. An action that returns normally
// fails the check, so an expected refusal cannot be satisfied by success.
template <class Action> void rejects(Action action, const std::string &label) {
  bool refused = false;
  try {
    action();
  } catch (const pagepilot::BridgeError &) {
    refused = true;
  }
  check(refused, label);
}
} // namespace pagepilot_aborting_tests
