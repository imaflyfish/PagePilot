#include <pagepilot/tool_runtime.hpp>
#include <cstdlib>
#include <iostream>
#include <pagepilot/node_tools.hpp>
#include <thread>
using namespace pagepilot;
int main(int argc, char **argv) {
  if (argc != 2) return 2;
  try {
    const auto port = static_cast<unsigned>(std::stoul(argv[1]));
    ToolRuntime runtime(port);
    ToolCatalog catalog;
    unsigned checks = 0;
    auto check = [&](bool condition, const std::string &message) {
      if (!condition) throw BridgeError(message);
      ++checks;
    };
    auto call = [&](const std::string &name, JsonDoc args = JsonDoc::object()) {
      return runtime.invoke(catalog.resolve(name, args, true));
    };
    auto eval = [&](const std::string &script) {
      return call("page_evaluate", {{"script", script}}).at("result");
    };
    // Keep the only surviving page first in the workspace's stable inventory.
    call("browser_status");
    const auto survivor = runtime.browser().current_target();
    eval("document.body.innerHTML='<input id=survivor>';document.querySelector('input').focus();true");
    const auto target = call("tab_create").at("target").get<std::string>();
    eval("document.body.innerHTML='<input id=typing>';true");
    CdpChannel observer(port);
    const auto session = observer.call("Target.attachToTarget", {{"targetId", target}, {"flatten", true}}).at("sessionId").get<std::string>();
    std::exception_ptr observer_error;
    bool closed = false;
    std::jthread closing([&] {
      try {
        const auto until = std::chrono::steady_clock::now() + MsDuration(5000);
        while (observer.call("Runtime.evaluate", {{"expression", "document.querySelector('#typing').value"}, {"returnByValue", true}}, session).at("result").value("value", JsonDoc()) != "a") {
          if (std::chrono::steady_clock::now() >= until) throw BridgeError("observer did not see the first input character");
          std::this_thread::sleep_for(MsDuration(5));
        }
        closed = observer.call("Target.closeTarget", {{"targetId", target}}).at("success");
      } catch (...) { observer_error = std::current_exception(); }
    });
    std::string failure;
    try {
      call("element_type", {{"selector", "#typing"}, {"text", "abc"}, {"mode", "slow"}, {"delay", 300}, {"timeout", 4000}});
    } catch (const BridgeError &error) { failure = error.what(); }
    closing.join();
    if (observer_error) std::rethrow_exception(observer_error);
    check(closed, "observer closed the original target after its first character");
    check(!failure.empty(), "interrupted typing reports a failure");
    const auto survivor_session = observer.call("Target.attachToTarget", {{"targetId", survivor}, {"flatten", true}}).at("sessionId").get<std::string>();
    const auto untouched = observer.call("Runtime.evaluate", {{"expression", "document.querySelector('#survivor').value"}, {"returnByValue", true}}, survivor_session).at("result").at("value");
    check(untouched == "", "remaining text escaped into a surviving tab: " + untouched.dump());
    check(call("element_fill", {{"selector", "#survivor"}, {"text", "next-request"}}).at("currentValue") == "next-request", "the next explicit action can use the surviving tab");
    // A library user may retain a remote element while changing selection.
    // Its object ID must stay in the session that produced it.
    DomTools elements(runtime.browser(), MsDuration(4000));
    auto lease = elements.locate({{"selector", "#survivor"}});
    call("tab_create");
    eval("document.body.innerHTML='<input id=survivor value=other-tab>';true");
    check(lease.call({{"operation", "read"}, {"type", "value"}}, MsDuration(1000)).at("value") == "next-request", "an element lease retains its creating tab session");
    check(eval("document.querySelector('#survivor').value") == "other-tab", "leased read leaves the selected tab untouched");
    const auto *fixture = std::getenv("PAGEPILOT_FIXTURE_URL");
    if (!fixture) throw BridgeError("missing owned fixture URL");
    call("page_navigate", {{"url", std::string(fixture) + "/frame-host.html"}});
    DomTools frame_elements(runtime.browser(), MsDuration(4000));
    auto parent = frame_elements.locate({{"selector", "#frame-input"}});
    call("frame_enter", {{"selector", "#crossFrame"}});
    call("element_fill", {{"selector", "#frame-input"}, {"text", "frame-value"}});
    auto child = frame_elements.locate({{"selector", "#frame-input"}});
    check(parent.call({{"operation", "read"}, {"type", "value"}}, MsDuration(1000)).at("value") == "ROOT", "parent lease stays in its original session while an OOP frame is selected");
    call("frame_reset");
    check(child.call({{"operation", "read"}, {"type", "value"}}, MsDuration(1000)).at("value") == "frame-value", "OOP lease stays in its creating session after frame reset");
    eval("document.querySelector('#crossFrame').remove();true");
    bool detached = false;
    try { child.call({{"operation", "focus"}}, MsDuration(1000)); }
    catch (const BridgeError &) { detached = true; }
    check(detached, "a destroyed OOP object fails instead of rebinding");
    check(eval("document.querySelector('#frame-input').value") == "ROOT", "destroyed lease leaves same-selector parent control unchanged");
    call("tab_close");
    std::cout << checks << " target-affinity checks passed; 0 failed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
