#pragma once
#include <deque>
#include <map>
#include <optional>
#include <pagepilot/cdp_channel.hpp>
#include <set>
namespace pagepilot {
struct FrameSelection {
  std::string frame, session;
  std::optional<std::int64_t> context;
  std::string unique_context, owner_session;
  std::int64_t owner_node = 0;
};
class BrowserSession {
public:
  // Bind a multi-command action on its first page access. A closed page may be
  // replaced in the tab inventory, but the pending action must not follow it.
  // Nested scopes share their outer binding; pure delays need no connection.
  class PageScope {
  public:
    explicit PageScope(BrowserSession &browser);
    ~PageScope();
    PageScope(const PageScope &) = delete;
    PageScope &operator=(const PageScope &) = delete;

  private:
    BrowserSession &browser_;
    std::string target_;
    std::string *previous_;
  };
  using Deadline = std::optional<std::chrono::steady_clock::time_point>;
  explicit BrowserSession(unsigned port) : port_(port) {}
  ~BrowserSession() { disconnect(); }
  void connect();
  void disconnect();
  bool connected() const;
  JsonDoc tabs();
  JsonDoc status();
  JsonDoc create_tab(const std::string &url = "about:blank");
  JsonDoc activate_tab(std::size_t index);
  JsonDoc close_tab(std::optional<std::size_t> index = {});
  JsonDoc evaluate(const std::string &expression,
                   MsDuration timeout = MsDuration(10000),
                   bool by_value = true);
  bool test_condition(const std::string &expression, MsDuration timeout);
  JsonDoc evaluate_legacy(std::string expression, MsDuration timeout);
  JsonDoc page_call(const std::string &method,
                    const JsonDoc &parameters = JsonDoc::object(),
                    MsDuration timeout = MsDuration(10000));
  JsonDoc context_call(const std::string &method,
                       const JsonDoc &parameters = JsonDoc::object(),
                       MsDuration timeout = MsDuration(10000));
  // Remote object IDs belong to their creating session, independently of the
  // current tab/frame selection. Never reroute or replay these calls.
  JsonDoc session_call(const std::string &session, const std::string &method,
                       const JsonDoc &parameters, MsDuration timeout);
  JsonDoc browser_call(const std::string &method,
                       const JsonDoc &parameters = JsonDoc::object(),
                       MsDuration timeout = MsDuration(10000));
  JsonDoc navigate(const std::string &url,
                   const std::string &readiness = "load",
                   MsDuration timeout = MsDuration(30000));
  JsonDoc reload(MsDuration timeout = MsDuration(30000));
  JsonDoc history(int direction, MsDuration timeout = MsDuration(30000));
  void wait_ready(const std::string &readiness, MsDuration timeout);
  JsonDoc console_messages(std::size_t maximum = 100, bool clear = false);
  void pump();
  std::string current_session();
  const std::vector<FrameSelection> &frames() const { return frames_; }
  std::vector<FrameSelection> &frames() { return frames_; }
  std::string current_target() const { return current_; }
  Deadline exchange_deadline(Deadline limit);
  std::chrono::steady_clock::time_point
  bounded_deadline(MsDuration requested) const;
  MsDuration time_left(MsDuration requested) const;
  std::string context_session() const;
  void release_object(const std::string &session,
                      const std::string &identity) noexcept;
  void release_input(const std::string &method,
                     const JsonDoc &parameters) noexcept;
  std::string observe_input(const std::string &element,
                            const std::string &event, int count,
                            MsDuration timeout);
  void await_input(const std::string &ticket, MsDuration timeout);
  void release_input_signal(const std::string &ticket) noexcept;
  JsonDoc enter_frame(const std::string &object, MsDuration timeout);
  JsonDoc list_frames();
  JsonDoc project_point(JsonDoc point, MsDuration timeout,
                        bool hit_test = true);
  void reveal_frames(MsDuration timeout);
  void settle_layout(MsDuration timeout);
  JsonDoc manage_cookies(const JsonDoc &arguments);
  JsonDoc manage_storage(const JsonDoc &arguments);
  JsonDoc accessibility_snapshot();
  JsonDoc arm_dialog(const JsonDoc &arguments);
  JsonDoc pointer_position();
  void press_keyboard(const std::string &combination, MsDuration timeout);
  void dispatch_pointer(const std::string &type, double x, double y,
                        const std::string &button, int count,
                        MsDuration timeout,
                        const std::string &expected_target = {});
  void cancel_pointer() noexcept;

private:
  std::string *bound_target_ = nullptr;
  JsonDoc context_parameters();
  JsonDoc run_script(const std::string &expression, MsDuration timeout,
                     bool by_value, bool retry_replaced);
  JsonDoc await_navigation(const std::string &session,
                           const std::string &expected_loader,
                           const std::string &replaced_loader,
                           std::optional<int> history_entry,
                           const std::string &readiness,
                           std::chrono::steady_clock::time_point deadline);
  void refresh();
  void attach();
  void enable_session(const std::string &session);
  void discard_session(const std::string &session) noexcept;
  // Enabling happens after the session is already recorded, so a failure has to
  // discard it: an attached but unenabled session must never be reused.
  void enable_or_discard(const std::string &session);
  std::string frame_session(const std::string &frame,
                            const std::string &parent);
  void prepare_frames();
  JsonDoc frame_viewport(std::size_t index, MsDuration timeout);
  JsonDoc owner_geometry(std::size_t index, MsDuration timeout);
  bool owner_hit(std::size_t index, double x, double y, MsDuration timeout);
  JsonDoc send(const std::string &method,
               const JsonDoc &parameters = JsonDoc::object(),
               const std::string &session = {},
               MsDuration timeout = MsDuration(10000),
               std::function<void()> observe = {});
  unsigned port_;
  std::unique_ptr<CdpChannel> channel_;
  std::vector<JsonDoc> targets_;
  std::map<std::string, std::string> sessions_;
  std::map<std::string, std::string> frame_sessions_, session_roots_;
  std::map<std::string, std::string> session_pages_;
  std::map<std::string, JsonDoc> dialog_rules_;
  struct PointerState {
    double x = 0, y = 0;
    int buttons = 0;
    bool intercept = false, entered = false;
    std::string session;
    JsonDoc drag;
  };
  std::map<std::string, PointerState> pointers_;
  std::map<std::string, std::map<std::string, JsonDoc>> documents_;
  struct InputWatch {
    std::string session, frame, unique_context, object;
    std::int64_t context = 0;
    bool received = false, settled = false;
  };
  std::map<std::string, InputWatch> input_watches_;
  std::string current_, context_;
  bool context_selected_ = false;
  Deadline deadline_;
  std::vector<FrameSelection> frames_;
  std::deque<JsonDoc> console_;
  struct RequestScope {
    std::string frame, loader;
  };
  std::map<std::string, std::map<std::string, RequestScope>> requests_;
  std::map<std::string, std::chrono::steady_clock::time_point> changed_;
};

// Narrows the session's deadline for a scope and restores the previous one,
// including on an early return or an exception. Take it by duration to bound a
// step against the enclosing deadline, or by deadline when one is already
// computed. Copying would restore the same prior deadline twice, so it is
// deleted, as on CancelScope.
class DeadlineScope {
public:
  DeadlineScope(BrowserSession &browser, MsDuration allowance);
  DeadlineScope(BrowserSession &browser, BrowserSession::Deadline deadline);
  ~DeadlineScope();
  DeadlineScope(const DeadlineScope &) = delete;
  DeadlineScope &operator=(const DeadlineScope &) = delete;

private:
  BrowserSession &browser_;
  BrowserSession::Deadline prior_;
};
} // namespace pagepilot
