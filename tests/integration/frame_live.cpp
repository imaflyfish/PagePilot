#include <cstdlib>
#include <exception>
#include <iostream>
#include <pagepilot/tool_runtime.hpp>
using namespace pagepilot;
unsigned passed = 0, failed = 0;
std::string active_step;
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
      active_step = name + " " + args.dump();
      return runtime.invoke(catalog.resolve(name, args, true));
    };
    auto evaluate = [&](const std::string &expression) {
      return call("page_evaluate", {{"script", expression}}).at("result");
    };
    auto root_eval = [&](const std::string &expression) {
      return runtime.browser()
          .page_call("Runtime.evaluate",
                     {{"expression", expression}, {"returnByValue", true}})
          .at("result")
          .value("value", JsonDoc(nullptr));
    };
    auto clicks = [&]() {
      return evaluate(
          "Number(document.querySelector('#frame-clicks').textContent)");
    };
    call("tab_create");
    call("page_navigate", {{"url", std::string(site) + "/frame-host.html"}});
    call("page_wait", {{"type", "function"},
                       {"expression", "readyFrames.length>=6"},
                       {"timeout", 5000}});
    auto inventory = call("frame_list");
    check(inventory.at("frames").size() == 7,
          "inventory includes all same-process and OOP descendants");
    check(inventory.at("current_depth") == 0, "root depth starts at zero");
    auto entered = call("frame_enter", {{"selector", "#sameFrame"}});
    check(entered.at("depth") == 1 && entered.at("separateSession") == false,
          "same-origin frame uses parent session");
    check(evaluate("frameMarker") == "127.0.0.1|0",
          "frame eval uses the real default world");
    call("element_fill",
         {{"selector", "label=Frame field"}, {"text", "Same 中文😀"}});
    check(evaluate("document.querySelector('#frame-input').value") ==
              "Same 中文😀",
          "same-origin frame fill");
    check(root_eval("document.querySelector('#frame-input').value") == "ROOT",
          "frame fill preserves same selector in parent");
    call("element_click", {{"selector", "role=button[name=\"Frame action\"]"}});
    check(clicks() == 1, "same-origin frame native click uses parent offset");
    check(evaluate("frameProof.some(e=>e.type==='click'&&e.trusted)") == true,
          "same-frame click is trusted");
    call("frame_enter", {{"selector", "#child-frame"}});
    call("frame_enter", {{"selector", "#child-frame"}});
    check(evaluate("frameMarker") == "127.0.0.1|2",
          "nested same-origin frame execution context");
    call("element_click", {{"selector", "#frame-button"}});
    check(clicks() == 1,
          "nested same-process offsets are applied exactly once");
    check(call("frame_leave").at("depth") == 2,
          "frame leave returns to immediate parent");
    check(evaluate("frameMarker") == "127.0.0.1|1", "parent context restored");
    check(call("frame_reset").at("exited") == 2,
          "reset reports exact exited depth");
    check(evaluate("frameMarker") == "ROOT",
          "reset returns to page default world");
    entered = call("frame_enter", {{"selector", "#crossFrame"}});
    check(entered.at("separateSession") == true,
          "cross-site frame has an actual independent CDP session");
    check(evaluate("frameMarker") == "localhost|0",
          "OOP frame default-world execution");
    check(evaluate("(()=>{try{void top.document.body;return false}catch{return "
                   "true}})()") == true,
          "fixture is genuinely cross-origin");
    call("element_fill", {{"selector", "#frame-input"}, {"text", "Cross 😀"}});
    check(evaluate("document.querySelector('#frame-input').value") ==
              "Cross 😀",
          "native input routes to OOP focused frame");
    check(evaluate("frameProof.some(e=>e.type==='input'&&e.trusted)") == true,
          "OOP input remains trusted");
    call("keyboard_chord", {{"keys", "ControlOrMeta+a"}});
    call("keyboard_press", {{"key", "Z"}});
    check(evaluate("document.querySelector('#frame-input').value") == "Z",
          "keyboard chord routes to focused OOP frame");
    // Keep the original hover deadline and receipt requirement. If routing
    // fails on a hosted browser, preserve its coordinates and actual events.
    const std::string hover_trace = R"JS(
      window.ownedHoverTrace=[];
      window.ownedHoverListener=e=>{
        if(ownedHoverTrace.length>=32)return;
        const record={id:e.target.id,tag:e.target.tagName,x:e.clientX,y:e.clientY,
          trusted:e.isTrusted,now:performance.now(),settled:null};
        ownedHoverTrace.push(record);
        setTimeout(()=>record.settled=performance.now(),0);
      };
      document.addEventListener('mousemove',ownedHoverListener,true);true
    )JS";
    evaluate(hover_trace);
    root_eval(hover_trace);
    auto hover_snapshot = [&]() {
      const auto pointer = runtime.browser().pointer_position();
      JsonDoc result = {{"pointer", pointer}, {"frames", JsonDoc::array()}};
      for (const auto &frame : runtime.browser().frames())
        result["frames"].push_back({{"frame", frame.frame},
                                    {"session", frame.session},
                                    {"unique_context", frame.unique_context}});
      result["selected"] = runtime.browser().evaluate(R"JS((()=>{
        const button=document.querySelector('#frame-button');
        return {marker:frameMarker,now:performance.now(),visibility:document.visibilityState,
          rect:button.getBoundingClientRect().toJSON(),hovered:button.matches(':hover'),
          hoverChain:Array.from(document.querySelectorAll(':hover'),e=>({id:e.id,tag:e.tagName})),
          lastPointer:globalThis.lastPointer??null,proof:frameProof,
          trace:ownedHoverTrace,innerWidth,innerHeight,
          clientWidth:document.documentElement.clientWidth,scrollX,scrollY};
      })())JS",
                                                      MsDuration(1000));
      const auto root = "(()=>{const point=" + pointer.dump() + R"JS(;
        const owner=document.querySelector('#crossFrame'),hit=document.elementFromPoint(point.x,point.y);
        return {now:performance.now(),visibility:document.visibilityState,
          rect:owner.getBoundingClientRect().toJSON(),hit:hit?{id:hit.id,tag:hit.tagName}:null,
          hoverChain:Array.from(document.querySelectorAll(':hover'),e=>({id:e.id,tag:e.tagName})),
          trace:ownedHoverTrace,innerWidth,innerHeight,
          clientWidth:document.documentElement.clientWidth,scrollX,scrollY};
      })())JS";
      result["root"] =
          runtime.browser()
              .page_call("Runtime.evaluate",
                         {{"expression", root}, {"returnByValue", true}},
                         MsDuration(1000))
              .at("result")
              .value("value", JsonDoc(nullptr));
      result["layout"] = runtime.browser().page_call(
          "Page.getLayoutMetrics", JsonDoc::object(), MsDuration(1000));
      return result;
    };
    const auto hover_before = hover_snapshot();
    try {
      call("element_hover", {{"selector", "#frame-button"}});
    } catch (...) {
      const auto failure = std::current_exception();
      JsonDoc diagnostic = {{"before", hover_before}};
      try {
        diagnostic["after"] = hover_snapshot();
      } catch (const std::exception &error) {
        diagnostic["snapshot_error"] = error.what();
      }
      std::cerr << "Hover routing state: " << diagnostic.dump() << '\n';
      std::rethrow_exception(failure);
    }
    const std::string clear_hover_trace =
        "document.removeEventListener('mousemove',ownedHoverListener,true);"
        "delete window.ownedHoverListener;delete window.ownedHoverTrace;true";
    evaluate(clear_hover_trace);
    root_eval(clear_hover_trace);
    check(
        evaluate("document.querySelector('#frame-button').matches(':hover')") ==
            true,
        "OOP hover waits for routed mouse event");
    call("element_click", {{"selector", "#frame-button"}});
    check(clicks() == 1, "rotated and scaled OOP owner maps click correctly");
    check(evaluate("Object.keys(globalThis).filter(key=>key.startsWith('__"
                   "pagepilot_receipt_')).length") == 0,
          "input observation leaves no page-global binding behind");
    const auto bounds = call("element_read", {{"selector", "#frame-button"},
                                              {"type", "bounding_box"}});
    const auto global = runtime.browser().project_point(
        evaluate(
            "(()=>{const "
            "r=document.querySelector('#frame-button').getBoundingClientRect();"
            "return {x:r.x+r.width/2,y:r.y+r.height/2}})()"),
        MsDuration(1000));
    check(bounds.at("x").get<double>() < global.at("x").get<double>() &&
              bounds.at("x").get<double>() + bounds.at("width").get<double>() >
                  global.at("x").get<double>(),
          "frame bounds use root-page coordinates");
    check(call("element_check",
               {{"selector", "#frame-button"}, {"state", "in_viewport"}})
                  .at("in_viewport") == true,
          "frame viewport check includes visible parent viewport");
    root_eval("scrollTo(0,0);true");
    check(call("element_check",
               {{"selector", "#frame-button"}, {"state", "in_viewport"}})
                  .at("in_viewport") == false,
          "frame viewport check detects an offscreen parent frame");
    root_eval(
        "document.body.insertAdjacentHTML('beforeend','<div id=frame-cover "
        "style=\"position:fixed;inset:0;z-index:99999\"></div>');true");
    rejects(
        [&] {
          call("element_click",
               {{"selector", "#frame-button"}, {"timeout", 100}});
        },
        "parent overlay blocks frame input");
    check(clicks() == 1, "blocked frame click does not change child state");
    root_eval("document.querySelector('#frame-cover').remove();true");
    call("frame_enter", {{"selector", "#child-frame"}});
    call("frame_enter", {{"selector", "#child-frame"}});
    check(evaluate("frameMarker") == "localhost|2",
          "nested cross-process frame context");
    call("element_fill",
         {{"selector", "#frame-input"}, {"text", "Nested cross"}});
    call("element_click", {{"selector", "#frame-button"}});
    check(clicks() == 1,
          "three-level cross-process click maps all frame boundaries");
    check(call("frame_list").at("frames").size() == 7,
          "inventory from inside a frame still covers the page");
    check(call("frame_list").at("current_depth") == 3,
          "inventory retains selected frame depth");
    check(call("exit_all_frames").at("exited") == 3,
          "legacy frame-reset alias");
    check(root_eval("document.querySelector('#root-clicks').textContent") ==
              "0",
          "all frame clicks preserve root button state");
    rejects([&] { call("frame_enter", {{"selector", "#frame-input"}}); },
            "non-frame element refused");
    check(call("browser_status").at("frameDepth") == 0,
          "failed entry does not alter frame stack");
    call("frame_enter", {{"selector", "#crossFrame"}});
    const auto same =
        std::string(site) + "/frame-branch.html?remaining=0&level=9&mode=same";
    root_eval("document.querySelector('#crossFrame').src=" +
              JsonDoc(same).dump() + ";true");
    call("page_wait", {{"type", "function"},
                       {"expression", "frameMarker==='127.0.0.1|9'"},
                       {"timeout", 5000}});
    check(evaluate("frameMarker") == "127.0.0.1|9",
          "selected OOP frame survives navigation into parent process");
    call("element_click", {{"selector", "#frame-button"}});
    check(clicks() == 1, "input after OOP-to-same-process navigation");
    auto cross = same;
    cross.replace(cross.find("127.0.0.1"), 9, "localhost");
    root_eval("document.querySelector('#crossFrame').src=" +
              JsonDoc(cross).dump() + ";true");
    call("page_wait", {{"type", "function"},
                       {"expression", "frameMarker==='localhost|9'"},
                       {"timeout", 5000}});
    check(evaluate("frameMarker") == "localhost|9",
          "selected frame survives navigation into a new OOP session");
    call("element_click", {{"selector", "#frame-button"}});
    check(clicks() == 1, "input after same-process-to-OOP navigation");
    for (int iteration = 0; iteration < 4; ++iteration) {
      const bool remote = iteration % 2 != 0;
      auto destination = std::string(site) +
                         "/frame-branch.html?remaining=0&mode=same&level=" +
                         std::to_string(20 + iteration);
      if (remote)
        destination.replace(destination.find("127.0.0.1"), 9, "localhost");
      root_eval("document.querySelector('#crossFrame').src=" +
                JsonDoc(destination).dump() + ";true");
      const auto marker = std::string(remote ? "localhost|" : "127.0.0.1|") +
                          std::to_string(20 + iteration);
      call("page_wait",
           {{"type", "function"},
            {"expression", "typeof frameMarker!=='undefined'&&frameMarker===" +
                               JsonDoc(marker).dump()},
            {"timeout", 5000}});
      check(evaluate("frameMarker") == marker,
            "repeated process swap resolves replacement default context");
      check(evaluate("window.executionProof=(window.executionProof??0)+1") == 1,
            "script executes once after process replacement");
    }
    rejects(
        [&] {
          evaluate("window.failureProof=(window.failureProof??0)+1;throw "
                   "Error('intentional script failure')");
        },
        "ordinary script exceptions remain failures");
    check(evaluate("failureProof") == 1,
          "script exceptions are not retried after a side effect");
    const auto link =
        std::string(site) + "/frame-branch.html?remaining=0&level=10&mode=same";
    evaluate("(()=>{const "
             "link=document.createElement('a');link.id='frame-link';link."
             "textContent='Navigate frame';link.href=" +
             JsonDoc(link).dump() + ";document.body.append(link)})()");
    call("element_click", {{"selector", "#frame-link"},
                           {"wait_after", "load"},
                           {"timeout", 5000}});
    check(evaluate("frameMarker") == "127.0.0.1|10",
          "click can navigate a frame across processes");
    check(evaluate("Object.keys(globalThis).filter(key=>key.startsWith('__"
                   "pagepilot_receipt_')).length") == 0,
          "input observation cleans up across process navigation");
    call("frame_reset");
    call("frame_enter", {{"selector", "#sameFrame"}});
    root_eval("document.querySelector('#sameFrame').remove();true");
    rejects(
        [&] {
          call("element_fill", {{"selector", "#frame-input"},
                                {"text", "WRONG"},
                                {"timeout", 80}});
        },
        "detached selected frame fails instead of typing in parent");
    check(root_eval("document.querySelector('#frame-input').value") == "ROOT",
          "detached-frame failure preserves parent input");
    check(call("frame_leave").at("exited") == true,
          "can explicitly leave detached frame");
    check(evaluate("frameMarker") == "ROOT",
          "root restored after explicit detached-frame exit");
    call("frame_enter", {{"selector", "#crossFrame"}});
    call("page_navigate", {{"url", std::string(site) + "/page.html"}});
    check(call("browser_status").at("frameDepth") == 0,
          "page navigation clears selected frame scopes");
    check(evaluate("document.title") == "PagePilot fixture",
          "navigation resumes page context");
    call("tab_close");
    std::cout << passed << " live-frame checks passed; " << failed
              << " failed\n";
    return failed ? 1 : 0;
  } catch (const std::exception &error) {
    std::cerr << "UNCAUGHT during " << active_step << ": " << error.what()
              << '\n';
    return 1;
  }
}
