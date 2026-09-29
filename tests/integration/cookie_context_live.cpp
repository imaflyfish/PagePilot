#include <cstdlib>
#include <iostream>
#include <pagepilot/session.hpp>
using namespace pagepilot;
int main(int argc, char **argv) {
  if (argc != 2)
    return 2;
  try {
    const auto *fixture = std::getenv("PAGEPILOT_FIXTURE_URL");
    if (!fixture || !std::string(fixture).starts_with("http://127.0.0.1:"))
      throw BridgeError(
          "Cookie-context tests require the owned-browser fixture harness");
    unsigned checks = 0;
    auto check = [&](bool condition, const char *label) {
      if (!condition)
        throw BridgeError(label);
      ++checks;
    };
    auto rejects = [&](auto action, const char *label) {
      bool failed = false;
      try {
        action();
      } catch (const BridgeError &) {
        failed = true;
      }
      check(failed, label);
    };
    const auto port = static_cast<unsigned>(std::stoul(argv[1]));
    CdpChannel observer(port);
    auto raw_cookies = [&](const JsonDoc &scope) {
      return observer.call("Storage.getCookies", scope).at("cookies");
    };
    auto marker = [&](const std::string &value) {
      return JsonDoc{{"name", "owned-context-marker"},
                     {"value", value},
                     {"domain", "example.test"},
                     {"path", "/"}};
    };
    observer.call("Storage.clearCookies");
    observer.call("Storage.setCookies",
                  {{"cookies", JsonDoc::array({marker("default")})}});
    BrowserSession normal(port);
    normal.connect();
    check(normal.manage_cookies({{"name", "owned-context-marker"}})
                  .at("cookies")
                  .at(0)
                  .at("value") == "default",
          "default profile cookie read succeeds with its reported context ID");
    normal.manage_cookies({{"action", "clear"}});
    check(raw_cookies(JsonDoc::object()).empty(),
          "default profile clear succeeds");
    normal.manage_cookies({{"action", "set"},
                           {"name", "owned-context-marker"},
                           {"value", "default"},
                           {"domain", "example.test"}});
    check(raw_cookies(JsonDoc::object()).at(0).at("value") == "default",
          "default profile cookie write succeeds");
    normal.disconnect();

    const auto context =
        observer.call("Target.createBrowserContext").at("browserContextId");
    const auto isolated =
        observer
            .call("Target.createTarget",
                  {{"url", "about:blank"}, {"browserContextId", context}})
            .at("targetId");
    const auto inventory = observer.call("Target.getTargets");
    // This program runs only in the harness's disposable owned browser. Keep
    // the private page alive so the next workspace selects it unambiguously.
    for (const auto &target : inventory.at("targetInfos"))
      if (target.at("type") == "page" && target.at("targetId") != isolated)
        observer.call("Target.closeTarget",
                      {{"targetId", target.at("targetId")}});
    BrowserSession private_page(port);
    private_page.connect();
    check(private_page.current_target() == isolated.get<std::string>(),
          "workspace selects the owned private context");
    check(private_page.manage_cookies(JsonDoc::object()).at("cookies").empty(),
          "private context does not inherit default cookies");
    private_page.manage_cookies({{"action", "set"},
                                 {"name", "owned-context-marker"},
                                 {"value", "private"},
                                 {"domain", "example.test"}});
    check(private_page.manage_cookies(JsonDoc::object())
                  .at("cookies")
                  .at(0)
                  .at("value") == "private",
          "private context cookie round trip");
    check(raw_cookies(JsonDoc::object()).at(0).at("value") == "default",
          "private write preserves default cookie");
    const auto created = private_page.create_tab().at("target");
    check(observer.call("Target.getTargetInfo", {{"targetId", created}})
                  .at("targetInfo")
                  .at("browserContextId") == context,
          "new tab remains inside the selected private context");
    check(private_page.tabs().at("count") == 2,
          "private tab inventory excludes other contexts");
    const auto default_target =
        observer.call("Target.createTarget", {{"url", "about:blank"}})
            .at("targetId");
    private_page.manage_cookies({{"action", "clear"}});
    check(raw_cookies({{"browserContextId", context}}).empty(),
          "private clear targets its own store");
    check(raw_cookies(JsonDoc::object()).at(0).at("value") == "default",
          "private clear preserves default cookies");
    private_page.manage_cookies({{"action", "set"},
                                 {"name", "owned-context-marker"},
                                 {"value", "private-again"},
                                 {"domain", "example.test"}});
    private_page.manage_cookies(
        {{"action", "delete"}, {"name", "owned-context-marker"}});
    check(raw_cookies({{"browserContextId", context}}).empty(),
          "private delete targets the selected page context");
    check(raw_cookies(JsonDoc::object()).at(0).at("value") == "default",
          "private delete preserves same-name default cookie");

    observer.call("Target.disposeBrowserContext",
                  {{"browserContextId", context}});
    rejects([&] { private_page.manage_cookies({{"action", "clear"}}); },
            "disposed private context cannot clear the default profile");
    rejects([&] { private_page.create_tab(); },
            "disposed private context cannot create a default-profile tab");
    check(raw_cookies(JsonDoc::object()).at(0).at("value") == "default",
          "failed private operations preserve default cookies");
    const auto remaining = observer.call("Target.getTargets");
    unsigned pages = 0;
    for (const auto &target : remaining.at("targetInfos"))
      if (target.at("type") == "page") {
        ++pages;
        check(target.at("targetId") == default_target,
              "failed private creation does not create a foreign target");
      }
    check(pages == 1, "only the deliberately restored default page remains");
    observer.call("Storage.clearCookies");
    std::cout << checks << " cookie-context checks passed; 0 failed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
