#pragma once
#include <pagepilot/session.hpp>
namespace pagepilot {
class InputAck {
public:
  InputAck(BrowserSession &browser, const std::string &element,
           const std::string &event, int count, MsDuration timeout)
      : browser_(browser),
        ticket_(browser.observe_input(element, event, count, timeout)) {}
  ~InputAck() { browser_.release_input_signal(ticket_); }
  InputAck(const InputAck &) = delete;
  InputAck &operator=(const InputAck &) = delete;
  void finish(MsDuration timeout) { browser_.await_input(ticket_, timeout); }

private:
  BrowserSession &browser_;
  std::string ticket_;
};
} // namespace pagepilot
