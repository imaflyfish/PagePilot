#include <algorithm>
#include <pagepilot/core.hpp>
#include <thread>
namespace pagepilot {
namespace {
thread_local std::stop_token active_token;
}
CancelScope::CancelScope(std::stop_token token) : previous_(active_token) {
  active_token = std::move(token);
}
CancelScope::~CancelScope() { active_token = previous_; }
void cancellation_point() {
  if (active_token.stop_requested())
    throw RequestAborted();
}
void interruptible_pause(MsDuration duration) {
  const auto until = std::chrono::steady_clock::now() + duration;
  while (true) {
    cancellation_point();
    const auto left =
        std::chrono::ceil<MsDuration>(until - std::chrono::steady_clock::now());
    if (left.count() <= 0)
      return;
    std::this_thread::sleep_for(std::min(left, MsDuration(10)));
  }
}
} // namespace pagepilot
