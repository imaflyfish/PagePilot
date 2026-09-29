#include <cstdlib>
#include <iostream>
#include <pagepilot/session.hpp>
using namespace pagepilot;
unsigned passed = 0, failed = 0;
std::string step;
void check(bool value, const char *label) {
  if (value)
    ++passed;
  else {
    ++failed;
    std::cerr << "FAIL: " << label << '\n';
  }
}
template <class F> void rejects(F operation, const char *label) {
  try {
    operation();
    check(false, label);
  } catch (const BridgeError &) {
    check(true, label);
  }
}
int main(int argc, char **argv) {
  if (argc != 2)
    return 2;
  try {
    const auto *fixture = std::getenv("PAGEPILOT_FIXTURE_URL");
    if (!fixture)
      throw BridgeError("Missing fixture URL");
    const std::string site = fixture;
    BrowserSession browser(static_cast<unsigned>(std::stoul(argv[1])));
    browser.create_tab();
    auto loader = [&] {
      return browser.page_call("Page.getFrameTree")
          .at("frameTree")
          .at("frame")
          .at("loaderId");
    };
    auto state = [&] {
      return browser.evaluate(
          "({sequence:navSequence,label:navLabel,loaded:navLoaded,fetched:"
          "navFetched,state:document.readyState})");
    };
    auto elapsed = [](auto begin) {
      return std::chrono::duration_cast<MsDuration>(
                 std::chrono::steady_clock::now() - begin)
          .count();
    };
    step = "delayed headers and resource load";
    const auto old = loader();
    auto begin = std::chrono::steady_clock::now();
    auto page =
        browser.navigate(site + "/nav-slow.html?delay=180&asset=300&label=Slow",
                         "load", MsDuration(5000));
    check(
        page.at("title") == "Slow" && elapsed(begin) >= 400,
        "navigation waits for the requested delayed document and its resource");
    check(loader() != old && state().at("loaded") == true,
          "load result belongs to new loader and completed page");
    const auto initial_sequence = state().at("sequence"),
               initial_loader = loader();
    step = "delayed reload";
    begin = std::chrono::steady_clock::now();
    auto reloaded = browser.reload(MsDuration(5000));
    check(reloaded.at("reloaded") == true && elapsed(begin) >= 400,
          "reload cannot accept the prior complete document");
    check(loader() != initial_loader &&
              state().at("sequence") != initial_sequence &&
              state().at("loaded") == true,
          "reload observes a new actual server response and loader");
    step = "DOMContentLoaded";
    begin = std::chrono::steady_clock::now();
    page = browser.navigate(site + "/nav-slow.html?asset=1200&label=DOMReady",
                            "domcontentloaded", MsDuration(4000));
    auto observed = state();
    check(page.at("title") == "DOMReady" && observed.at("loaded") == false &&
              elapsed(begin) < 1000,
          "DOMContentLoaded does not wait for a deliberately delayed image");
    check(observed.at("state") == "interactive",
          "DOMContentLoaded probes requested document's interactive state");
    browser.wait_ready("load", MsDuration(3000));
    check(state().at("loaded") == true,
          "later load observation sees the pending image complete");
    step = "network idle";
    browser.navigate(site + "/nav-slow.html?consume=0&label=UnreadPrior",
                     "load", MsDuration(3000));
    begin = std::chrono::steady_clock::now();
    page = browser.navigate(site + "/nav-slow.html?fetch=300&label=Quiet",
                            "networkidle", MsDuration(4000));
    check(page.at("title") == "Quiet" && state().at("fetched") == true &&
              elapsed(begin) >= 700,
          "network idle includes fetch completion and a quiet interval");
    check(state().at("label") == "Quiet" && elapsed(begin) < 3500,
          "unread fetch body from discarded document cannot keep replacement "
          "busy");
    step = "redirect commit";
    page = browser.navigate(
        site + "/nav-redirect?delay=100&to=%2Fnav-slow.html%3Fdelay%3D100%"
               "26asset%3D150%26label%3DRedirected",
        "load", MsDuration(4000));
    check(page.at("redirected") == true && page.at("title") == "Redirected",
          "redirect result describes final committed document");
    check(page.at("finalUrl") ==
                  site +
                      "/nav-slow.html?delay=100&asset=150&label=Redirected" &&
              state().at("loaded") == true,
          "redirect waits for final resources and reports final URL");
    step = "same-document navigation";
    const auto fragment_loader = loader(),
               fragment_sequence = state().at("sequence");
    const auto fragment_url =
        page.at("finalUrl").get<std::string>() + "#owned-fragment";
    page = browser.navigate(fragment_url, "load", MsDuration(2000));
    check(
        page.at("finalUrl") == fragment_url && loader() == fragment_loader &&
            state().at("sequence") == fragment_sequence,
        "fragment navigation keeps its document identity and still completes");
    step = "same-document history";
    auto moved = browser.history(-1, MsDuration(2000));
    check(moved.at("moved") == true &&
              browser.evaluate("location.hash") == "" &&
              loader() == fragment_loader,
          "back reaches the requested same-document entry");
    moved = browser.history(1, MsDuration(2000));
    check(moved.at("moved") == true &&
              browser.evaluate("location.hash") == "#owned-fragment" &&
              loader() == fragment_loader,
          "forward reaches the requested same-document entry");
    browser.evaluate(
        "history.pushState({owned:1},'', "
        "'#state-one');history.pushState({owned:2},'', '#state-two');true");
    moved = browser.history(-1, MsDuration(2000));
    check(moved.at("moved") == true &&
              browser.evaluate("history.state.owned") == 1,
          "history entry identity handles pushState without a new loader");
    step = "cross-document history";
    browser.navigate(
        site + "/nav-slow.html?delay=180&asset=200&label=HistoryDestination",
        "load", MsDuration(4000));
    const auto destination_loader = loader();
    moved = browser.history(-1, MsDuration(4000));
    check(moved.at("moved") == true && state().at("label") == "Redirected" &&
              loader() != destination_loader,
          "back confirms the requested document even with cache restoration");
    moved = browser.history(1, MsDuration(4000));
    check(moved.at("moved") == true &&
              state().at("label") == "HistoryDestination" &&
              state().at("loaded") == true,
          "forward waits for destination readiness");
    moved = browser.history(1, MsDuration(1000));
    check(moved.at("moved") == false &&
              state().at("label") == "HistoryDestination",
          "unavailable history leaves current page intact");
    step = "cross-process top-level navigation";
    std::string other = site;
    other.replace(other.find("127.0.0.1"), 9, "localhost");
    page = browser.navigate(
        other + "/nav-slow.html?delay=100&asset=120&label=OtherProcess", "load",
        MsDuration(5000));
    check(page.at("title") == "OtherProcess" &&
              browser.evaluate("location.hostname") == "localhost" &&
              state().at("loaded") == true,
          "top-level process change binds readiness to replacement default "
          "world");
    page = browser.navigate(site + "/nav-slow.html?delay=100&label=Returned",
                            "load", MsDuration(5000));
    check(page.at("title") == "Returned" &&
              browser.evaluate("location.hostname") == "127.0.0.1",
          "reverse process change reports the intended page");
    step = "timeout and recovery";
    begin = std::chrono::steady_clock::now();
    rejects(
        [&] {
          browser.navigate(site + "/nav-slow.html?delay=800&label=Late", "load",
                           MsDuration(90));
        },
        "slow commit cannot be reported successful before deadline");
    check(elapsed(begin) < 1000,
          "direct browser navigation shares one finite allowance");
    browser.page_call("Page.stopLoading");
    page = browser.navigate(site + "/nav-slow.html?label=Recovered", "load",
                            MsDuration(4000));
    check(page.at("title") == "Recovered" && state().at("label") == "Recovered",
          "next request recovers after aborted delayed navigation");
    rejects(
        [&] { browser.navigate(site + "/nav-drop", "load", MsDuration(1500)); },
        "network failure is not a success on old document");
    browser.navigate(site + "/nav-slow.html?label=Kept", "load",
                     MsDuration(4000));
    rejects(
        [&] {
          browser.navigate(site + "/nav-empty", "load", MsDuration(1500));
        },
        "204 response without page commit is not a false navigation success");
    check(state().at("label") == "Kept",
          "no-content navigation retains the previous actual document");
    rejects(
        [&] {
          browser.navigate(site + "/nav-slow.html?label=Wrong", "invalid",
                           MsDuration(1000));
        },
        "unknown readiness is rejected before navigation");
    check(state().at("label") == "Kept",
          "invalid readiness causes no new page request");
    browser.close_tab();
    check(
        browser.connected(),
        "closing owned navigation tab leaves shared browser connection alive");
  } catch (const std::exception &error) {
    std::cerr << "at " << step << ": " << error.what() << '\n';
    return 1;
  }
  std::cout << passed << " navigation checks passed; " << failed << " failed\n";
  return failed ? 1 : 0;
}
