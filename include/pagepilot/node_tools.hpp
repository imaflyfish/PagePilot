#pragma once
#include <pagepilot/session.hpp>
namespace pagepilot {
class StepClock {
public:
  explicit StepClock(MsDuration allowance);
  MsDuration remaining() const;
  void pause(MsDuration duration) const;

private:
  std::chrono::steady_clock::time_point end_;
};

// A remote object is held only for the duration of an action. Its identity
// stays fixed across readiness checks and input, and is released on failure as
// well.
class NodeLease {
public:
  NodeLease(BrowserSession &browser, std::string identity);
  NodeLease(NodeLease &&other) noexcept;
  NodeLease(const NodeLease &) = delete;
  NodeLease &operator=(const NodeLease &) = delete;
  ~NodeLease();
  JsonDoc call(const JsonDoc &arguments, MsDuration timeout);
  const std::string &identity() const { return identity_; }
  const std::string &session() const { return session_; }

private:
  BrowserSession *browser_;
  std::string identity_, session_;
};

class DomTools {
public:
  DomTools(BrowserSession &browser, MsDuration allowance)
      : browser_(browser), clock_(allowance) {}
  static bool supports(const std::string &operation);
  JsonDoc execute(const std::string &operation, const JsonDoc &arguments);
  JsonDoc query(JsonDoc arguments, bool by_value = true);
  NodeLease locate(JsonDoc arguments, bool visible = false,
                      bool enabled = false, bool editable = false);
  JsonDoc capture_bounds(const std::string &selector);

private:
  JsonDoc click(const JsonDoc &arguments);
  JsonDoc enter_text(const JsonDoc &arguments, bool filling);
  JsonDoc read(const JsonDoc &arguments);
  JsonDoc check(const JsonDoc &arguments);
  JsonDoc wait(const JsonDoc &arguments);
  JsonDoc assert_fact(const JsonDoc &arguments);
  JsonDoc scroll(const JsonDoc &arguments);
  JsonDoc fill_form(const JsonDoc &arguments);
  JsonDoc mouse(const JsonDoc &arguments);
  JsonDoc drag(const JsonDoc &arguments);
  JsonDoc point(NodeLease &element, bool scroll = true);
  void press(const std::string &combination);
  void pointer(const std::string &type, double x, double y,
               const std::string &button = "none", int count = 0);
  BrowserSession &browser_;
  StepClock clock_;
  std::string pointer_target_;
};
} // namespace pagepilot
