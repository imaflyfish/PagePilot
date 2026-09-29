#include <cstdlib>
#include <iostream>
#include <pagepilot/tool_runtime.hpp>
using namespace pagepilot;
namespace {
unsigned checks = 0;
void check(bool condition, const std::string &label) {
  if (!condition)
    throw BridgeError(label);
  ++checks;
}
template <class Function>
void rejects(Function action, const std::string &label) {
  try {
    action();
  } catch (const BridgeError &) {
    ++checks;
    return;
  }
  throw BridgeError(label);
}
} // namespace
int main(int argc, char **argv) {
  if (argc != 2)
    return 2;
  try {
    const auto *fixture = std::getenv("PAGEPILOT_FIXTURE_URL");
    if (!fixture)
      throw BridgeError("missing owned fixture URL");
    const std::string site = fixture;
    ToolRuntime runtime(static_cast<unsigned>(std::stoul(argv[1])));
    ToolCatalog catalog;
    auto call = [&](const std::string &name,
                    JsonDoc arguments = JsonDoc::object()) {
      return runtime.invoke(catalog.resolve(name, arguments, true));
    };
    auto eval = [&](const std::string &expression) {
      return call("page_evaluate", {{"script", expression}}).at("result");
    };
    call("tab_create");
    call("page_navigate", {{"url", site + "/page.html"}});
    auto vectors =
        eval("fetch(" + JsonDoc(site + "/keyboard-values.json").dump() +
             ").then(r=>r.json())")
            .at("vectors");
    eval(R"JS(window.keys=[];window.suppressKeys=true;
      for(const type of ['keydown','keyup'])document.addEventListener(type,e=>{
        keys.push({type:e.type,key:e.key,code:e.code,location:e.location,keyCode:e.keyCode,
          shift:e.shiftKey,ctrl:e.ctrlKey,alt:e.altKey,meta:e.metaKey,trusted:e.isTrusted});
        if(suppressKeys)e.preventDefault();
      });true)JS");
    call("element_focus", {{"selector", "#person"}});
    for (const auto &vector : vectors) {
      eval("keys=[];true");
      const auto key = vector.at("key").get<std::string>();
      call("keyboard_press", {{"key", key}});
      auto actual = eval("keys");
      if (vector.at("error") == true) {
        check(key == "F13" && actual.size() == 2 &&
                  actual[0].at("key") == "F13",
              "F13 is a deliberate extension beyond baseline F1..F12");
      } else {
        auto expected = vector.at("events");
        // Baseline omitted isKeypad on release, so Chrome reported location 1.
        // The native implementation preserves physical keypad location 3.
        for (auto &event : expected)
          if (event.at("type") == "keyup" &&
              event.at("code").get<std::string>().starts_with("Numpad"))
            event["location"] = 3;
        check(actual == expected,
              "actual baseline keyboard event contract: " + key +
                  " expected=" + expected.dump() + " actual=" + actual.dump());
      }
    }
    std::vector<std::string> scans = {"Escape",
                                      "Backquote",
                                      "Minus",
                                      "Equal",
                                      "Backslash",
                                      "Backspace",
                                      "Tab",
                                      "BracketLeft",
                                      "BracketRight",
                                      "CapsLock",
                                      "Semicolon",
                                      "Quote",
                                      "Enter",
                                      "ShiftLeft",
                                      "ShiftRight",
                                      "ControlLeft",
                                      "ControlRight",
                                      "MetaLeft",
                                      "MetaRight",
                                      "AltLeft",
                                      "AltRight",
                                      "Space",
                                      "AltGraph",
                                      "ContextMenu",
                                      "PrintScreen",
                                      "ScrollLock",
                                      "Pause",
                                      "PageUp",
                                      "PageDown",
                                      "Insert",
                                      "Delete",
                                      "Home",
                                      "End",
                                      "ArrowLeft",
                                      "ArrowRight",
                                      "ArrowUp",
                                      "ArrowDown",
                                      "NumLock",
                                      "NumpadDivide",
                                      "NumpadMultiply",
                                      "NumpadSubtract",
                                      "NumpadAdd",
                                      "NumpadDecimal",
                                      "NumpadEnter",
                                      "Comma",
                                      "Period",
                                      "Slash"};
    for (char letter = 'A'; letter <= 'Z'; ++letter)
      scans.push_back("Key" + std::string(1, letter));
    for (int digit = 0; digit < 10; ++digit) {
      scans.push_back("Digit" + std::to_string(digit));
      scans.push_back("Numpad" + std::to_string(digit));
    }
    for (int key = 1; key <= 24; ++key)
      scans.push_back("F" + std::to_string(key));
    check(scans.size() == 117,
          "105 baseline physical names plus twelve function-key extensions");
    for (const auto &scan : scans) {
      eval("keys=[];true");
      call("keyboard_press", {{"key", scan}});
      const auto events = eval("keys");
      check(events.size() == 2 && events[0].at("type") == "keydown" &&
                events[1].at("type") == "keyup" &&
                events[0].at("trusted") == true &&
                events[1].at("trusted") == true &&
                events[0].at("code") == (scan == "AltGraph" ? "" : scan) &&
                events[1].at("shift") == false &&
                events[1].at("ctrl") == false && events[1].at("alt") == false &&
                events[1].at("meta") == false,
            "complete native press/release for " + scan);
    }
    eval("keys=[];true");
    call(
        "keyboard_press",
        {{"key", "KeyA"}, {"modifiers", JsonDoc::array({"Control", "Shift"})}});
    check(eval("keys") == vectors.back().at("events"),
          "modifier array emits the complete ordered chord");
    eval("keys=[];true");
    call("keyboard_chord", {{"keys", "ShiftLeft+ShiftRight+KeyA"}});
    check(
        eval("keys.length===6&&keys[4].shift===true&&keys[5].shift===false") ==
            true,
        "releasing one Shift side keeps the other held until its own release");
    eval("keys=[];true");
    call("press_key", {{"key", "+"}});
    check(eval("keys.length===2&&keys[0].key==='+'&&keys[0].code==='Equal'") ==
              true,
          "legacy key tool accepts a literal plus");
    for (const auto &key :
         {std::string("Shift+UnknownOwnedKey"), std::string("Control+"),
          std::string(""), std::string("F25"), std::string("中文")}) {
      eval("keys=[];true");
      rejects([&] { call("keyboard_chord", {{"keys", key}}); },
              "invalid chord should fail");
      check(eval("keys.length") == 0,
            "invalid chord is rejected before any modifier effect");
    }
    std::string excessive;
    for (int i = 0; i < 17; ++i)
      excessive += (i ? "+KeyA" : "KeyA");
    rejects([&] { call("keyboard_chord", {{"keys", excessive}}); },
            "chord count is bounded");
    check(eval("keys.length") == 0, "chord limit has no partial input");

    eval("suppressKeys=false;true");
    call("element_fill", {{"selector", "#person"}, {"text", ""}});
    for (const auto &key : {"a", "Shift+KeyB", "Shift+c", "Shift+Digit1", "+"})
      call("keyboard_chord", {{"keys", key}});
    const auto inserted = eval("document.querySelector('#person').value");
    check(inserted == "aBc!+", "physical shifted keys and literal aliases "
                               "insert distinct intended text: " +
                                   inserted.dump());
    call("keyboard_chord", {{"keys", "Shift+Numpad1"}});
    check(eval("document.querySelector('#person').value") == inserted,
          "keypad End virtual key retains the observed baseline editor effect "
          "even when key is 1");
    call("keyboard_chord", {{"keys", "ControlOrMeta+A"}});
    call("keyboard_press", {{"key", "Backspace"}});
    check(eval("document.querySelector('#person').value") == "",
          "native select-all/delete editor effect");
#ifdef __APPLE__
    call("element_fill", {{"selector", "#person"}, {"text", "alpha beta"}});
    eval("document.querySelector('#person').setSelectionRange(10,10);true");
    call("keyboard_chord", {{"keys", "Alt+Backspace"}});
    check(eval("document.querySelector('#person').value") == "alpha ",
          "macOS word deletion command");
    call("keyboard_chord", {{"keys", "Control+KeyA"}});
    check(eval("document.querySelector('#person').selectionStart") == 0,
          "macOS Control+A moves to paragraph start rather than selecting all");
    call("keyboard_chord", {{"keys", "Meta+ArrowRight"}});
    check(eval("document.querySelector('#person').selectionStart") == 6,
          "macOS command-arrow moves to line end");
#endif
    call("tab_close");
    std::cout << checks << " keyboard checks passed; 0 failed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
