#include <pagepilot/tool_runtime.hpp>
#include <cstdlib>
#include <exception>
#include <iostream>
using namespace pagepilot;
unsigned passed = 0, failed = 0;
std::string step;
void check(bool condition, const char *label) {
  if (condition)
    ++passed;
  else {
    ++failed;
    std::cerr << "FAIL: " << label << '\n';
  }
}
template <class F> void rejects(F action, const char *label) {
  try {
    action();
    check(false, label);
  } catch (const std::exception &) {
    check(true, label);
  }
}
int main(int argc, char **argv) {
  if (argc != 2)
    return 2;
  try {
    const auto *site = std::getenv("PAGEPILOT_FIXTURE_URL");
    if (!site)
      throw BridgeError("Fixture URL missing");
    ToolRuntime runtime(static_cast<unsigned>(std::stoul(argv[1])));
    ToolCatalog catalog;
    auto call = [&](const std::string &name, JsonDoc arguments = JsonDoc::object()) {
      step = name + " " + arguments.dump();
      return runtime.invoke(catalog.resolve(name, arguments, true));
    };
    auto eval = [&](const std::string &expression) {
      return call("page_evaluate", {{"script", expression}}).at("result");
    };
    call("tab_create");
    call("page_navigate", {{"url", std::string(site) + "/page.html"}});
    eval("document.body.insertAdjacentHTML('beforeend','<input id=other "
         "value=untouched>');true");
    call("element_fill",
         {{"selector", "#person"}, {"text", "chosen"}, {"field", "#other"}});
    check(eval("document.querySelector('#person').value==='chosen'&&document."
               "querySelector('#other').value==='untouched'") == true,
          "unknown public field cannot redirect fill to another element");

    check(call("page_dialog").at("handler") == "accept",
          "dialog default arms acceptance");
    check(eval("confirm('once')") == true,
          "armed confirm accepted during pending evaluation");
    check(eval("confirm('again')") == false,
          "rule consumed once; unarmed dialog dismissed");
    call("page_dialog", {{"text", "你好😀\\\""}});
    check(eval("prompt('value','initial')") == "你好😀\\\"",
          "prompt text keeps Unicode and punctuation");
    call("page_dialog", {{"action", "dismiss"}});
    check(eval("prompt('dismiss')").is_null(), "prompt dismissal returns null");
    check(eval("alert('automatic');42") == 42,
          "unarmed alert cannot deadlock a tool");
    call("page_dialog", {{"action", "accept"}});
    eval("document.querySelector('#count-button').onclick=()=>{window."
         "dialogProof=confirm('click');};true");
    call("element_click", {{"selector", "#count-button"}});
    check(eval("dialogProof") == true,
          "native click completes through modal handler and input receipt");
    const auto first = runtime.browser().current_target();
    call("page_dialog", {{"action", "accept"}});
    call("tab_create", {{"url", std::string(site) + "/page.html"}});
    check(eval("confirm('other tab')") == false,
          "armed dialog belongs to its page");
    call("tab_close");
    auto tabs = call("tab_list").at("tabs");
    for (const auto &tab : tabs)
      if (tab.at("id") == first)
        call("tab_activate", {{"index", tab.at("index")}});
    check(eval("confirm('original tab')") == true,
          "switching pages preserves original one-shot rule");

    call("clear_storage");
    call("clear_storage", {{"type", "session"}});
    for (const auto &key : {std::string("中文😀"), std::string("__proto__"),
                            std::string("constructor"), std::string("")}) {
      call("set_storage", {{"key", key}, {"value", "literal '\"\\ value"}});
      check(call("get_storage", {{"key", key}}).at(key) ==
                "literal '\"\\ value",
            "literal storage key/value roundtrip");
    }
    const auto storage = call("get_storage").at("storage");
    check(storage.size() == 4 &&
              storage.at("__proto__") == "literal '\"\\ value",
          "all storage includes prototype-like keys");
    call("set_storage",
         {{"key", "scope"}, {"value", "tab"}, {"type", "session"}});
    check(call("get_storage", {{"key", "scope"}}).at("scope").is_null(),
          "session storage does not write local storage");
    check(call("get_storage", {{"key", "scope"}, {"type", "session"}})
                  .at("scope") == "tab",
          "session storage read");
    call("page_reload");
    check(call("get_storage", {{"key", "scope"}, {"type", "session"}})
                  .at("scope") == "tab",
          "session storage survives same-tab reload");
    call("page_storage", {{"action", "remove"}, {"key", "__proto__"}});
    check(call("get_storage").at("storage").size() == 3,
          "remove exact storage key");
    rejects([&] { call("page_storage", {{"action", "set"}}); },
            "storage write requires explicit key");
    call("clear_storage");
    check(call("get_storage").at("storage").empty(), "clear local storage");

    call("clear_cookies");
    call("set_cookie",
         {{"name", "relay"}, {"value", "base"}, {"domain", "127.0.0.1"}});
    check(call("get_cookies").at("cookies").size() == 1,
          "cookie aliases operate on real browser store");
    check(eval("document.cookie").get<std::string>().find("relay=base") !=
              std::string::npos,
          "cookie is observable by page");
    call("browser_cookies", {{"action", "set"},
                             {"name", "relay"},
                             {"value", "scoped"},
                             {"domain", "127.0.0.1"},
                             {"path", "/scope"}});
    check(call("browser_cookies", {{"name", "relay"}}).at("cookies").size() ==
              2,
          "same-name cookie paths coexist");
    call("browser_cookies", {{"action", "delete"},
                             {"name", "relay"},
                             {"domain", "127.0.0.1"},
                             {"path", "/scope"}});
    check(call("browser_cookies", {{"name", "relay"}}).at("cookies").size() ==
              1,
          "cookie deletion filters exact domain and path");
    call("browser_cookies", {{"action", "set"},
                             {"name", "secret"},
                             {"value", ""},
                             {"domain", "127.0.0.1"},
                             {"httpOnly", true},
                             {"secure", true}});
    const auto secret =
        call("browser_cookies", {{"name", "secret"}}).at("cookies").at(0);
    check(secret.at("value") == "" && secret.at("httpOnly") == true &&
              secret.at("secure") == true,
          "cookie empty value and flags preserved");
    check(eval("document.cookie.includes('secret=')") == false,
          "HttpOnly cookie hidden from page JavaScript");
    const double expiry = eval("Math.floor(Date.now()/1000)+3600");
    call("browser_cookies", {{"action", "set"},
                             {"name", "expiry"},
                             {"value", "time"},
                             {"domain", "127.0.0.1"},
                             {"expires", expiry}});
    check(call("browser_cookies", {{"name", "expiry"}})
                  .at("cookies")
                  .at(0)
                  .at("expires") == expiry,
          "cookie expiration uses Unix seconds");
    rejects([&] { call("browser_cookies", {{"action", "delete"}}); },
            "cookie deletion requires name");
    rejects(
        [&] {
          call("browser_cookies",
               {{"action", "set"}, {"name", "bad"}, {"domain", "127.0.0.1"}});
        },
        "cookie set requires value");
    const auto isolated = runtime.browser()
                              .browser_call("Target.createBrowserContext")
                              .at("browserContextId");
    check(runtime.browser()
              .browser_call("Storage.getCookies",
                            {{"browserContextId", isolated}})
              .at("cookies")
              .empty(),
          "writes do not enter another browser context");
    runtime.browser().browser_call("Target.disposeBrowserContext",
                                   {{"browserContextId", isolated}});
    call("browser_cookies", {{"action", "delete"}, {"name", "relay"}});
    check(call("browser_cookies", {{"name", "relay"}}).at("cookies").empty(),
          "delete name across matching paths");
    call("clear_cookies");
    check(call("get_cookies").at("cookies").empty(),
          "clear selected browser context cookies");

    eval(R"JS(
      document.querySelector('#person').style.setProperty('outline','1px dotted blue','important');
      window.ownedHighlightState=()=>{
        const e=document.querySelector('#person'), key=Symbol.for('PagePilot.outlineLease'), lease=e[key];
        return {outline:e.style.outline,priority:e.style.getPropertyPriority('outline'),
          width:e.style.outlineWidth,style:e.style.outlineStyle,color:e.style.outlineColor,
          computedColor:getComputedStyle(e).outlineColor,hasLease:Object.prototype.hasOwnProperty.call(e,key),timer:lease?.timer??null,
          now:performance.now(),visibility:document.visibilityState,
          timers:(window.ownedHighlightTimers??[]).map(r=>({id:r.id,delay:r.delay,cancelled:r.cancelled,fired:r.fired}))};
      };true
    )JS");
    auto highlight_check = [&](auto predicate, const char *label) {
      const auto state = eval("ownedHighlightState()");
      const bool matches = predicate(state);
      if (!matches)
        std::cerr << "Highlight state: " << state.dump() << '\n';
      check(matches, label);
    };
    const auto outline = eval("ownedHighlightState().outline");
    // Inspect the visible style before scheduling the short expiry. The actual
    // expiry is observed through its lease; host sleep is not renderer progress.
    call("element_highlight",
         {{"selector", "#person"}, {"duration", 5000}, {"color", "#0f0"}});
    highlight_check([](const JsonDoc &state) {
      return state.at("computedColor") == "rgb(0, 255, 0)";
    }, "highlight visible in computed style");
    call("element_highlight",
         {{"selector", "#person"}, {"duration", 150}, {"color", "#0f0"}});
    try {
      call("page_wait", {{"type", "function"},
                         {"expression", "!ownedHighlightState().hasLease"},
                         {"timeout", 5000}});
    } catch (...) {
      const auto failure = std::current_exception();
      try {
        std::cerr << "Highlight expiry wait: "
                  << eval("ownedHighlightState()").dump() << '\n';
      } catch (...) {}
      std::rethrow_exception(failure);
    }
    highlight_check([&](const JsonDoc &state) {
      return state.at("outline") == outline && state.at("hasLease") == false;
    }, "highlight restores original outline");
    highlight_check([](const JsonDoc &state) {
      return state.at("priority") == "important";
    }, "highlight restores original priority");

    // Record the real production callbacks with a controlled page scheduler.
    // Replaying an already-cancelled callback explicitly tests stale callback
    // ordering without relying on a narrow interval between two host sleeps.
    eval(R"JS(
      window.ownedHighlightTimers=[];
      window.ownedSetTimeout=window.setTimeout;window.ownedClearTimeout=window.clearTimeout;
      window.setTimeout=(callback,delay,...args)=>{
        const record={id:1000000000+ownedHighlightTimers.length,delay,cancelled:false,fired:false};
        record.fire=()=>{record.fired=true;callback(...args)};
        ownedHighlightTimers.push(record);return record.id;
      };
      window.clearTimeout=id=>{
        const record=ownedHighlightTimers.find(r=>r.id===id);
        if(record)record.cancelled=true;else ownedClearTimeout(id);
      };true
    )JS");
    call("element_highlight", {{"selector", "#person"}, {"duration", 120}});
    call("element_highlight",
         {{"selector", "#person"}, {"duration", 250}, {"color", "blue"}});
    eval("ownedHighlightTimers[0].fire();true");
    highlight_check([](const JsonDoc &state) {
      const auto &timers = state.at("timers");
      return state.at("width") == "3px" && state.at("color") == "blue" &&
             state.at("hasLease") == true && timers.size() == 2 &&
             timers.at(0).at("delay") == 120 &&
             timers.at(0).at("cancelled") == true &&
             timers.at(0).at("fired") == true &&
             timers.at(1).at("delay") == 250 &&
             state.at("timer") == timers.at(1).at("id");
    }, "earlier highlight timer cannot cancel later highlight");
    eval("ownedHighlightTimers[1].fire();true");
    highlight_check([&](const JsonDoc &state) {
      return state.at("outline") == outline &&
             state.at("priority") == "important" && state.at("hasLease") == false;
    }, "overlapping highlights restore pre-highlight style");
    call("element_highlight", {{"selector", "#person"}, {"duration", 120}});
    eval("document.querySelector('#person').style.outline='5px dashed "
         "orange';ownedHighlightTimers.at(-1).fire();true");
    highlight_check([](const JsonDoc &state) {
      return state.at("width") == "5px" && state.at("style") == "dashed" &&
             state.at("color") == "orange";
    }, "highlight cleanup respects later page edits");
    highlight_check([](const JsonDoc &state) {
      return state.at("hasLease") == false;
    }, "highlight lease removed after restoration");
    call("element_highlight", {{"selector", "#person"}, {"duration", 120}});
    eval("document.querySelector('#person').style.outlineColor='lime';"
         "ownedHighlightTimers.at(-1).fire();true");
    highlight_check([](const JsonDoc &state) {
      return state.at("width") == "5px" && state.at("style") == "dashed" &&
             state.at("color") == "lime" && state.at("hasLease") == false;
    }, "highlight restores untouched properties while preserving one page-edited property");
    eval("window.setTimeout=ownedSetTimeout;window.clearTimeout=ownedClearTimeout;"
         "delete window.ownedSetTimeout;delete window.ownedClearTimeout;true");
    rejects(
        [&] {
          call("element_highlight",
               {{"selector", "#person"}, {"color", "red;display:none"}});
        },
        "highlight rejects invalid CSS color");
    eval("document.querySelector('#person').value='Snapshot "
         "text';document.body.insertAdjacentHTML('beforeend','<input "
         "type=checkbox checked aria-label=\"Snapshot choice\"><button "
         "aria-hidden=true>Hidden snapshot name</button>');true");
    const auto snapshot = call("page_snapshot");
    check(snapshot.at("format") == "ax-yaml" && snapshot.at("tree").is_array(),
          "snapshot identifies native AX format and tree");
    check(snapshot.at("snapshot").get<std::string>().find("Snapshot choice") !=
              std::string::npos,
          "snapshot contains computed accessible name");
    check(snapshot.at("tree").dump().find("\"checked\":\"true\"") !=
              std::string::npos,
          "snapshot preserves checkbox state");
    check(snapshot.at("tree").dump().find("Snapshot text") != std::string::npos,
          "snapshot preserves input value");
    check(snapshot.at("snapshot")
                  .get<std::string>()
                  .find("Hidden snapshot name") == std::string::npos,
          "snapshot excludes aria-hidden subtree");

    call("page_navigate", {{"url", std::string(site) + "/frame-host.html"}});
    call("page_wait", {{"type", "function"},
                       {"expression", "readyFrames.length>=6"},
                       {"timeout", 5000}});
    call("page_storage",
         {{"action", "set"}, {"key", "scope"}, {"value", "root"}});
    call("frame_enter", {{"selector", "#crossFrame"}});
    check(call("page_storage", {{"key", "scope"}}).at("scope").is_null(),
          "OOP frame storage isolated by real origin");
    call("page_storage",
         {{"action", "set"}, {"key", "scope"}, {"value", "frame"}});
    check(eval("localStorage.getItem('scope')") == "frame",
          "storage targets selected OOP default context");
    call("page_dialog", {{"text", "Frame prompt"}});
    check(eval("prompt('frame prompt')") == "Frame prompt",
          "OOP dialog handled while renderer call waits");
    call(
        "element_highlight",
        {{"selector", "#frame-input"}, {"duration", 500}, {"color", "purple"}});
    check(eval("getComputedStyle(document.querySelector('#frame-input'))."
               "outlineColor") == "rgb(128, 0, 128)",
          "highlight targets selected OOP document");
    const auto frame_snapshot =
        call("page_snapshot").at("snapshot").get<std::string>();
    check(frame_snapshot.find("Frame action") != std::string::npos &&
              frame_snapshot.find("Root button") == std::string::npos,
          "snapshot follows selected frame without root content");
    call("frame_reset");
    check(call("page_storage", {{"key", "scope"}}).at("scope") == "root",
          "frame storage preserves root value");
    call("frame_enter", {{"selector", "#sameFrame"}});
    check(call("page_storage", {{"key", "scope"}}).at("scope") == "root",
          "same-origin frame shares local storage");
    call("page_dialog", {{"action", "accept"}});
    check(eval("confirm('same frame')") == true,
          "same-process frame dialog acceptance");
    call("tab_close");
    std::cout << passed << " page services checks passed; " << failed
              << " failed\n";
    return failed ? 1 : 0;
  } catch (const std::exception &error) {
    std::cerr << step << "\n" << error.what() << '\n';
    return 1;
  }
}
