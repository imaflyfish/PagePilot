#include <pagepilot/key_input.hpp>
#include <embedded_resources.hpp>
#include <array>
#include <map>
#include <set>
namespace pagepilot {
namespace {
struct KeyChord {
  std::string scan, plain, shifted, text;
  int virtual_key = 0, position = 0;
};
const std::map<std::string, KeyChord> &layout() {
  static const auto entries = [] {
    std::map<std::string, KeyChord> result;
    const auto source = parse_message(resources::keys);
    for (const auto &row : source.at("keys")) {
      KeyChord pattern{row.at("scan"), row.at("plain"), row.at("shift"),
                         row.at("text"), row.at("virtual"), row.at("position")};
      result[pattern.scan] = pattern;
      auto literal = pattern;
      literal.shifted.clear();
      if (!pattern.position && pattern.plain.size() == 1)
        result[pattern.plain] = literal;
      if (pattern.position == 1)
        result[pattern.plain] = literal;
      if (!pattern.position && !pattern.shifted.empty()) {
        literal.plain = literal.text = pattern.shifted;
        result[pattern.shifted] = literal;
      }
    }
    result["\n"] = result["\r"] = result.at("Enter");
    result["Ctrl"] = result.at("Control");
    result["Command"] = result.at("Meta");
    for (int i = 13; i <= 24; ++i) {
      const auto name = "F" + std::to_string(i);
      result[name] = {name, name, {}, {}, 111 + i, 0};
    }
    return result;
  }();
  return entries;
}
unsigned modifier(const std::string &key) {
  return key == "Alt" ? 1 : key == "Control" ? 2 : key == "Meta" ? 4
                                             : key == "Shift" ? 8 : 0;
}
JsonDoc editing(const std::string &scan, unsigned mask) {
#ifdef __APPLE__
  static const auto bindings = [] {
    std::map<std::pair<std::string, unsigned>, JsonDoc> result;
    const auto source = parse_message(resources::editing);
    for (const auto &row : source.at("commands"))
      result[{row.at("scan").get<std::string>(), row.at("mask").get<unsigned>()}] = row.at("actions");
    return result;
  }();
  const auto found = bindings.find({scan, mask});
  if (found != bindings.end()) return found->second;
#else
  if (scan == "KeyA" && mask == 2) return JsonDoc::array({"selectAll"});
#endif
  return JsonDoc::array();
}
} // namespace
std::vector<KeyEvent> plan_keyboard(const std::string &combination) {
  if (combination.empty() || combination.size() > 1024)
    throw BridgeError("Keyboard chord must contain 1 to 1024 bytes");
  std::vector<std::string> names;
  std::string piece;
  for (const char value : combination) {
    if (value == '+' && !piece.empty()) {
      names.push_back(std::move(piece)); piece.clear();
    } else piece += value;
  }
  names.push_back(std::move(piece));
  if (names.size() > 16)
    throw BridgeError("Keyboard chord exceeds 16 keys");
  std::vector<KeyChord> patterns;
  for (auto name : names) {
    if (name == "ControlOrMeta") {
#ifdef __APPLE__
      name = "Meta";
#else
      name = "Control";
#endif
    }
    const auto found = layout().find(name);
    if (found == layout().end())
      throw BridgeError("Unsupported keyboard key: " + name);
    patterns.push_back(found->second);
  }
  std::array<unsigned, 16> held{};
  auto modifiers = [&] {
    unsigned result = 0;
    for (unsigned flag : {1U, 2U, 4U, 8U})
      if (held[flag]) result |= flag;
    return result;
  };
  std::set<std::string> pressed_scans;
  std::vector<KeyEvent> strokes;
  for (auto &pattern : patterns) {
    if ((modifiers() & 8) && !pattern.shifted.empty())
      pattern.plain = pattern.text = pattern.shifted;
    if (const auto flag = modifier(pattern.plain)) ++held[flag];
    const auto mask = modifiers();
    const auto text = mask & 7 ? std::string() : pattern.text;
    JsonDoc event = {{"type", text.empty() ? "rawKeyDown" : "keyDown"},
                  {"key", pattern.plain}, {"code", pattern.scan},
                  {"windowsVirtualKeyCode", pattern.virtual_key},
                  {"location", pattern.position}, {"isKeypad", pattern.position == 3},
                  {"modifiers", mask}, {"text", text}, {"unmodifiedText", text},
                  {"autoRepeat", !pressed_scans.insert(pattern.scan).second},
                  {"commands", editing(pattern.scan, mask)}};
    strokes.push_back({std::move(event), JsonDoc::object()});
  }
  for (std::size_t i = strokes.size(); i-- > 0;) {
    if (const auto flag = modifier(patterns[i].plain)) --held[flag];
    auto release = strokes[i].pressed;
    release["type"] = "keyUp";
    release["modifiers"] = modifiers();
    for (const char *key : {"text", "unmodifiedText", "autoRepeat", "commands"})
      release.erase(key);
    strokes[i].released = std::move(release);
  }
  return strokes;
}
} // namespace pagepilot
