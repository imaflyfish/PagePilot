#pragma once
#include <map>
#include <pagepilot/core.hpp>
namespace pagepilot {
struct ToolDefinition {
  std::string name, legacy, description;
  JsonDoc schema, presets = JsonDoc::object();
  std::string operation;
};
struct ToolInvocation {
  std::string operation;
  JsonDoc arguments;
  bool allow_legacy = false;
  bool legacy_name = false;
};
class ToolCatalog {
public:
  ToolCatalog();
  JsonDoc list(bool compatibility = false) const;
  bool contains(const std::string &name, bool compatibility = false) const;
  ToolInvocation resolve(const std::string &name, const JsonDoc &arguments,
                         bool compatibility = false) const;
  const std::vector<ToolDefinition> &definitions() const {
    return definitions_;
  }

private:
  std::vector<ToolDefinition> definitions_;
  std::map<std::string, std::size_t> canonical_, legacy_;
};
} // namespace pagepilot
