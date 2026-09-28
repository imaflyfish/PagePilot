#include <algorithm>
#include <cctype>
#include <cerrno>
#include <pagepilot/core.hpp>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <set>
namespace pagepilot {
JsonDoc parse_message(const std::string &text, std::size_t limit) {
  if (text.size() > limit)
    throw BridgeError("JSON message exceeds size limit");
  std::vector<std::set<std::string>> objects;
  std::size_t nodes = 0;
  auto callback = [&](int depth, JsonDoc::parse_event_t event, JsonDoc &value) {
    if (depth > 64 || ++nodes > 1000000)
      throw BridgeError("JSON structural limit exceeded");
    if (event == JsonDoc::parse_event_t::object_start)
      objects.emplace_back();
    if (event == JsonDoc::parse_event_t::key &&
        !objects.back().insert(value.get<std::string>()).second)
      throw BridgeError("duplicate JSON property");
    if (event == JsonDoc::parse_event_t::object_end)
      objects.pop_back();
    return true;
  };
  try {
    return JsonDoc::parse(text, callback);
  } catch (const JsonDoc::exception &error) {
    throw BridgeError(std::string("invalid JSON: ") + error.what());
  }
}
namespace {
JsonDoc normalize(JsonDoc value, const JsonDoc &schema, unsigned depth,
               const std::string &path) {
  if (depth > 32)
    throw BridgeError("argument nesting limit exceeded");
  if (!schema.is_object())
    throw BridgeError("invalid action schema");
  if (schema.contains("oneOf")) {
    unsigned matches = 0;
    JsonDoc selected;
    const auto &choices = schema.at("oneOf");
    if (!choices.is_array() || choices.empty() || choices.size() > 16)
      throw BridgeError("invalid schema alternatives");
    for (const auto &choice : choices) {
      try {
        auto candidate = normalize(value, choice, depth + 1, path);
        selected = std::move(candidate);
        ++matches;
      } catch (const BridgeError &) {
      }
    }
    if (matches != 1)
      throw BridgeError(path + " must satisfy exactly one input shape");
    value = std::move(selected);
  }
  const auto kind = schema.value("type", std::string());
  if ((kind == "number" || kind == "integer") && value.is_string()) {
    const auto text = value.get<std::string>();
    if (text.empty() || text.size() > 128 ||
        text.find('\0') != std::string::npos)
      throw BridgeError(path + " is not a finite number");
    errno = 0;
    char *end = nullptr;
    const auto number = std::strtod(text.c_str(), &end);
    const bool converted = end && end != text.c_str();
    while (end && *end && std::isspace(static_cast<unsigned char>(*end)))
      ++end;
    if (!converted || !end || *end || errno == ERANGE || !std::isfinite(number))
      throw BridgeError(path + " is not a finite number");
    value = number;
  }
  bool valid = kind.empty() || (kind == "object" && value.is_object()) ||
               (kind == "array" && value.is_array()) ||
               (kind == "string" && value.is_string()) ||
               (kind == "boolean" && value.is_boolean()) ||
               ((kind == "number" || kind == "integer") && value.is_number()) ||
               (kind == "null" && value.is_null());
  if (!valid)
    throw BridgeError(path + " requires " + kind);
  if (value.is_number()) {
    const auto number = value.get<double>();
    if (!std::isfinite(number) ||
        (kind == "integer" && std::trunc(number) != number))
      throw BridgeError(path + " has an invalid number");
    if (schema.contains("minimum") &&
        number < schema.at("minimum").get<double>())
      throw BridgeError(path + " is below minimum");
    if (schema.contains("maximum") &&
        number > schema.at("maximum").get<double>())
      throw BridgeError(path + " is above maximum");
  }
  if (schema.contains("enum")) {
    const auto &allowed = schema.at("enum");
    if (!allowed.is_array() ||
        std::find(allowed.begin(), allowed.end(), value) == allowed.end())
      throw BridgeError(path + " is not an allowed value");
  }
  if (value.is_string() &&
      value.get_ref<const std::string &>().size() > 4 * 1024 * 1024)
    throw BridgeError(path + " string is too long");
  if (value.is_array()) {
    const auto maximum = schema.value("maxItems", std::size_t{10000});
    if (value.size() > maximum)
      throw BridgeError(path + " has too many items");
    if (schema.contains("items"))
      for (std::size_t i = 0; i < value.size(); ++i)
        value[i] = normalize(value[i], schema.at("items"), depth + 1,
                             path + "[" + std::to_string(i) + "]");
  }
  if (value.is_object()) {
    if (value.size() > 10000)
      throw BridgeError(path + " has too many properties");
    const auto properties = schema.value("properties", JsonDoc::object());
    for (const auto &key : schema.value("required", JsonDoc::array()))
      if (!value.contains(key.get<std::string>()))
        throw BridgeError(path + " requires property " + key.get<std::string>());
    for (auto &[key, child] : value.items()) {
      if (properties.contains(key))
        child =
            normalize(child, properties.at(key), depth + 1, path + "." + key);
      else if (schema.contains("additionalProperties")) {
        const auto &additional = schema.at("additionalProperties");
        if (additional.is_boolean() && !additional.get<bool>())
          throw BridgeError(path + " has unknown property " + key);
        if (additional.is_object())
          child = normalize(child, additional, depth + 1, path + "." + key);
      }
    }
  }
  return value;
}
} // namespace
JsonDoc normalize_arguments(const JsonDoc &arguments, const JsonDoc &schema) {
  return normalize(arguments.is_null() ? JsonDoc::object() : arguments, schema, 0,
                   "arguments");
}
std::string read_text(const std::filesystem::path &path, std::size_t maximum) {
  const auto size = std::filesystem::file_size(path);
  if (size > maximum)
    throw BridgeError("input file is too large");
  std::ifstream input(path, std::ios::binary);
  if (!input)
    throw BridgeError("cannot open input file");
  std::string output(static_cast<std::size_t>(size), '\0');
  if (!input.read(output.data(), static_cast<std::streamsize>(size)))
    throw BridgeError("incomplete file read");
  return output;
}
} // namespace pagepilot
