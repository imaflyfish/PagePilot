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
  std::string step;
  try {
    const auto *fixture = std::getenv("PAGEPILOT_FIXTURE_URL");
    if (!fixture)
      throw BridgeError("missing owned fixture");
    const std::string site = fixture;
    const auto port = static_cast<unsigned>(std::stoul(argv[1]));
    ToolRuntime runtime(port);
    ToolCatalog catalog;
    auto call = [&](const std::string &name, JsonDoc args = JsonDoc::object()) {
      return runtime.invoke(catalog.resolve(name, args, true));
    };
    auto eval = [&](const std::string &source) {
      return call("page_evaluate", {{"script", source}}).at("result");
    };
    auto navigate = [&](const std::string &suffix) {
      return call("page_navigate", {{"url", site + suffix}, {"timeout", 4000}});
    };
    call("tab_create");
    step = "click initiated delayed navigation";
    navigate("/click-targets.html");
    eval("document.querySelector('#first').onclick=()=>location.href=" +
         JsonDoc(site + "/nav-slow.html?delay=300&asset=200&label=AfterClick")
             .dump());
    auto clicked = call(
        "element_click",
        {{"selector", "#first"}, {"wait_after", "load"}, {"timeout", 4000}});
    check(clicked.at("clicked") == true &&
              eval("document.title") == "AfterClick",
          "post-click load cannot complete on the previous document");
    check(eval("navLoaded") == true,
          "post-click load includes destination resources");
    step = "beforeunload dismissal and acceptance";
    navigate("/click-targets.html");
    call("element_click", {{"selector", "#first"}});
    eval("window.onbeforeunload=e=>{e.preventDefault();e.returnValue='owned';};"
         "true");
    call("page_dialog", {{"action", "dismiss"}});
    rejects([&] { navigate("/nav-slow.html?label=Dismissed"); },
            "dismissed beforeunload is not navigation success");
    check(eval("document.title") == "Owned click targets",
          "dismissed navigation keeps original document");
    call("page_dialog", {{"action", "accept"}});
    check(navigate("/nav-slow.html?label=Accepted").at("title") == "Accepted",
          "accepted beforeunload reaches destination");
    check(eval("confirm('rule consumed')") == false,
          "beforeunload consumes the one-shot dialog rule");
    step = "superseding navigation";
    CdpChannel observer(port);
    const auto target = runtime.browser().current_target();
    const auto session = observer
                             .call("Target.attachToTarget",
                                   {{"targetId", target}, {"flatten", true}})
                             .at("sessionId")
                             .get<std::string>();
    std::exception_ptr observed_error;
    auto await_title = [&](const std::string &title) {
      const auto until = std::chrono::steady_clock::now() + MsDuration(3500);
      while (true) {
        const auto observed = observer.call(
            "Runtime.evaluate",
            {{"expression", "document.title"}, {"returnByValue", true}},
            session);
        if (observed.at("result").value("value", JsonDoc()) == title)
          return;
        if (std::chrono::steady_clock::now() >= until)
          throw BridgeError("observer did not see requested document");
        std::this_thread::sleep_for(MsDuration(5));
      }
    };
    std::jthread replacing([&] {
      try {
        await_title("Superseded");
        observer.call("Page.navigate",
                      {{"url", site + "/nav-slow.html?label=External"}},
                      session);
      } catch (...) {
        observed_error = std::current_exception();
      }
    });
    rejects(
        [&] {
          call("page_navigate",
               {{"url", site + "/nav-slow.html?asset=1500&label=Superseded"},
                {"timeout", 2500}});
        },
        "superseding navigation cannot satisfy the original loader");
    replacing.join();
    if (observed_error)
      std::rethrow_exception(observed_error);
    check(eval("document.title") == "External",
          "external navigation remains independently observable");
    check(navigate("/nav-slow.html?label=Recovered").at("title") == "Recovered",
          "following explicit navigation recovers");
    step = "target closure during generic load wait";
    call("page_navigate",
         {{"url", site + "/nav-slow.html?asset=1500&label=ClosingWait"},
          {"wait_until", "domcontentloaded"},
          {"timeout", 3000}});
    std::jthread closing_wait([&] {
      try {
        await_title("ClosingWait");
        std::this_thread::sleep_for(MsDuration(70));
        observer.call("Target.closeTarget", {{"targetId", target}});
      } catch (...) {
        observed_error = std::current_exception();
      }
    });
    bool wait_failed = false;
    try {
      runtime.browser().wait_ready("load", MsDuration(2500));
    } catch (const BridgeError &) {
      wait_failed = true;
    }
    closing_wait.join();
    if (observed_error)
      std::rethrow_exception(observed_error);
    check(wait_failed, "load wait cannot report success from a surviving tab");
    const auto replacement = call("tab_create").at("target").get<std::string>();
    const auto replacement_session =
        observer
            .call("Target.attachToTarget",
                  {{"targetId", replacement}, {"flatten", true}})
            .at("sessionId")
            .get<std::string>();
    step = "target closure during explicit navigation";
    std::jthread closing([&] {
      try {
        const auto until = std::chrono::steady_clock::now() + MsDuration(3500);
        while (observer
                   .call("Runtime.evaluate",
                         {{"expression", "document.title"},
                          {"returnByValue", true}},
                         replacement_session)
                   .at("result")
                   .value("value", JsonDoc()) != "Closing") {
          if (std::chrono::steady_clock::now() >= until)
            throw BridgeError("observer did not see closing navigation");
          std::this_thread::sleep_for(MsDuration(5));
        }
        observer.call("Target.closeTarget", {{"targetId", replacement}});
      } catch (...) {
        observed_error = std::current_exception();
      }
    });
    rejects(
        [&] {
          call("page_navigate",
               {{"url", site + "/nav-slow.html?asset=1500&label=Closing"},
                {"timeout", 3000}});
        },
        "closing target cannot succeed on another tab");
    closing.join();
    if (observed_error)
      std::rethrow_exception(observed_error);
    check(runtime.browser().connected(),
          "closed tab leaves browser transport available");
    call("tab_create");
    check(navigate("/nav-slow.html?label=NewTab").at("title") == "NewTab",
          "new tab remains usable after target closure");
    call("tab_close");
    std::cout << checks << " recovery checks passed; 0 failed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << step << ": " << error.what() << '\n';
    return 1;
  }
}
