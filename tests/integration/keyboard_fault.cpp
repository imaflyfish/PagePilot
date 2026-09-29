#include "../support/aborting_checks.hpp"
#include <iostream>
#include <pagepilot/session.hpp>
#include <thread>
using namespace pagepilot;
using namespace pagepilot_aborting_tests;
int main(int argc, char **argv) {
  if (argc != 3)
    return 2;
  try {
    const auto port = static_cast<unsigned>(std::stoul(argv[1]));
    const std::string mode = argv[2];
    BrowserSession browser(port);
    browser.connect();
    CdpChannel observer(port, MsDuration(2000));
    std::stop_source source;
    bool cancellation_observed = false;
    std::jthread stopping;
    if (mode == "cancel")
      stopping = std::jthread([&] {
        try {
          cancellation_observed = observer
                                      .call("Owned.waiting", JsonDoc::object(),
                                            {}, MsDuration(4000))
                                      .at("waiting");
        } catch (...) {
        }
        source.request_stop();
      });
    const auto started = std::chrono::steady_clock::now();
    bool matched = false;
    try {
      CancelScope scope(source.get_token());
      browser.press_keyboard(
          "Control+Shift+KeyA",
          MsDuration(mode == "timeout" || mode == "cleanup-timeout" ? 200
                                                                    : 3000));
    } catch (const RequestAborted &) {
      matched = mode == "cancel";
    } catch (const DeadlineReached &) {
      matched = mode == "timeout" || mode == "cleanup-timeout";
    } catch (const WireFailure &error) {
      matched = error.code == (mode == "session-gone" ? -32001 : -32000);
    }
    if (stopping.joinable())
      stopping.join();
    check(matched && (mode != "cancel" || cancellation_observed),
          "the injected fault, including observed cancellation, reaches the "
          "caller");
    check(std::chrono::steady_clock::now() - started < MsDuration(2000),
          "deadline and cancellation cleanup remain bounded");
    const auto observed = observer.call("Owned.records");
    check(observed.at("attachments") == 1,
          "cleanup never attaches a replacement target");
    JsonDoc actual = JsonDoc::array();
    for (const auto &row : observed.at("keys")) {
      check(row.at("sessionId") == "owned-session-1",
            "every cleanup event stays on the original session");
      const auto &event = row.at("params");
      actual.push_back(JsonDoc::array(
          {event.at("type"), event.at("code"), event.at("modifiers")}));
    }
    auto expected =
        JsonDoc::array({JsonDoc::array({"rawKeyDown", "ControlLeft", 2}),
                        JsonDoc::array({"rawKeyDown", "ShiftLeft", 10})});
    if (mode == "keyup-error") {
      expected.push_back(JsonDoc::array({"rawKeyDown", "KeyA", 10}));
      expected.push_back(JsonDoc::array({"keyUp", "KeyA", 10}));
      expected.push_back(JsonDoc::array({"keyUp", "KeyA", 10}));
    }
    expected.push_back(JsonDoc::array({"keyUp", "ShiftLeft", 2}));
    expected.push_back(JsonDoc::array({"keyUp", "ControlLeft", 0}));
    check(actual == expected, "partial input releases only potentially held "
                              "keys, in reverse order: " +
                                  actual.dump());
    browser.press_keyboard("KeyZ", MsDuration(2000));
    const auto recovered = observer.call("Owned.records").at("keys");
    const auto &down = recovered.at(recovered.size() - 2).at("params");
    const auto &up = recovered.back().at("params");
    check(recovered.size() == actual.size() + 2 && down.at("code") == "KeyZ" &&
              down.at("modifiers") == 0 && up.at("type") == "keyUp" &&
              up.at("modifiers") == 0,
          "a following key uses no abandoned modifiers or late responses");
    check(browser.connected(), "fault recovery retains the healthy transport");
    std::cout << JsonDoc({{"mode", mode}, {"checks", checks}, {"passed", true}})
                     .dump()
              << '\n';
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
