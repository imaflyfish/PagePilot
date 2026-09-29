#include "../support/checks.hpp"
#include <iostream>
#include <pagepilot/key_input.hpp>
#include <pagepilot/tool_catalog.hpp>
using namespace pagepilot;
using namespace pagepilot_tests;
int main() {
  try {
    // The keyboard layout and the editing commands are embedded at build time
    // and parsed the first time a chord is planned, which until now happened
    // only with a browser attached. Planning one here means a malformed table
    // fails the contract suite instead of the live suite.
    const auto chord = plan_keyboard("Control+Shift+KeyA");
    check(chord.size() == 3, "chord expands to one stroke per key");
    check(chord.back().pressed.at("code") == "KeyA" &&
              chord.back().pressed.at("modifiers") == 10,
          "the last key is pressed with both modifiers held");
    check(chord.front().released.at("type") == "keyUp" &&
              chord.front().released.at("modifiers") == 0,
          "the first key is released last, after the others");
    rejects([] { (void)plan_keyboard("NoSuchKey"); },
            "an unknown key name is refused");
    ToolCatalog catalog;
    check(catalog.list().size() == 54, "canonical catalog size");
    check(catalog.list(true).size() == 75, "legacy catalog size");
    check(catalog.resolve("element_fill", {{"selector", "#a"},
                                           {"text", "value"},
                                           {"field", "#b"},
                                           {"targetText", true},
                                           {"inputIndex", true}})
                  .arguments ==
              JsonDoc({{"selector", "#a"}, {"text", "value"}}),
          "public arguments cannot inject private DOM adapter fields");
    check(catalog.resolve("form_fill",
                          {{"fields",
                            {{"__proto__", "literal"}, {"field", "value"}}}})
                  .arguments.at("fields")
                  .at("__proto__") == "literal",
          "unknown root filtering preserves user-owned nested field names");
    check(
        catalog.resolve("page_navigate", {{"url", "about:blank"}}).operation ==
            "navigate",
        "canonical operation resolves");
    check(catalog.resolve("get_text", {{"selector", "h1"}}, true)
                  .arguments.at("type") == "text",
          "alias carries explicit operation preset");
    rejects([&] { catalog.resolve("get_text", {{"selector", "h1"}}); },
            "legacy mode is explicit");
    rejects([&] { catalog.resolve("constructor", JsonDoc::object(), true); },
            "unknown operation rejected");
    rejects([&] { catalog.resolve("page_navigate", JsonDoc::object()); },
            "required property checked");
    rejects([&] { catalog.resolve("browser_debug", {{"enabled", "false"}}); },
            "boolean string is not truthy coercion");
    check(catalog.resolve("page_wait", {{"type", "time"}, {"timeout", "3000"}})
                  .arguments.at("timeout") == 3000,
          "numeric strings normalized");
    rejects([&] { catalog.resolve("page_wait", {{"timeout", "nan"}}); },
            "nonfinite numeric strings rejected");
    rejects([&] { catalog.resolve("page_wait", {{"timeout", " \t\n"}}); },
            "whitespace-only number rejected");
    check(catalog.resolve("page_wait", {{"timeout", " 50 \t"}})
                  .arguments.at("timeout") == 50,
          "whitespace around numeric value accepted");
    rejects([&] { catalog.resolve("page_wait", {{"timeout", 60001}}); },
            "timeout range enforced");
    rejects([&] { catalog.resolve("tab_activate", {{"index", 1.5}}); },
            "fractional index rejected");
    check(catalog.resolve("form_upload",
                          {{"selector", "input"},
                           {"files", JsonDoc::array({"/tmp/a", "/tmp/b"})}})
                  .arguments.at("files")
                  .size() == 2,
          "oneOf array accepted");
    rejects(
        [&] {
          catalog.resolve("form_upload", {{"selector", "input"},
                                          {"files", JsonDoc::array({1})}});
        },
        "oneOf nested item checked");
    rejects(
        [&] {
          catalog.resolve("form_upload",
                          {{"selector", "input"}, {"files", true}});
        },
        "oneOf wrong type refused");
    rejects(
        [&] {
          catalog.resolve(
              "workflow_batch",
              {{"actions", JsonDoc::array({{{"args", JsonDoc::object()}}})}});
        },
        "nested composition requires tool name");
    rejects([&] { parse_message("{\"a\":1,\"a\":2}"); },
            "duplicate JSON keys rejected");
    rejects(
        [&] {
          parse_message(std::string(70, '[') + "0" + std::string(70, ']'));
        },
        "JSON depth bounded");
    rejects([&] { parse_message("{}", 1); }, "JSON size bounded");
    check(parse_message("{\"value\":\"中文😀\"}").at("value") == "中文😀",
          "UTF-8 roundtrip");
    check(catalog.resolve("workflow_steps", {{"steps", JsonDoc::array()},
                                             {"max_step_retries", 0}})
                  .arguments.at("max_step_retries") == 0,
          "zero retries remains zero");
    rejects(
        [&] {
          catalog.resolve("workflow_steps", {{"steps", JsonDoc::array()},
                                             {"max_step_retries", 11}});
        },
        "step retry maximum");
    std::cout << passed << " foundation checks passed; " << failed
              << " failed\n";
    return failed ? 1 : 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
