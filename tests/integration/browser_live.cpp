#include "../support/checks.hpp"
#include <cstdlib>
#include <iostream>
#include <pagepilot/tool_runtime.hpp>
using namespace pagepilot;
using namespace pagepilot_tests;
int main(int argc, char **argv) {
  if (argc != 2)
    return 2;
  try {
    const auto *site = std::getenv("PAGEPILOT_FIXTURE_URL");
    if (!site)
      throw BridgeError("missing owned fixture URL");
    ToolRuntime runtime(static_cast<unsigned>(std::stoul(argv[1])));
    ToolCatalog catalog;
    auto call = [&](const std::string &name,
                    JsonDoc arguments = JsonDoc::object()) {
      return runtime.invoke(catalog.resolve(name, arguments));
    };
    const auto baseline = call("tab_list").at("count").get<std::size_t>();
    check(call("browser_status").at("connected").get<bool>(),
          "native runtime connects");
    check(call("browser_health").at("healthy").get<bool>(),
          "health reflects real connection and tab");
    check(
        call("tab_create", {{"url", "about:blank"}}).at("created").get<bool>(),
        "owned tab created");
    check(call("tab_list").at("count") == baseline + 1,
          "created tab appears once");
    auto result =
        call("page_navigate", {{"url", std::string(site) + "/page.html"}});
    check(result.at("title") == "PagePilot fixture",
          "navigation observes new document title");
    check(call("page_read", {{"type", "text"}})
                  .at("text")
                  .get<std::string>()
                  .find("PagePilot local fixture") != std::string::npos,
          "page text read");
    check(call("page_read", {{"type", "viewport"}})
                  .at("viewport")
                  .at("width")
                  .get<int>() > 0,
          "real viewport measured");
    check(call("page_evaluate", {{"script", "Promise.resolve(42)"}})
                  .at("result") == 42,
          "async page result awaited");
    rejects(
        [&] {
          call("page_evaluate", {{"script", "throw new Error('owned error')"}});
        },
        "page exception propagates");
    call("page_evaluate",
         {{"script", "console.log('native console evidence',123); true"}});
    check(call("page_console").at("count").get<unsigned>() >= 1,
          "native console event captured");
    call("page_console", {{"clear", true}});
    check(call("page_console").at("count") == 0,
          "console clearing takes effect");
    result =
        call("page_navigate", {{"url", std::string(site) + "/frame.html"}});
    check(result.at("title") == "Owned frame", "second document observed");
    check(call("page_back").at("url") == std::string(site) + "/page.html",
          "history back targets prior document");
    check(call("page_forward").at("url") == std::string(site) + "/frame.html",
          "history forward targets next document");
    check(call("page_reload").at("reloaded").get<bool>(), "reload completes");
    check(call("page_stop").at("stopped").get<bool>(),
          "stop-loading command reaches Chrome");
    rejects([&] { call("tab_activate", {{"index", 9999}}); },
            "invalid tab index refused");
    rejects(
        [&] {
          call("page_evaluate",
               {{"script", "new Promise(()=>{})"}, {"timeout", 50}});
        },
        "unresolved promise hits request deadline");
    check(call("page_evaluate", {{"script", "6*7"}}).at("result") == 42,
          "connection remains usable after bounded request failure");
    check(call("browser_configure", {{"fast_timeout", 500}})
              .at("updated")
              .get<bool>(),
          "runtime configuration updates");
    check(call("browser_settings").at("timeouts").at("fast") == 500,
          "updated timeout visible");
    check(call("browser_metrics").at("errors").get<unsigned>() >= 3,
          "actual failures counted");
    call("tab_close");
    check(call("tab_list").at("count") == baseline,
          "close removes only created tab");
    check(call("browser_reconnect").at("reconnected").get<bool>(),
          "fresh reconnect restores native session");
    runtime.browser().disconnect();
    check(discover_browser(static_cast<unsigned>(std::stoul(argv[1])))
              .contains("Browser"),
          "disconnect keeps owned browser alive for harness cleanup");
    std::cout << passed << " live-browser checks passed; " << failed
              << " failed\n";
    return failed ? 1 : 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
