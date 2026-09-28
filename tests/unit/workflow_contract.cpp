#include <pagepilot/tool_runtime.hpp>
#include <pagepilot/step_sequence.hpp>
#include <iostream>
using namespace pagepilot;
unsigned passed = 0, failed = 0;
void check(bool condition, const char *label) {
  if (condition)
    ++passed;
  else {
    ++failed;
    std::cerr << "FAIL: " << label << '\n';
  }
}
template <class F> void rejects(F action, const char *label) {
  try {
    action();
    check(false, label);
  } catch (const std::exception &) {
    check(true, label);
  }
}
JsonDoc action(const std::string &tool, JsonDoc arguments = JsonDoc::object()) {
  return {{"tool", tool}, {"args", std::move(arguments)}};
}
int main() {
  try {
    ToolCatalog catalog;
    ToolRuntime runtime(9);
    auto call = [&](const std::string &name, JsonDoc arguments = JsonDoc::object(),
                    bool compatibility = false) {
      return runtime.invoke(catalog.resolve(name, arguments, compatibility));
    };
    const auto empty = call("workflow_batch", {{"actions", JsonDoc::array()}});
    check(empty == JsonDoc({{"executed", 0}, {"results", JsonDoc::array()}}),
          "empty batch has an explicit empty result");
    auto batch = call(
        "workflow_batch",
        {{"actions",
          JsonDoc::array({action("browser_configure", {{"fast_timeout", 1234}}),
                       action("browser_settings")})}});
    check(batch.at("executed") == 2 &&
              batch.at("results")[1].at("result").at("timeouts").at("fast") ==
                  1234,
          "batch applies children in order through runtime");
    batch = call("workflow_batch",
                 {{"actions",
                   JsonDoc::array({action("browser_debug", {{"enabled", "false"}}),
                                action("browser_settings")})}});
    check(batch.at("executed") == 2 &&
              !batch.at("results")[0].at("success").get<bool>() &&
              batch.at("results")[1].at("success") == true,
          "invalid child schema is a row failure and batch continues");
    auto invalid = action("missing");
    invalid["stopOnError"] = true;
    batch = call(
        "workflow_batch",
        {{"actions", JsonDoc::array({invalid, action("browser_configure",
                                                  {{"fast_timeout", 999}})})}});
    check(batch.at("executed") == 1 &&
              call("browser_settings").at("timeouts").at("fast") == 1234,
          "stopOnError prevents later mutation");
    batch =
        call("workflow_batch",
             {{"actions",
               JsonDoc::array({action("get_config"), action("browser_settings")})},
              {"allow_legacy", true}});
    check(batch.at("results")[0].at("success") == false &&
              batch.at("results")[1].at("success") == true,
          "JSON cannot enable compatibility inside canonical workflow");
    auto legacy = call(
        "batch",
        {{"actions", JsonDoc::array({action("set_debug", {{"enabled", "yes"}}),
                                  action("set_debug", {{"enabled", false}}),
                                  action("toString"), nullptr})}},
        true);
    check(legacy.at("executed") == 4,
          "legacy batch retains individual malformed-row diagnostics");
    check(legacy.at("results")[0].at("success") == false &&
              legacy.at("results")[1].at("result") == JsonDoc({{"debug", false}}),
          "legacy child arguments still validated");
    check(legacy.at("results")[2].at("success") == false &&
              legacy.at("results")[3].at("success") == false,
          "unknown and null legacy rows never execute");
    rejects(
        [&] { call("workflow_batch", {{"actions", JsonDoc::array({nullptr})}}); },
        "canonical schema checks row structure up front");
    rejects(
        [&] {
          call("workflow_retry", {{"tool", "missing"}, {"max_retries", 0}});
        },
        "zero-attempt retry still validates its child");
    auto retry = call("workflow_retry", {{"tool", "browser_configure"},
                                         {"args", {{"fast_timeout", 1111}}},
                                         {"max_retries", 0}});
    check(retry == JsonDoc({{"success", false}, {"attempts", 0}}) &&
              call("browser_settings").at("timeouts").at("fast") == 1234,
          "zero retry attempts performs no child mutation");
    retry = call("workflow_retry", {{"tool", "browser_settings"},
                                    {"max_retries", 3},
                                    {"delay_ms", 0},
                                    {"success_check", "missing"}});
    check(retry.at("success") == false && retry.at("attempts") == 3,
          "max_retries means total attempts");
    retry = call("workflow_retry", {{"tool", "browser_settings"},
                                    {"max_retries", 3},
                                    {"delay_ms", 0},
                                    {"success_check", "timeouts"}});
    check(retry.at("success") == true && retry.at("attempts") == 1,
          "object-valued success check uses JS truthiness");
    retry = call("retry",
                 {{"tool", "set_debug"},
                  {"args", {{"enabled", false}}},
                  {"max_retries", 1}},
                 true);
    check(retry == JsonDoc({{"success", true},
                         {"attempts", 1},
                         {"result", {{"debug", false}}}}),
          "retry without success check accepts a nonthrowing false-valued "
          "result");
    rejects(
        [&] {
          call("workflow_retry",
               {{"tool", "browser_settings"}, {"max_retries", -1}});
        },
        "negative attempt limit refused");
    rejects(
        [&] {
          call("workflow_retry",
               {{"tool", "browser_settings"}, {"max_retries", 1.5}});
        },
        "fractional attempt limit refused");
    rejects(
        [&] {
          call("workflow_retry",
               {{"tool", "browser_settings"}, {"max_retries", 11}});
        },
        "attempt limit bounded");
    retry = call("workflow_retry", {{"tool", "browser_settings"},
                                    {"max_retries", 3},
                                    {"delay_ms", 200},
                                    {"success_check", "missing"},
                                    {"timeout", 50}});
    check(retry.at("timed_out") == true && retry.at("attempts") == 1,
          "retry delay cannot extend total deadline");
    check(call("browser_settings").at("timeouts").at("fast") == 1234,
          "deadline scope restored after workflow failure");

    auto steps = call("workflow_steps",
                      {{"steps", JsonDoc::array({action("browser_configure",
                                                     {{"fast_timeout", 2222}}),
                                              action("browser_settings")})},
                       {"max_step_retries", 0},
                       {"return_intermediate", true}});
    check(steps.at("executed") == 2 && steps.at("succeeded") == 2 &&
              steps.at("failed") == 0,
          "steps report exact success/failure counts");
    check(steps.at("steps")[0].at("attempts") == 1 &&
              steps.at("last_result").at("timeouts").at("fast") == 2222,
          "zero extra retries still executes once and last result is retained");
    check(steps.at("steps")[1].contains("result"),
          "intermediate results requested explicitly");
    steps = call("workflow_steps",
                 {{"steps", JsonDoc::array({action("browser_settings")})}});
    check(!steps.at("steps")[0].contains("result") &&
              steps.at("last_result").is_object(),
          "summary mode omits per-step full results but retains last result");
    auto optional = action("missing");
    optional["optional"] = true;
    steps = call(
        "workflow_steps",
        {{"steps", JsonDoc::array({optional, action("browser_configure",
                                                 {{"fast_timeout", 3333}})})}});
    check(steps.at("executed") == 2 && steps.at("failed") == 1 &&
              call("browser_settings").at("timeouts").at("fast") == 3333,
          "optional failure continues while remaining visible");
    steps = call("workflow_steps",
                 {{"steps", JsonDoc::array({action("missing"),
                                         action("browser_configure",
                                                {{"fast_timeout", 4444}})})}});
    check(steps.at("executed") == 1 &&
              call("browser_settings").at("timeouts").at("fast") == 3333,
          "required failure stops default step sequence");
    steps = call("workflow_steps",
                 {{"steps", JsonDoc::array({action("missing"),
                                         action("browser_configure",
                                                {{"fast_timeout", 4444}})})},
                  {"stop_on_error", false}});
    check(steps.at("executed") == 2 && steps.at("succeeded") == 1,
          "stop_on_error false permits later steps");
    for (const auto &name : {"workflow_batch", "workflow_retry",
                             "workflow_steps", "batch", "retry", "run_steps"}) {
      JsonDoc args =
          std::string(name).find("batch") != std::string::npos
              ? JsonDoc{{"actions", JsonDoc::array()}}
              : (std::string(name).find("retry") != std::string::npos
                     ? JsonDoc{{"tool", "browser_settings"}}
                     : JsonDoc{{"steps",
                             JsonDoc::array({action("browser_settings")})}});
      steps = call("workflow_steps",
                   {{"steps", JsonDoc::array({action(name, args)})}}, true);
      check(steps.at("failed") == 1 &&
                !steps.at("steps")[0].contains("attempts"),
            "step workflow rejects every canonical/legacy nested workflow "
            "before dispatch");
    }
    rejects([&] { call("workflow_steps", {{"steps", JsonDoc::array()}}); },
            "empty steps refused");
    JsonDoc too_many = JsonDoc::array();
    for (int i = 0; i < 51; ++i)
      too_many.push_back(action("browser_settings"));
    rejects([&] { call("workflow_steps", {{"steps", too_many}}); },
            "step count bounded at 50");
    auto waiting = action("browser_configure", {{"fast_timeout", 5555}});
    waiting["wait_before"] = 200;
    steps = call("workflow_steps",
                 {{"steps", JsonDoc::array({waiting})}, {"timeout", 40}});
    check(steps.at("timed_out") == true &&
              steps.at("steps")[0].at("attempts") == 0 &&
              call("browser_settings").at("timeouts").at("fast") == 4444,
          "wait-before deadline stops before side effects");
    steps = call("workflow_steps",
                 {{"steps", JsonDoc::array({action("browser_configure",
                                                {{"fast_timeout", 5555}})})},
                  {"step_timeout", 0},
                  {"max_step_retries", 0}});
    check(steps.at("failed") == 1 && steps.at("steps")[0].at("attempts") == 1 &&
              call("browser_settings").at("timeouts").at("fast") == 4444,
          "zero step budget rejects execution without changing configuration");
    for (const auto &depth : JsonDoc::array({-100, "invalid", nullptr})) {
      auto nested = action("browser_configure", {{"fast_timeout", 5555}});
      for (int i = 0; i < 8; ++i)
        nested = i % 2 ? action("batch", {{"actions", JsonDoc::array({nested})},
                                          {"_depth", depth}})
                       : action("retry", {{"tool", nested.at("tool")},
                                          {"args", nested.at("args")},
                                          {"max_retries", 1},
                                          {"_depth", depth}});
      rejects([&] { call(nested.at("tool"), nested.at("args"), true); },
              "external depth fields cannot bypass native recursion guard");
    }
    check(call("browser_settings").at("timeouts").at("fast") == 4444,
          "recursion refusal prevents deepest mutation");
    JsonDoc group = JsonDoc::array();
    for (int i = 0; i < 600; ++i)
      group.push_back(action("browser_settings"));
    rejects(
        [&] {
          call("workflow_batch",
               {{"actions",
                 JsonDoc::array(
                     {action("workflow_batch", {{"actions", group}}),
                      action("workflow_batch", {{"actions", group}})})}});
        },
        "nested batches share a 1024-dispatch ceiling");
    check(call("browser_settings").at("timeouts").at("fast") == 4444,
          "dispatch budget resets for next independent request");
    BrowserSession browser(9);
    unsigned invoked = 0;
    StepSequence bounded(
        browser, catalog,
        [&](const auto &) {
          ++invoked;
          return JsonDoc{{"large", std::string(33 * 1024 * 1024, 'x')}};
        },
        false);
    rejects(
        [&] {
          bounded.run(catalog.resolve(
              "workflow_batch",
              {{"actions", JsonDoc::array({action("browser_settings"),
                                        action("browser_settings"),
                                        action("browser_settings")})}}));
        },
        "aggregate output is bounded before accumulation continues");
    check(invoked == 2, "output ceiling prevents a later child from executing");
    std::cout << passed << " workflow contract checks passed; " << failed
              << " failed\n";
    return failed ? 1 : 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
