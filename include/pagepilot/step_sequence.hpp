#pragma once
#include <functional>
#include <pagepilot/session.hpp>
#include <pagepilot/tool_catalog.hpp>
namespace pagepilot {
class StepSequence {
public:
  using Dispatch = std::function<JsonDoc(const ToolInvocation &)>;
  StepSequence(BrowserSession &browser, const ToolCatalog &catalog,
               Dispatch dispatch, bool allow_legacy)
      : browser_(browser), catalog_(catalog), dispatch_(std::move(dispatch)),
        allow_legacy_(allow_legacy) {}
  static bool supports(const std::string &operation);
  JsonDoc run(const ToolInvocation &invocation);

private:
  JsonDoc batch(const JsonDoc &arguments);
  JsonDoc retry(const JsonDoc &arguments);
  JsonDoc steps(const JsonDoc &arguments);
  JsonDoc row(const JsonDoc &input, const std::string &operation) const;
  JsonDoc child(const std::string &name, const JsonDoc &arguments,
                std::optional<MsDuration> timeout = {});
  void pause(MsDuration duration);
  bool expired() const;
  void append(JsonDoc &rows, JsonDoc value);
  BrowserSession &browser_;
  const ToolCatalog &catalog_;
  Dispatch dispatch_;
  bool allow_legacy_;
  std::size_t output_bytes_ = 0;
};
} // namespace pagepilot
