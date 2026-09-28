#include <pagepilot/tool_runtime.hpp>
#include <cstdlib>
#include <iostream>
using namespace pagepilot;
unsigned passed = 0, failed = 0;
std::string step;
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
      throw BridgeError("Missing fixture URL");
    ToolRuntime runtime(static_cast<unsigned>(std::stoul(argv[1])));
    ToolCatalog catalog;
    auto call = [&](const std::string &name, JsonDoc arguments = JsonDoc::object()) {
      step = name + " " + arguments.dump();
      return runtime.invoke(catalog.resolve(name, arguments, true));
    };
    auto eval = [&](const std::string &expression) {
      return call("page_evaluate", {{"script", expression}}).at("result");
    };
    auto center = [&](const std::string &selector) {
      auto box = call("element_read",
                      {{"selector", selector}, {"type", "bounding_box"}});
      return JsonDoc{
          {"x", box.at("x").get<double>() + box.at("width").get<double>() / 2},
          {"y",
           box.at("y").get<double>() + box.at("height").get<double>() / 2}};
    };
    call("tab_create");
    call("page_navigate", {{"url", std::string(site) + "/page.html"}});
    eval(R"JS(
      window.pointerProof=[];
      for(const type of ['mousedown','mouseup','mousemove','click','auxclick','dragstart','dragenter','dragover','drop','dragend'])
        document.addEventListener(type,e=>pointerProof.push({type,id:e.target.id,button:e.button,buttons:e.buttons,x:e.clientX,y:e.clientY,trusted:e.isTrusted,data:e.dataTransfer?.getData('text/plain')}));
      document.addEventListener('contextmenu',e=>e.preventDefault());
      document.body.insertAdjacentHTML('afterbegin','<div id=slider style="width:400px;height:60px;background:silver;user-select:none"></div>');
      window.sliderMoves=[];document.querySelector('#slider').onmousemove=e=>{if(e.buttons&1)sliderMoves.push(e.clientX)};
      true
    )JS");
    auto at = center("#count-button");
    call("move_mouse", at);
    check(runtime.browser().pointer_position().at("x") == at.at("x"),
          "mouse position persists across runtime calls");
    call("mouse_down");
    call("mouse_up");
    check(eval("Number(document.querySelector('#clicks').textContent)") == 1,
          "separate move/down/up click intended button");
    check(eval("pointerProof.some(e=>e.type==='mousedown'&&e.id==='count-"
               "button'&&e.buttons===1&&e.trusted)") == true,
          "native down contains pressed buttons bitmask");
    check(eval("pointerProof.some(e=>e.type==='mouseup'&&e.id==='count-button'&"
               "&e.buttons===0&&e.trusted)") == true,
          "native up clears pressed buttons bitmask");
    at["action"] = "click";
    at["button"] = "middle";
    call("pointer_action", at);
    check(eval("pointerProof.some(e=>e.type==='auxclick'&&e.button===1&&e."
               "trusted)") == true,
          "middle pointer click is a trusted auxiliary click");
    at["button"] = "right";
    call("pointer_action", at);
    check(eval("pointerProof.some(e=>e.type==='mousedown'&&e.button===2&&e."
               "buttons===2&&e.trusted)") == true,
          "right button has correct event button and mask");
    auto start = center("#slider");
    start["x"] = start.at("x").get<double>() - 150;
    call("move_mouse", start);
    call("mouse_down");
    auto end = start;
    end["x"] = start.at("x").get<double>() + 280;
    end["steps"] = 10;
    call("move_mouse", end);
    call("mouse_up");
    check(eval("sliderMoves.length>=10") == true,
          "interpolated movement retains held button across calls");
    check(runtime.browser().pointer_position().at("buttons") == 0,
          "explicit release leaves no held buttons");
    auto result =
        call("pointer_drag",
             {{"from_selector", "#slider"}, {"offset_x", 75}, {"offset_y", 0}});
    check(result.at("to").at("x").get<double>() -
                  result.at("from").at("x").get<double>() ==
              75,
          "offset drag reports exact endpoint");
    check(eval("sliderMoves.length>=26") == true,
          "drag dispatches native moves with held button");
    const auto bx = start.at("x").get<double>(),
               by = start.at("y").get<double>();
    result =
        call("pointer_drag",
             {{"from_x", bx}, {"from_y", by}, {"to_x", bx + 40}, {"to_y", by}});
    check(result.at("from").at("x") == bx && result.at("to").at("x") == bx + 40,
          "coordinate drag endpoints");
    rejects(
        [&] {
          call("pointer_drag",
               {{"from_x", bx}, {"from_y", by}, {"to_x", 1e20}});
        },
        "invalid drag destination rejected before button down");
    check(runtime.browser().pointer_position().at("buttons") == 0,
          "invalid drag does not leave button held");
    call("pointer_drag",
         {{"from_selector", "#drag"}, {"to_selector", "#drop"}});
    check(eval("document.querySelector('#drop').textContent") == "owned-item",
          "HTML drag transfers browser drag payload to drop target");
    check(eval("pointerProof.some(e=>e.type==='drop'&&e.id==='drop'&&e.trusted&"
               "&e.data==='owned-item')") == true,
          "HTML drop is trusted and retains native dataTransfer");
    check(runtime.browser().pointer_position().at("buttons") == 0,
          "HTML drag releases pointer state");
    eval("document.querySelector('#drop').textContent='reset';true");
    start = center("#drag");
    end = center("#drop");
    call("move_mouse", start);
    call("mouse_down");
    end["steps"] = 12;
    call("move_mouse", end);
    call("mouse_up");
    check(eval("document.querySelector('#drop').textContent") == "owned-item",
          "HTML drag works across separate pointer actions");
    call("element_click", {{"selector", "#count-button"}});
    check(eval("Number(document.querySelector('#clicks').textContent)") == 2,
          "ordinary clicks still work after native drag");
    call("move_mouse", start);
    call("mouse_down");
    call("mouse_down", {{"button", "right"}});
    check(runtime.browser().pointer_position().at("buttons") == 3,
          "multiple held buttons have independent state");
    call("mouse_up", {{"button", "right"}});
    call("mouse_up");
    check(runtime.browser().pointer_position().at("buttons") == 0,
          "releasing each button clears its bit");

    const auto first = runtime.browser().current_target();
    start = {{"x", 41}, {"y", 49}};
    call("move_mouse", start);
    call("tab_create", {{"url", std::string(site) + "/page.html"}});
    check(runtime.browser().pointer_position().at("x") == 0,
          "new tab gets independent pointer position");
    call("move_mouse", {{"x", 80}, {"y", 90}});
    call("tab_close");
    const auto tabs = call("tab_list").at("tabs");
    for (const auto &tab : tabs)
      if (tab.at("id") == first)
        call("tab_activate", {{"index", tab.at("index")}});
    check(runtime.browser().pointer_position().at("x") == 41 &&
              runtime.browser().pointer_position().at("y") == 49,
          "tab switch restores that page's pointer state");
    call("mouse_down");
    runtime.browser().cancel_pointer();
    check(runtime.browser().pointer_position().at("buttons") == 0,
          "cleanup releases a held pointer button");
    check(
        eval("pointerProof.filter(e=>e.type==='mouseup').at(-1).buttons===0") ==
            true,
        "cleanup reaches actual native mouseup listener");
    call("element_hover", {{"selector", "#count-button"}});
    rejects(
        [&] {
          call("element_click", {{"selector", "#count-button"},
                                 {"type", "long"},
                                 {"duration", 500},
                                 {"timeout", 120}});
        },
        "long click exceeding deadline reports failure");
    check(runtime.browser().pointer_position().at("buttons") == 0,
          "deadline failure releases held pointer state");
    check(
        eval("pointerProof.filter(e=>e.type==='mouseup').at(-1).buttons===0") ==
            true,
        "deadline cleanup sends actual browser button release");
    call("page_navigate", {{"url", std::string(site) + "/frame-host.html"}});
    call("page_wait", {{"type", "function"},
                       {"expression", "readyFrames.length>=6"},
                       {"timeout", 5000}});
    call("frame_enter", {{"selector", "#crossFrame"}});
    eval(R"JS(
      document.body.innerHTML='<div id=source draggable=true style="width:100px;height:50px;background:silver">Source</div><div id=target style="margin-left:150px;width:150px;height:80px;background:tan">Target</div>';
      window.dropTrusted=false;
      document.querySelector('#source').ondragstart=e=>e.dataTransfer.setData('text/plain','OOP value');
      document.querySelector('#target').ondragover=e=>e.preventDefault();
      document.querySelector('#target').ondrop=e=>{e.preventDefault();window.dropTrusted=e.isTrusted;e.target.textContent=e.dataTransfer.getData('text/plain')};true
    )JS");
    call("pointer_drag",
         {{"from_selector", "#source"}, {"to_selector", "#target"}});
    check(eval("document.querySelector('#target').textContent") == "OOP value",
          "native HTML drag works inside transformed OOP iframe");
    check(eval("dropTrusted") == true, "OOP drop remains trusted");
    call("frame_reset");
    check(eval("document.querySelector('#root-clicks').textContent") == "0",
          "OOP drag does not click parent content");
    call("tab_close");
    std::cout << passed << " pointer checks passed; " << failed << " failed\n";
    return failed ? 1 : 0;
  } catch (const std::exception &error) {
    std::cerr << step << "\n" << error.what() << '\n';
    return 1;
  }
}
