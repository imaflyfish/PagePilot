#pragma once
#include <pagepilot/file_tools.hpp>
#include <pagepilot/session.hpp>
#include <pagepilot/tool_catalog.hpp>
namespace pagepilot {
class ToolRuntime {
public:
  explicit ToolRuntime(unsigned port, std::vector<std::filesystem::path> roots =
                                          PathGuard::defaults())
      : browser_(port), paths_(std::move(roots)), port_(port) {}
  JsonDoc invoke(const ToolInvocation &invocation);
  BrowserSession &browser() { return browser_; }

private:
  JsonDoc execute(const ToolInvocation &invocation);
  MsDuration deadline(const JsonDoc &arguments, int fallback) const;
  MsDuration allowance(const std::string &operation,
                       const JsonDoc &arguments) const;
  BrowserSession browser_;
  PathGuard paths_;
  ToolCatalog catalog_;
  unsigned invocation_depth_ = 0, dispatched_ = 0;
  unsigned port_;
  int quick_ = 3000, normal_ = 5000, long_ = 10000;
  bool diagnostic_ = false;
  struct Metric {
    std::uint64_t count = 0, failures = 0, duration = 0;
  };
  std::map<std::string, Metric> metrics_;
};
} // namespace pagepilot
