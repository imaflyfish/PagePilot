#include "embedded_resources.hpp"
#include <pagepilot/tool_catalog.hpp>
namespace pagepilot {
ToolCatalog::ToolCatalog() {
  for (const auto &record : parse_message(resources::catalog)) {
    ToolDefinition definition;
    definition.name = record.at("name");
    definition.legacy = record.at("legacy");
    definition.operation = record.at("operation");
    definition.description = record.at("description");
    definition.schema = record.at("schema");
    definition.presets = record.at("presets");
    if (!definition.name.empty())
      canonical_.emplace(definition.name, definitions_.size());
    legacy_.emplace(definition.legacy, definitions_.size());
    definitions_.push_back(std::move(definition));
  }
  if (canonical_.size() != 54 || legacy_.size() != 75)
    throw BridgeError("invalid built-in catalog");
}
JsonDoc ToolCatalog::list(bool compatibility) const {
  JsonDoc output = JsonDoc::array();
  for (const auto &definition : definitions_) {
    if (definition.name.empty() && !compatibility)
      continue;
    output.push_back(
        {{"name", compatibility ? definition.legacy : definition.name},
         {"description", definition.description},
         {"inputSchema", definition.schema}});
  }
  return output;
}
bool ToolCatalog::contains(const std::string &name, bool compatibility) const {
  return canonical_.contains(name) || (compatibility && legacy_.contains(name));
}
ToolInvocation ToolCatalog::resolve(const std::string &name,
                                        const JsonDoc &arguments,
                                        bool compatibility) const {
  auto found = canonical_.find(name);
  const bool legacy_name = found == canonical_.end();
  std::size_t index = 0;
  if (found != canonical_.end())
    index = found->second;
  else {
    auto alias = legacy_.find(name);
    if (!compatibility || alias == legacy_.end())
      throw BridgeError("unknown action: " + name);
    index = alias->second;
  }
  const auto &definition = definitions_[index];
  auto schema = definition.schema;
  // Old compositions reported malformed rows individually. In compatibility
  // calls, defer each row's schema check to the sequencer, preserving those
  // diagnostics while still validating every row before execution.
  if (legacy_name &&
      (definition.operation == "batch" || definition.operation == "run_steps"))
    schema["properties"][definition.operation == "batch" ? "actions" : "steps"]
        .erase("items");
  auto values = normalize_arguments(arguments, schema);
  // Legacy handlers destructured their public parameters. Keep the same
  // boundary so unknown input cannot become a private DOM-adapter control
  // field.
  const auto properties = definition.schema.value("properties", JsonDoc::object());
  for (auto iterator = values.begin(); iterator != values.end();)
    if (!properties.contains(iterator.key()))
      iterator = values.erase(iterator);
    else
      ++iterator;
  for (const auto &[key, value] : definition.presets.items())
    values[key] = value;
  return {definition.operation, std::move(values), compatibility, legacy_name};
}
} // namespace pagepilot
