#include "../support/aborting_checks.hpp"
#include <cstdlib>
#include <iostream>
#include <pagepilot/tool_runtime.hpp>
#include <thread>
using namespace pagepilot;
using namespace pagepilot_aborting_tests;
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
    auto call = [&](const std::string &name, JsonDoc args = JsonDoc::object()) {
      return runtime.invoke(catalog.resolve(name, args, true));
    };
    auto eval = [&](const std::string &expression) {
      return call("page_evaluate", {{"script", expression}}).at("result");
    };
    auto fresh = [&] {
      call("page_navigate", {{"url", site + "/click-targets.html"}});
      call("move_mouse", {{"x", 0}, {"y", 0}});
      eval("resetOwned()");
    };
    const auto created = call("tab_create");
    fresh();
    const auto vectors =
        eval("fetch(" + JsonDoc(site + "/click-values.json").dump() +
             ").then(r=>r.json())")
            .at("vectors");
    check(vectors.size() == 28, "28 actual baseline click vectors");
    for (const auto &vector : vectors) {
      call("move_mouse", {{"x", 0}, {"y", 0}});
      eval("resetOwned()");
      auto args = vector.at("arguments");
      args["timeout"] = 3000;
      auto result = call("click", args);
      result.erase("url");
      check(result == vector.at("result"),
            "baseline click result: " + args.dump() +
                " actual=" + result.dump());
      const auto observed = eval("({events,hovered})");
      check(observed.at("events") == vector.at("observed").at("events"),
            "baseline trusted click events: " + args.dump() +
                " actual=" + observed.dump());
      if (args.contains("hover_first"))
        check(observed.at("hovered") == vector.at("observed").at("hovered"),
              "explicit hover and conditional-skip effects match the baseline");
    }
    const auto css =
        eval("fetch(" + JsonDoc(site + "/css-list-values.json").dump() +
             ").then(r=>r.json())");
    eval(css.at("setup").get<std::string>());
    for (const auto &vector : css.at("vectors")) {
      const auto result =
          call("element_find",
               {{"selector", vector.at("selector")}, {"limit", 100}});
      check(result == vector.at("result"),
            "CSS selector grammar and list order: " +
                vector.at("selector").get<std::string>() +
                " actual=" + result.dump());
    }
    for (const auto &selector :
         {"#first,,#opener", ":is(#first,#third", "[data-token='unterminated"})
      rejects([&] { call("element_find", {{"selector", selector}}); },
              "invalid CSS selector must fail");
    for (int i = 0; i < 3; ++i)
      check(
          call("element_type",
               {{"index", i}, {"text", "position-" + std::to_string(i)}})
                  .at("currentValue") == "position-" + std::to_string(i),
          "global visible-input index uses ordered light and shadow controls");
    check(eval("document.querySelector('#middle').value==='position-0'&&"
               "document.querySelector('#shadow-list').shadowRoot."
               "querySelector('#shadow-input').value==='position-1'&&document."
               "querySelector('#tail-input').value==='position-2'") == true,
          "shadow input precedes the following document input in the global "
          "index");
    for (const auto &mode : {"fast", "smart", "human"}) {
      fresh();
      eval("const "
           "b=document.querySelector('#first');b.addEventListener('mouseenter',"
           "()=>b.style.transform='translateY(250px)',{once:true});true");
      call("element_click", {{"selector", "#first"}, {"mode", mode}});
      check(eval("events.filter(e=>e.kind==='mousedown').length===1&&events."
                 "filter(e=>e.kind==='click').length===1&&events.filter(e=>e."
                 "kind==='click')[0].id==='first'") == true,
            "target moving on hover is rechecked before native press");

      fresh();
      eval("window.oldPresses=0;window.newPresses=0;const "
           "b=document.querySelector('#first');b.onmousedown=()=>oldPresses++;"
           "b.addEventListener('mouseenter',()=>{const "
           "n=b.cloneNode(true);n.textContent='Replacement "
           "item';n.onmousedown=()=>newPresses++;b.replaceWith(n)},{once:true})"
           ";true");
      call("element_click", {{"selector", "#first"}, {"mode", mode}});
      check(
          eval("oldPresses===0&&newPresses===1&&events.filter(e=>e.kind==='"
               "click').length===1") == true,
          "a detached pre-press target is reacquired by its original selector");
    }
    fresh();
    eval(
        "window.replaced=false;const "
        "b=document.querySelector('#first');b.addEventListener('mouseenter',()="
        ">{replaced=true;b.replaceWith(b.cloneNode(true))},{once:true});true");
    call("element_click", {{"selector", "role=button[name=\"Pick item\"]"}});
    check(eval("replaced&&events.filter(e=>e.kind==='click').length===1&&"
               "events.filter(e=>e.kind==='click')[0].id==='first'") == true,
          "native accessibility lookup reacquires the replacement element");
    fresh();
    eval("window.motionSettled=false;window.pressedAfterMotion=false;const "
         "b=document.querySelector('#first');b.onmousedown=()=>"
         "pressedAfterMotion=motionSettled;const "
         "a=b.animate([{transform:'translateY(0)'},{transform:'translateY("
         "230px)'}],{duration:350,fill:'forwards'});a.finished.then(()=>"
         "motionSettled=true);true");
    call("element_click", {{"selector", "#first"}});
    check(eval("pressedAfterMotion&&events.filter(e=>e.kind==='mousedown')."
               "length===1") == true,
          "animation settles before an element click is pressed");

    fresh();
    eval("document.querySelector('#first').onmouseenter=()=>{const "
         "cover=document.createElement('div');cover.id='cover';cover.style='"
         "position:fixed;inset:0;background:white;z-index:100';document.body."
         "append(cover)};true");
    rejects(
        [&] {
          call("element_click", {{"selector", "#first"}, {"timeout", 250}});
        },
        "new overlay must block click");
    check(eval("events.length===0") == true,
          "hover-created overlay receives no accidental press");
    check(runtime.browser().pointer_position().at("buttons") == 0,
          "pre-press timeout leaves no held button");

    fresh();
    eval("document.querySelector('#first').onmouseenter=e=>e.target.disabled="
         "true;true");
    rejects(
        [&] {
          call("element_click", {{"selector", "#first"}, {"timeout", 250}});
        },
        "hover-disabled target must block click");
    check(eval("events.length===0") == true,
          "disabled-after-hover target receives no native press");

    // These scenarios require the first press to happen. Use the ordinary
    // click allowance for preparation, then verify the exact failure stage;
    // an arbitrary short total timeout can otherwise fail before any input.
    auto failed_click = [&](JsonDoc args) {
      JsonDoc observed = {{"arguments", args}, {"error", nullptr}};
      const auto started = std::chrono::steady_clock::now();
      try {
        call("element_click", args);
      } catch (const WireFailure &error) {
        observed["error"] = {{"type", "WireFailure"},
                             {"code", error.code},
                             {"message", error.what()}};
      } catch (const DeadlineReached &error) {
        observed["error"] = {{"type", "DeadlineReached"},
                             {"message", error.what()}};
      } catch (const BridgeError &error) {
        observed["error"] = {{"type", "BridgeError"},
                             {"message", error.what()}};
      }
      observed["elapsed_ms"] = std::chrono::duration_cast<MsDuration>(
                                   std::chrono::steady_clock::now() - started)
                                   .count();
      observed["state"] = eval(R"JS(({
        downCount:globalThis.downCount??null,events,
        presses:events.filter(e=>e.kind==='mousedown').length,
        releases:events.filter(e=>e.kind==='mouseup').length,
        visibility:document.visibilityState,now:performance.now(),
        receipts:Object.keys(globalThis).filter(k=>k.startsWith('__pagepilot_receipt_'))
      }))JS");
      observed["pointer"] = runtime.browser().pointer_position();
      return observed;
    };
    fresh();
    eval("window.downCount=0;const "
         "b=document.querySelector('#first');b.onmousedown=()=>{downCount++;b."
         "replaceWith(b.cloneNode(true))};true");
    const auto replacement = failed_click({{"selector", "#first"}});
    const auto replacement_detail = " actual=" + replacement.dump();
    check(replacement.at("error") ==
              JsonDoc({{"type", "BridgeError"},
                       {"message", "Target did not acknowledge the input event "
                                   "before the action deadline"}}),
          "post-press replacement cannot acknowledge original click" +
              replacement_detail);
    check(replacement.at("state").at("downCount") == 1 &&
              replacement.at("state").at("presses") == 1,
          "failure after the first press never reacquires or replays input" +
              replacement_detail);
    check(replacement.at("pointer").at("buttons") == 0,
          "post-press failure releases its button" + replacement_detail);
    check(replacement.at("state").at("receipts").empty(),
          "failed click removes input receipt bindings" + replacement_detail);
    for (const auto &kind : {"double", "triple"}) {
      fresh();
      eval("const "
           "b=document.querySelector('#first');b.onclick=()=>b.replaceWith(b."
           "cloneNode(true));true");
      rejects(
          [&] {
            call("element_click", {{"selector", "#first"}, {"type", kind}});
          },
          "multi-click stops when the target is replaced after the first "
          "click");
      check(eval("events.filter(e=>e.kind==='mousedown').length===1&&events."
                 "filter(e=>e.kind==='click').length===1") == true,
            "later click ordinals cannot press a replacement target");
    }
    fresh();
    // The requested hold exceeds the ordinary action allowance. After the
    // first press, StepClock must reject the delay and release that press.
    const auto long_press = failed_click(
        {{"selector", "#first"}, {"type", "long"}, {"duration", 10000}});
    const auto long_detail = " actual=" + long_press.dump();
    check(long_press.at("error") ==
              JsonDoc(
                  {{"type", "DeadlineReached"},
                   {"message",
                    "Requested delay exceeds the remaining action deadline"}}),
          "long press cannot exceed its action allowance" + long_detail);
    check(long_press.at("state").at("presses") == 1 &&
              long_press.at("state").at("releases") == 1,
          "long-press deadline releases the single attempted press" +
              long_detail);
    check(long_press.at("pointer").at("buttons") == 0,
          "long-press failure leaves no held button" + long_detail);

    fresh();
    eval("const "
         "h=document.createElement('div');h.id='shadow-host';h.style='position:"
         "absolute;left:40px;top:280px;width:160px;height:64px';h.attachShadow("
         "{mode:'open'}).innerHTML='<button id=shadow-click "
         "style=\"width:160px;height:64px\">Shadow</"
         "button>';document.body.append(h);const "
         "c=document.createElement('div');c.id='cover';c.style='position:"
         "absolute;left:20px;top:250px;width:220px;height:140px;background:"
         "white;z-index:10';document.body.append(c);window.shadowClicks=0;h."
         "shadowRoot.querySelector('button').onclick=()=>shadowClicks++;true");
    rejects(
        [&] {
          call("element_click",
               {{"selector", "#shadow-click"}, {"timeout", 250}});
        },
        "outer overlay blocks a shadow-root target");
    check(eval("shadowClicks===0&&events.length===0") == true,
          "shadow hit testing respects outer document occlusion");
    eval("document.querySelector('#cover').remove();true");
    call("element_click", {{"selector", "#shadow-click"}});
    check(eval("shadowClicks") == 1,
          "uncovered shadow-root target accepts one trusted click");

    fresh();
    auto cross = site;
    cross.replace(cross.find("127.0.0.1"), 9, "localhost");
    eval(
        "window.frameShifted=false;addEventListener('message',e=>{if(e.data==='"
        "shift-frame'){frameShifted=true;document.querySelector('#moving-frame'"
        ").style.transform='translateX(60px)'}});new Promise(resolve=>{const "
        "f=document.createElement('iframe');f.id='moving-frame';f.style='"
        "position:absolute;left:50px;top:260px;width:500px;height:230px';f."
        "onload=()=>resolve(true);f.src=" +
        JsonDoc(cross + "/click-targets.html").dump() +
        ";document.body.append(f)})");
    call("frame_enter", {{"selector", "#moving-frame"}});
    eval("document.title='Owned child click "
         "targets';document.querySelector('#first').addEventListener('"
         "mouseenter',()=>parent.postMessage('shift-frame','*'),{once:true});"
         "true");
    const auto frame_click =
        call("element_click", {{"selector", "#first"}, {"mode", "human"}});
    check(
        eval("events.filter(e=>e.kind==='click').length===1&&events.filter(e=>"
             "e.kind==='click')[0].id==='first'") == true,
        "moving OOP frame coordinates are revalidated after pointer movement");
    check(frame_click.at("inFrame") == true &&
              frame_click.at("title") == "Owned click targets" &&
              frame_click.at("url") == site + "/click-targets.html",
          "frame click result retains root-page metadata and selected-frame "
          "flag");
    call("frame_reset");
    check(eval("frameShifted") == true,
          "frame owner actually moved during the click");

    const auto original_target = created.at("target").get<std::string>();
    fresh();
    CdpChannel observer(static_cast<unsigned>(std::stoul(argv[1])));
    const auto watched =
        observer
            .call("Target.attachToTarget",
                  {{"targetId", original_target}, {"flatten", true}})
            .at("sessionId")
            .get<std::string>();
    const auto inventory = observer.call("Target.getTargets");
    std::string survivor;
    for (const auto &target : inventory.at("targetInfos"))
      if (target.at("type") == "page" &&
          target.at("targetId") != original_target)
        survivor = target.at("targetId").get<std::string>();
    const auto survivor_session =
        observer
            .call("Target.attachToTarget",
                  {{"targetId", survivor}, {"flatten", true}})
            .at("sessionId")
            .get<std::string>();
    observer.call("Runtime.evaluate",
                  {{"expression", "window.survivorPresses=0;addEventListener('"
                                  "mousedown',()=>survivorPresses++)"}},
                  survivor_session);
    eval("document.querySelector('#first').onmouseenter=e=>{e.target.hidden="
         "true;window.closeClickReady=true};true");
    bool closed = false;
    std::exception_ptr observer_error;
    std::jthread closing([&] {
      try {
        const auto until = std::chrono::steady_clock::now() + MsDuration(2000);
        while (!observer
                    .call("Runtime.evaluate",
                          {{"expression", "!!window.closeClickReady"},
                           {"returnByValue", true}},
                          watched)
                    .at("result")
                    .value("value", false)) {
          if (std::chrono::steady_clock::now() >= until)
            throw BridgeError("close observer never saw target hover");
          std::this_thread::sleep_for(MsDuration(5));
        }
        closed =
            observer.call("Target.closeTarget", {{"targetId", original_target}})
                .at("success");
      } catch (...) {
        observer_error = std::current_exception();
      }
    });
    bool click_failed = false;
    try {
      call("element_click", {{"selector", "#first"}, {"timeout", 3000}});
    } catch (const BridgeError &) {
      click_failed = true;
    }
    closing.join();
    if (observer_error)
      std::rethrow_exception(observer_error);
    check(closed && click_failed,
          "closing the hovered target makes the pending click fail");
    check(observer.call("Runtime.evaluate",
                        {{"expression", "survivorPresses"},
                         {"returnByValue", true}},
                        survivor_session)
                  .at("result")
                  .at("value") == 0,
          "pending click never presses the survivor after original target "
          "closure");
    rejects(
        [&] {
          runtime.browser().dispatch_pointer("mousePressed", 100, 100, "left",
                                             1, MsDuration(1000),
                                             original_target);
        },
        "pinned click cannot switch to a survivor after target closure");
    check(eval("survivorPresses") == 0,
          "closed-target pointer command has no effect on the survivor");
    std::cout << checks << " click checks passed; 0 failed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
