#include <cstdlib>
#include <iostream>
#include <pagepilot/tool_runtime.hpp>
#include <thread>
using namespace pagepilot;
namespace {
unsigned checks = 0;
void check(bool value, const std::string &label) {
  if (!value)
    throw BridgeError(label);
  ++checks;
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
    auto cross = site;
    cross.replace(cross.find("127.0.0.1"), 9, "localhost");
    ToolRuntime runtime(static_cast<unsigned>(std::stoul(argv[1])));
    ToolCatalog catalog;
    auto call = [&](const std::string &name, JsonDoc args = JsonDoc::object()) {
      return runtime.invoke(catalog.resolve(name, args, true));
    };
    auto eval = [&](const std::string &source) {
      return call("page_evaluate", {{"script", source}}).at("result");
    };
    const auto owned_target =
        call("tab_create").at("target").get<std::string>();
    for (const auto &scenario : {"root", "same-to-cross", "cross-to-same"}) {
      call("page_navigate", {{"url", site + "/frame-host.html"}});
      const bool remote = std::string(scenario) == "same-to-cross";
      if (std::string(scenario) != "root")
        call("frame_enter",
             {{"selector", remote ? "#sameFrame" : "#crossFrame"}});
      const auto destination =
          (remote ? cross : site) +
          "/frame-branch.html?remaining=0&mode=same&level=99";
      const auto marker = std::string(remote ? "localhost|99" : "127.0.0.1|99");
      const auto predicate =
          "()=>{if(typeof frameMarker!=='undefined'&&frameMarker===" +
          JsonDoc(marker).dump() +
          ")return true;if(document.readyState==='loading')return false;"
          "if(!window.ownedStarted){window.ownedStarted=true;setTimeout(()=>"
          "location.href=" +
          JsonDoc(destination).dump() + ",30)}return new Promise(()=>{})}";
      const auto started = std::chrono::steady_clock::now();
      const auto result = call(
          "page_wait",
          {{"type", "function"}, {"expression", predicate}, {"timeout", 4000}});
      check(result.at("condition") == true,
            "condition resumes after document replacement");
      check(eval("frameMarker") == marker,
            "condition completes only in the replacement document");
      check(std::chrono::steady_clock::now() - started < MsDuration(3000),
            "discarded promise does not consume the full action timeout");
      check(call("browser_status").at("frameDepth") ==
                (std::string(scenario) == "root" ? 0 : 1),
            "condition polling preserves selected frame depth");
      call("frame_reset");
    }
    call("page_navigate", {{"url", site + "/frame-host.html"}});
    eval("window.ownedEffects=0;addEventListener('message',e=>{if(e.data==='"
         "owned-eval-effect')ownedEffects++});true");
    call("frame_enter", {{"selector", "#crossFrame"}});
    const auto destination =
        site + "/frame-branch.html?remaining=0&mode=same&level=100";
    const auto source = "parent.postMessage('owned-eval-effect','*');"
                        "setTimeout(()=>location.href=" +
                        JsonDoc(destination).dump() +
                        ",30);new Promise(()=>{})";
    const auto started = std::chrono::steady_clock::now();
    bool failed = false;
    try {
      call("page_evaluate", {{"script", source}, {"timeout", 4000}});
    } catch (const BridgeError &) {
      failed = true;
    }
    check(failed, "arbitrary script reports context loss instead of replaying");
    check(std::chrono::steady_clock::now() - started < MsDuration(3000),
          "arbitrary pending script fails promptly when its document is gone");
    call("page_wait",
         {{"type", "function"},
          {"expression",
           "typeof frameMarker!=='undefined'&&frameMarker==='127.0.0.1|100'"}});
    call("frame_reset");
    check(eval("ownedEffects") == 1, "arbitrary script effect is delivered "
                                     "exactly once across process replacement");
    CdpChannel observer(static_cast<unsigned>(std::stoul(argv[1])));
    const auto observed_session =
        observer
            .call("Target.attachToTarget",
                  {{"targetId", owned_target}, {"flatten", true}})
            .at("sessionId")
            .get<std::string>();
    std::string survivor;
    const auto inventory = observer.call("Target.getTargets");
    for (const auto &target : inventory.at("targetInfos"))
      if (target.at("type") == "page" && target.at("targetId") != owned_target)
        survivor = target.at("targetId").get<std::string>();
    check(!survivor.empty(),
          "owned Chrome has a separate untouched survivor tab");
    std::exception_ptr observer_error;
    bool closed = false;
    std::jthread closing([&] {
      try {
        const auto until = std::chrono::steady_clock::now() + MsDuration(3000);
        while (observer
                   .call("Runtime.evaluate",
                         {{"expression", "!!window.readyToClose"},
                          {"returnByValue", true}},
                         observed_session)
                   .at("result")
                   .value("value", false) != true) {
          if (std::chrono::steady_clock::now() >= until)
            throw BridgeError("close observer never saw active condition");
          std::this_thread::sleep_for(MsDuration(5));
        }
        closed =
            observer.call("Target.closeTarget", {{"targetId", owned_target}})
                .at("success");
      } catch (...) {
        observer_error = std::current_exception();
      }
    });
    std::string close_error;
    try {
      call("page_wait",
           {{"type", "function"},
            {"timeout", 4000},
            {"expression",
             "()=>{window.closedWaitEffects=(window.closedWaitEffects??0)+1;"
             "window.readyToClose=true;return new Promise(()=>{})}"}});
    } catch (const BridgeError &error) {
      close_error = error.what();
    }
    closing.join();
    if (observer_error)
      std::rethrow_exception(observer_error);
    check(closed,
          "independent observer closed only the owned condition target");
    check(close_error.find("Selected page closed") != std::string::npos,
          "condition reports original target closure rather than changing "
          "tabs: " +
              close_error);
    const auto survivor_session =
        observer
            .call("Target.attachToTarget",
                  {{"targetId", survivor}, {"flatten", true}})
            .at("sessionId")
            .get<std::string>();
    check(observer.call("Runtime.evaluate",
                        {{"expression", "typeof closedWaitEffects"},
                         {"returnByValue", true}},
                        survivor_session)
                  .at("result")
                  .at("value") == "undefined",
          "abandoned condition is never evaluated on the surviving tab");
    std::cout << checks << " condition checks passed; 0 failed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
