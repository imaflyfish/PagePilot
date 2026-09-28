#include <pagepilot/tool_runtime.hpp>
#include <cstdlib>
#include <iostream>
using namespace pagepilot;
unsigned passed = 0, failed = 0;
void check(bool value, const char *name) {
  if (value)
    ++passed;
  else {
    ++failed;
    std::cerr << "FAIL: " << name << '\n';
  }
}
template <class F> void rejects(F action, const char *name) {
  try {
    action();
    check(false, name);
  } catch (const std::exception &) {
    check(true, name);
  }
}
int main(int argc, char **argv) {
  if (argc != 2)
    return 2;
  try {
    const auto *site = std::getenv("PAGEPILOT_FIXTURE_URL");
    if (!site)
      throw BridgeError("missing fixture URL");
    ToolRuntime runtime(static_cast<unsigned>(std::stoul(argv[1])));
    ToolCatalog catalog;
    auto call = [&](const std::string &name, JsonDoc args = JsonDoc::object()) {
      return runtime.invoke(catalog.resolve(name, args, true));
    };
    auto evaluate = [&](const std::string &source) {
      return call("page_evaluate", {{"script", source}}).at("result");
    };
    auto value = [&](const std::string &selector) {
      return call("element_read", {{"selector", selector}, {"type", "value"}})
          .at("value");
    };
    call("tab_create");
    call("page_navigate", {{"url", std::string(site) + "/page.html"}});
    evaluate(R"JS(
      window.proof=[];
      for(const kind of ['input','change','keydown','keyup','mousedown','mouseup','dblclick','contextmenu'])
        document.addEventListener(kind,e=>{proof.push({kind,id:e.target.id,trusted:e.isTrusted,key:e.key,value:e.target.value});if(kind==='contextmenu')e.preventDefault()});
      document.querySelector('#person').name='personName';
      document.body.insertAdjacentHTML('beforeend','<div id="editable" contenteditable="true">Before</div><input id="limited" maxlength="3"><input id="readonly" readonly value="untouched"><input id="date" type="date"><button id="disabled" disabled>Disabled</button><div id="scrollbox" style="width:100px;height:50px;overflow:scroll"><div style="height:500px">Scroll</div></div><p class="repeated">first</p><p class="repeated">second</p><div id="shadow-host"></div>');
      document.querySelector('#shadow-host').attachShadow({mode:'open'}).innerHTML='<button id="shadow-button">Shadow action</button><input id="shadow-input" aria-label="Shadow label">';
      true
    )JS");
    check(call("element_count", {{"selector", ".repeated"}}).at("count") == 2,
          "CSS matches all elements");
    check(call("element_read",
               {{"selector", "xpath=//input[@id='person']"}, {"type", "tag"}})
                  .at("tag") == "input",
          "XPath resolves element");
    check(value("id=person") == "initial", "id shortcut");
    check(value("data-testid=person") == "initial", "test-id shortcut");
    check(value("placeholder=Your name") == "initial", "placeholder shortcut");
    check(value("label=Display name") == "initial",
          "associated label shortcut");
    check(value("label=Notes") == "", "ARIA label shortcut");
    check(call("element_read",
               {{"selector", "role=button[name=\"Count clicks\"]"},
                {"type", "text"}})
                  .at("text") == "Count clicks",
          "role and accessible name select intended button");
    evaluate(
        "document.body.insertAdjacentHTML('beforeend','<button "
        "id=ax-image><img alt=\"Image supplied name\"></button><button "
        "id=ax-hidden aria-hidden=true>AX excluded</button><span id=ax-label "
        "hidden>Labelled by hidden text</span><button id=ax-labelled "
        "aria-labelledby=ax-label>Other text</button>');true");
    check(call("element_find",
               {{"selector", "role=button[name=\"Image supplied name\"]"}})
                  .at("elements")[0]
                  .at("id") == "ax-image",
          "role name includes child image alternative");
    check(call("element_count",
               {{"selector", "role=button[name=\"AX excluded\"]"}})
                  .at("count") == 0,
          "role selector excludes aria-hidden nodes");
    check(call("element_read",
               {{"selector", "role=button[name=\"Labelled by hidden text\"]"},
                {"type", "attribute"},
                {"attribute", "id"}})
                  .at("id") == "ax-labelled",
          "role name follows aria-labelledby even to hidden label");
    check(call("element_read",
               {{"selector", "text=Bottom marker"}, {"type", "text"}})
                  .at("text") == "Bottom marker",
          "text selects deepest match");
    check(call("element_count", {{"selector", "#shadow-button"}}).at("count") ==
              1,
          "open shadow root queried");
    check(call("element_check", {{"selector", "#hidden"}, {"state", "hidden"}})
                  .at("hidden") == true,
          "hidden element state");
    check(call("element_check", {{"selector", "#missing"}, {"state", "hidden"}})
                  .at("hidden") == true,
          "missing element is hidden");
    check(call("element_check",
               {{"selector", "#disabled"}, {"state", "disabled"}})
                  .at("disabled") == true,
          "native disabled state");
    check(call("element_check",
               {{"selector", "#readonly"}, {"state", "editable"}})
                  .at("editable") == false,
          "readonly is not editable");
    check(
        call("element_check", {{"selector", ".repeated"}, {"state", "exists"}})
                .at("count") == 2,
        "state count retains all matches");
    check(call("element_check", {{"text", "Bottom marker"}}).at("exists") ==
              true,
          "check text shortcut");
    check(call("element_check",
               {{"selector", "#hidden"}, {"class_name", "hidden"}})
                  .at("has_class") == true,
          "class inspection");
    check(
        call("element_find", {{"text", "second"}, {"tag", "p"}}).at("total") ==
            1,
        "find filters text and tag");
    check(call("element_find",
               {{"attribute", "data-testid"}, {"value", "person"}})
                  .at("elements")[0]
                  .at("id") == "person",
          "find attribute equality");
    check(call("element_find", {{"selector", "p"}, {"limit", 0}}).at("found") ==
              0,
          "find limit zero preserved");
    rejects([&] { call("element_count", {{"selector", "]invalid"}}); },
            "invalid CSS reports an error");
    check(call("element_read", {{"selector", "#person"},
                                {"type", "attribute"},
                                {"attribute", "data-testid"}})
                  .at("data-testid") == "person",
          "attribute result retains key");
    check(call("element_read", {{"selector", "#person"}, {"type", "dataset"}})
                  .at("dataset")
                  .at("testid") == "person",
          "dataset read");
    check(call("element_read", {{"selector", "#hidden"}, {"type", "classes"}})
                  .at("classes") == JsonDoc::array({"hidden"}),
          "class list read");
    check(call("element_read",
               {{"selector", "#person"}, {"type", "bounding_box"}})
                  .at("width")
                  .get<double>() > 0,
          "real bounds returned");
    check(call("element_read", {{"selector", "#hidden"},
                                {"type", "styles"},
                                {"properties", JsonDoc::array({"display"})}})
                  .at("styles")
                  .at("display") == "none",
          "computed style read");
    check(call("element_read",
               {{"selector", "#person"}, {"type", "html"}, {"outer", true}})
              .at("html")
              .get<std::string>()
              .starts_with("<input"),
          "outer HTML read");

    call("element_fill",
         {{"selector", "#person"}, {"text", "中文😀 !@#$\\'\""}});
    check(value("#person") == "中文😀 !@#$\\'\"",
          "fill keeps Unicode and literal punctuation");
    check(evaluate(
              "proof.some(e=>e.kind==='input'&&e.id==='person'&&e.trusted)") ==
              true,
          "fill generates trusted browser input event");
    call("element_type",
         {{"label", "Display name"}, {"text", " plus"}, {"clear", false}});
    check(value("#person") == "中文😀 !@#$\\'\" plus",
          "type appends without replacing value");
    call("element_type", {{"placeholder", "Your name"},
                          {"text", "慢😀文"},
                          {"mode", "slow"},
                          {"delay", 1}});
    check(value("#person") == "慢😀文",
          "paced input preserves supplementary Unicode");
    call("element_type", {{"selector", "#person"},
                          {"text", "Human"},
                          {"mode", "human"},
                          {"delay", 1}});
    check(value("#person") == "Human",
          "human input preserves exact intended text");
    call("element_type",
         {{"selector", "#person"}, {"text", ""}, {"clear", false}});
    check(value("#person") == "Human",
          "appending an empty string does not delete a character");
    call("element_fill", {{"selector", "#person"}, {"text", ""}});
    check(value("#person") == "", "empty fill clears selected value");
    call("element_fill", {{"selector", "#limited"}, {"text", "12345"}});
    check(value("#limited") == "123", "native maxlength honored");
    call("element_fill", {{"selector", "#editable"}, {"text", "Editable😀"}});
    check(value("#editable") == "Editable😀", "contenteditable replacement");
    call("element_fill", {{"selector", "#date"}, {"text", "2026-09-23"}});
    check(value("#date") == "2026-09-23", "date control assignment");
    call("element_fill",
         {{"selector", "#shadow-input"}, {"text", "Shadow text"}});
    check(value("#shadow-input") == "Shadow text",
          "trusted input in shadow root");
    const auto preview =
        call("element_fill", {{"selector", "#notes"},
                              {"text", std::string(49, 'x') + "😀tail"}});
    check(!preview.dump().empty() &&
              value("#notes") == std::string(49, 'x') + "😀tail",
          "preview truncation keeps UTF-8 valid at byte boundary");
    call("element_type", {{"index", 0}, {"text", "Indexed"}});
    check(value("#person") == "Indexed", "visible input index");
    check(call("element_type", {{"selector", "#not-there"},
                                {"text", "skip"},
                                {"if_exists", true}})
                  .at("typed") == false,
          "conditional absent input does not type elsewhere");
    const auto before = std::chrono::steady_clock::now();
    rejects(
        [&] {
          call("element_fill", {{"selector", "#readonly"},
                                {"text", "changed"},
                                {"timeout", 80}});
        },
        "readonly input times out");
    check(std::chrono::steady_clock::now() - before < std::chrono::seconds(1),
          "element timeout has a bounded wall clock");
    check(value("#readonly") == "untouched", "readonly value preserved");
    call("element_focus", {{"selector", "#person"}});
    check(call("element_check", {{"selector", "#person"}, {"state", "focused"}})
                  .at("focused") == true,
          "focus reaches intended element");
    call("keyboard_chord", {{"keys", "ControlOrMeta+a"}});
    call("keyboard_press", {{"key", "Backspace"}});
    check(value("#person") == "", "keyboard select-all and delete");
    call("keyboard_press",
         {{"key", "KeyX"}, {"modifiers", JsonDoc::array({"Shift"})}});
    check(value("#person") == "X", "modified key inserts correct case");
    call("element_blur", {{"selector", "#person"}});
    check(call("element_check", {{"selector", "#person"}, {"state", "focused"}})
                  .at("focused") == false,
          "blur removes focus");
    check(
        evaluate("proof.some(e=>e.kind==='keydown'&&e.key==='X'&&e.trusted)") ==
            true,
        "keyboard events are native and trusted");
    rejects([&] { call("keyboard_press", {{"key", "MadeUpKey"}}); },
            "unsupported key does not become text silently");

    call("page_scroll", {{"to", "top"}});
    call("element_click", {{"selector", "#count-button"}});
    check(evaluate("document.querySelector('#clicks').textContent") == "1",
          "native click changes actual page state");
    check(evaluate("proof.some(e=>e.kind==='mousedown'&&e.id==='count-button'&&"
                   "e.trusted)") == true,
          "trusted mouse event");
    call("element_click", {{"text", "Count clicks"}, {"type", "double"}});
    check(
        evaluate("proof.some(e=>e.kind==='dblclick'&&e.id==='count-button')") ==
            true,
        "double click dispatches dblclick");
    call("element_click", {{"selector", "#count-button"}, {"type", "right"}});
    check(evaluate(
              "proof.some(e=>e.kind==='contextmenu'&&e.id==='count-button')") ==
              true,
          "right click dispatches contextmenu");
    call("element_click",
         {{"selector", "#count-button"}, {"type", "long"}, {"duration", 5}});
    call("element_click", {{"selector", "#count-button"}, {"mode", "smart"}});
    call("element_click", {{"selector", "#count-button"}, {"mode", "human"}});
    check(evaluate("Number(document.querySelector('#clicks').textContent)") ==
              6,
          "long and paced clicks each fire once");
    check(call("element_click", {{"selector", "#missing"}, {"if_exists", true}})
                  .at("clicked") == false,
          "absent conditional click skipped");
    rejects(
        [&] {
          call("element_click", {{"selector", "#disabled"}, {"timeout", 80}});
        },
        "disabled target is not clicked");
    evaluate("document.body.insertAdjacentHTML('beforeend','<div id=cover "
             "style=\"position:fixed;inset:0;z-index:9999\"></div>');true");
    rejects(
        [&] {
          call("element_click",
               {{"selector", "#count-button"}, {"timeout", 80}});
        },
        "occluded target is not force clicked");
    evaluate("document.querySelector('#cover').remove();true");
    call("element_hover", {{"selector", "#count-button"}});
    check(
        evaluate("document.querySelector('#count-button').matches(':hover')") ==
            true,
        "hover reaches target");
    check(call("form_check", {{"selector", "#check"}, {"checked", true}})
                  .at("checked") == true,
          "checkbox becomes checked");
    check(call("set_checked", {{"selector", "#check"}, {"checked", true}})
                  .at("checked") == true,
          "setting same checkbox state is idempotent");
    check(call("toggle_checkbox", {{"selector", "#check"}}).at("checked") ==
              false,
          "legacy toggle changes checkbox state");
    rejects(
        [&] {
          call("form_check", {{"selector", "#person"}, {"checked", false}});
        },
        "checkbox operation rejects an ordinary text input");
    call("form_select", {{"selector", "#color"}, {"value", "g"}});
    check(value("#color") == "g", "select by value");
    call("form_select", {{"selector", "#color"}, {"index", 2}});
    check(value("#color") == "b", "select by option index");
    call("select_option", {{"selector", "#color"}, {"value", "r"}});
    call("form_select", {{"selector", "#color"}, {"text", "Green"}});
    check(value("#color") == "g", "select by visible label and legacy alias");
    check(evaluate("proof.some(e=>e.kind==='change'&&e.id==='color'&&e.value==="
                   "'g')") == true,
          "select dispatches change event");
    rejects(
        [&] {
          call("form_select", {{"selector", "#color"}, {"value", "missing"}});
        },
        "nonexistent option refused");
    const auto form = call("form_fill", {{"fields",
                                          {{"Display name", "By label"},
                                           {"#notes", "By selector"},
                                           {"personName", "By name"}}}});
    check(form.at("filled") == 3 && value("#person") == "By name" &&
              value("#notes") == "By selector",
          "form fields resolve label, selector and name");

    evaluate("setTimeout(()=>{const "
             "n=document.createElement('button');n.id='late';n.textContent='"
             "Delayed button';document.body.append(n)},80);true");
    check(
        call("page_wait",
             {{"selector", "#late"}, {"state", "attached"}, {"timeout", 1000}})
                .at("found") == true,
        "wait observes delayed DOM insertion");
    evaluate(
        "setTimeout(()=>document.querySelector('#late').remove(),60);true");
    check(
        call("wait_for",
             {{"selector", "#late"}, {"state", "detached"}, {"timeout", 1000}})
                .at("found") == true,
        "legacy wait observes detachment");
    check(call("page_wait", {{"type", "text"}, {"text", "Loaded later"}})
                  .at("found") == true,
          "text wait");
    check(
        call("page_wait",
             {{"type", "function"},
              {"expression",
               "() => document.querySelector('#clicks').textContent === '6'"}})
                .at("condition") == true,
        "function wait invokes predicate");
    check(call("page_wait", {{"type", "url"}, {"pattern", "**/page.html"}})
                  .at("matched") == true,
          "URL glob wait");
    check(call("page_wait", {{"ms", 0}}).at("waited") == 0,
          "zero wait preserved");
    check(call("page_wait", {{"type", "time"}, {"timeout", 0}}).at("waited") ==
              0,
          "zero time-type timeout is a zero delay");
    rejects([&] { call("page_wait", {{"ms", -1}}); },
            "negative delay rejected");
    rejects(
        [&] { call("page_wait", {{"selector", "#never"}, {"timeout", 60}}); },
        "missing wait times out");
    check(call("page_assert",
               {{"type", "count"}, {"selector", ".repeated"}, {"expected", 2}})
                  .at("passed") == true,
          "positive count assertion");
    check(call("page_assert", {{"type", "count"},
                               {"selector", ".repeated"},
                               {"expected", 3},
                               {"operator", ">="}})
                  .at("passed") == false,
          "negative count assertion returns false");
    check(call("page_assert", {{"type", "element"},
                               {"selector", "#check"},
                               {"state", "checked"}})
                  .at("passed") == false,
          "failed element assertion remains a failed result");
    check(call("page_assert", {{"type", "url"}, {"pattern", "page\\.html$"}})
                  .at("passed") == true,
          "URL regex assertion");
    check(call("page_assert",
               {{"type", "visible"}, {"selector", "#never"}, {"timeout", 60}})
                  .at("passed") == false,
          "assertion timeout returned as failed assertion");
    call("page_scroll", {{"to", "#below"}});
    check(call("element_check",
               {{"selector", "#below"}, {"state", "in_viewport"}})
                  .at("in_viewport") == true,
          "scroll to selector reveals element");
    call("page_scroll", {{"within", "#scrollbox"}, {"by", {{"y", 100}}}});
    check(evaluate("document.querySelector('#scrollbox').scrollTop") == 100,
          "scroll inside control");
    call("page_scroll", {{"position", {{"x", 0}, {"y", 100}}}});
    call("page_scroll", {{"direction", "down"}, {"amount", 50}});
    check(evaluate("scrollY") == 150, "position and directional scroll");
    check(call("get_text", {{"selector", "h1"}}).at("text") ==
              "PagePilot local fixture",
          "legacy text alias dispatches real implementation");
    call("browser_configure", {{"fast_timeout", 80}});
    check(call("get_text",
               {{"selector", "#absent"}, {"fallback", "fallback text"}})
                  .at("text") == "fallback text",
          "missing read returns requested fallback");
    call("tab_close");
    std::cout << passed << " live-DOM checks passed; " << failed << " failed\n";
    return failed ? 1 : 0;
  } catch (const std::exception &error) {
    std::cerr << "UNCAUGHT: " << error.what() << '\n';
    return 1;
  }
}
