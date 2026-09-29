#include "../support/checks.hpp"
#include <cstdlib>
#include <iostream>
#include <pagepilot/tool_runtime.hpp>
using namespace pagepilot;
using namespace pagepilot_tests;
std::string step;
JsonDoc action(const std::string &name, JsonDoc args = JsonDoc::object()) {
  return {{"tool", name}, {"args", std::move(args)}};
}
int main(int argc, char **argv) {
  if (argc != 2)
    return 2;
  try {
    const auto *site = std::getenv("PAGEPILOT_FIXTURE_URL");
    if (!site)
      throw BridgeError("Missing fixture URL");
    ToolCatalog catalog;
    ToolRuntime runtime(static_cast<unsigned>(std::stoul(argv[1])));
    auto call = [&](const std::string &name, JsonDoc args = JsonDoc::object()) {
      step = name + " " + args.dump();
      return runtime.invoke(catalog.resolve(name, args, true));
    };
    auto eval = [&](const std::string &script) {
      return call("page_evaluate", {{"script", script}}).at("result");
    };
    call("tab_create");
    call("page_navigate", {{"url", std::string(site) + "/page.html"}});
    auto batch =
        call("workflow_batch",
             {{"actions",
               JsonDoc::array(
                   {action("element_fill", {{"selector", "#person"},
                                            {"text", "Workflow 中文😀"}}),
                    action("element_click", {{"selector", "#count-button"}}),
                    action("element_read",
                           {{"selector", "#person"}, {"type", "value"}})})}});
    check(batch.at("executed") == 3 && batch.at("results")[2].at("result").at(
                                           "value") == "Workflow 中文😀",
          "batch sequence produces actual fill and read effects");
    check(eval("document.querySelector('#clicks').textContent") == "1",
          "batch performs exactly one native click");
    batch =
        call("batch",
             {{"actions",
               JsonDoc::array({action("set_debug", {{"enabled", "false"}}),
                               action("fill", {{"selector", "#person"},
                                               {"text", "legacy sequence"}}),
                               nullptr})}});
    check(batch.at("executed") == 3 &&
              batch.at("results")[0].at("success") == false &&
              batch.at("results")[2].at("success") == false,
          "legacy malformed rows remain independent failures");
    check(eval("document.querySelector('#person').value") == "legacy sequence",
          "legacy batch continues to valid browser child");
    auto stopping =
        action("element_click", {{"selector", "#missing"}, {"timeout", 50}});
    stopping["stopOnError"] = true;
    batch = call(
        "workflow_batch",
        {{"actions", JsonDoc::array({stopping, action("element_fill",
                                                      {{"selector", "#person"},
                                                       {"text", "wrong"}})})}});
    check(batch.at("executed") == 1 &&
              eval("document.querySelector('#person').value") ==
                  "legacy sequence",
          "stopOnError blocks later page mutation");

    eval("window.retrySeen=0;window.predicateSeen=0;window.failedSeen=0;window."
         "noAttempt=0;true");
    auto retry = call("workflow_retry",
                      {{"tool", "page_evaluate"},
                       {"args",
                        {{"script", "(()=>{if(++retrySeen<3)throw Error('try "
                                    "again');return retrySeen})()"}}},
                       {"max_retries", 5},
                       {"delay_ms", 0}});
    check(retry.at("success") == true && retry.at("attempts") == 3 &&
              retry.at("result").at("result") == 3,
          "exception retry reaches third real execution");
    check(eval("retrySeen") == 3,
          "successful retry stops without an extra script execution");
    retry =
        call("workflow_retry", {{"tool", "page_evaluate"},
                                {"args", {{"script", "++predicateSeen>=3"}}},
                                {"max_retries", 5},
                                {"delay_ms", 0},
                                {"success_check", "result"}});
    check(retry.at("success") == true && retry.at("attempts") == 3 &&
              eval("predicateSeen") == 3,
          "success_check retries falsy browser results");
    retry = call(
        "workflow_retry",
        {{"tool", "page_evaluate"},
         {"args", {{"script", "++failedSeen;throw Error('owned failure')"}}},
         {"max_retries", 2},
         {"delay_ms", 0}});
    check(retry.at("success") == false && retry.at("attempts") == 2 &&
              retry.at("error").get<std::string>().find("owned failure") !=
                  std::string::npos,
          "exhausted retry reports last actual script failure");
    check(eval("failedSeen") == 2,
          "max_retries is total calls rather than extra retries");
    retry = call("workflow_retry", {{"tool", "page_evaluate"},
                                    {"args", {{"script", "++noAttempt"}}},
                                    {"max_retries", 0}});
    check(retry.at("attempts") == 0 && eval("noAttempt") == 0,
          "zero retry attempts causes no page effect");
    for (const auto &expression : {"[]", "({})", "0", "null", "''"}) {
      retry = call("workflow_retry", {{"tool", "page_evaluate"},
                                      {"args", {{"script", expression}}},
                                      {"max_retries", 2},
                                      {"delay_ms", 0},
                                      {"success_check", "result"}});
      const bool expected =
          std::string(expression) == "[]" || std::string(expression) == "({})";
      check(retry.at("success") == expected &&
                retry.at("attempts") == (expected ? 1 : 2),
            "success_check matches JS truthiness of actual JSON result");
    }
    eval("window.deadlineSeen=0;true");
    // Allow the first browser round trip on shared runners. The requested
    // retry delay still exceeds the entire budget, so no second execution is
    // allowed; the workflow rejects that delay without sleeping for it.
    retry =
        call("workflow_retry", {{"tool", "page_evaluate"},
                                {"args", {{"script", "++deadlineSeen;false"}}},
                                {"success_check", "result"},
                                {"max_retries", 10},
                                {"delay_ms", 5000},
                                {"timeout", 2000}});
    check(retry.at("timed_out") == true && retry.at("attempts") == 1 &&
              eval("deadlineSeen") == 1,
          "overall retry deadline prevents delayed second execution");

    eval("window.stepSeen=0;window.stepOnce=0;window.noRetry=0;window.order=[];"
         "true");
    auto steps = call(
        "workflow_steps",
        {{"steps",
          JsonDoc::array(
              {action("page_evaluate",
                      {{"script", "(()=>{if(++stepSeen<2)throw Error('first "
                                  "attempt');return 'second attempt'})()"}}),
               action("element_fill",
                      {{"selector", "#person"}, {"text", "Steps 中文😀"}}),
               action("element_read",
                      {{"selector", "#person"}, {"type", "value"}})})},
         {"max_step_retries", 1},
         {"auto_wait", false},
         {"return_intermediate", true}});
    check(steps.at("executed") == 3 && steps.at("failed") == 0 &&
              steps.at("steps")[0].at("attempts") == 2,
          "steps retry count adds to first attempt");
    check(eval("stepSeen") == 2 &&
              steps.at("last_result").at("value") == "Steps 中文😀",
          "steps retain last real result and exact retry side effects");
    check(steps.at("steps")[1].at("brief").get<std::string>().find(
              "Steps 中文😀") != std::string::npos,
          "brief preserves Unicode text");
    check(steps.at("page").at("title") == "PagePilot fixture" &&
              steps.at("page").at("url") == std::string(site) + "/page.html",
          "workflow returns actual final root page information");
    auto optional =
        action("element_click", {{"selector", "#missing"}, {"timeout", 40}});
    optional["optional"] = true;
    steps =
        call("workflow_steps",
             {{"steps",
               JsonDoc::array(
                   {optional, action("element_fill",
                                     {{"selector", "#person"},
                                      {"text", "After optional failure"}})})},
              {"max_step_retries", 0},
              {"return_intermediate", true}});
    check(steps.at("executed") == 2 && steps.at("failed") == 1 &&
              steps.at("succeeded") == 1,
          "optional failure is counted while later browser steps continue");
    check(eval("document.querySelector('#person').value") ==
              "After optional failure",
          "optional failure preserves intended later mutation");
    steps =
        call("workflow_steps",
             {{"steps",
               JsonDoc::array(
                   {action("page_evaluate",
                           {{"script", "++stepOnce;throw Error('one only')"}}),
                    action("element_fill",
                           {{"selector", "#person"}, {"text", "wrong"}})})},
              {"max_step_retries", 0}});
    check(
        steps.at("executed") == 1 && steps.at("steps")[0].at("attempts") == 1 &&
            eval("stepOnce") == 1,
        "zero extra retries executes exactly once and stops required failure");
    check(eval("document.querySelector('#person').value") ==
              "After optional failure",
          "required failure prevents later browser mutation");
    steps =
        call("workflow_steps",
             {{"steps",
               JsonDoc::array({action(
                   "page_evaluate",
                   {{"script", "++noRetry;throw Error('retry disabled')"}})})},
              {"retry_on_fail", false},
              {"max_step_retries", 10}});
    check(steps.at("failed") == 1 && steps.at("steps")[0].at("attempts") == 1 &&
              eval("noRetry") == 1,
          "retry_on_fail false overrides positive extra retry count");
    auto bounded =
        action("element_click", {{"selector", "#missing"}, {"timeout", 1000}});
    bounded["optional"] = true;
    bounded["timeout"] = 60;
    auto begin = std::chrono::steady_clock::now();
    steps = call(
        "workflow_steps",
        {{"steps",
          JsonDoc::array({bounded, action("element_fill",
                                          {{"selector", "#person"},
                                           {"text", "After step deadline"}})})},
         {"max_step_retries", 1},
         {"auto_wait", false},
         {"step_timeout", 5000}});
    const auto elapsed = std::chrono::duration_cast<MsDuration>(
                             std::chrono::steady_clock::now() - begin)
                             .count();
    check(steps.at("steps")[0].at("attempts") == 2 && steps.at("failed") == 1 &&
              elapsed < 1500,
          "per-step override bounds every attempt below child timeout");
    check(eval("document.querySelector('#person').value") ==
              "After step deadline",
          "step deadline restored before following action");
    auto before = action("page_evaluate", {{"script", "order.push('A');'A'"}});
    before["wait_before"] = 20;
    before["wait_after"] = 30;
    steps = call(
        "workflow_steps",
        {{"steps", JsonDoc::array(
                       {before, action("page_evaluate",
                                       {{"script", "order.push('B');'B'"}})})},
         {"return_intermediate", true}});
    check(eval("order") == JsonDoc::array({"A", "B"}) &&
              steps.at("total_time_ms").get<int>() >= 40,
          "before/after waits retain real execution order");
    check(steps.at("steps")[1].at("result").at("result") == "B",
          "intermediate result keeps browser script output");
    steps =
        call("workflow_steps",
             {{"steps",
               JsonDoc::array({action("page_assert", {{"type", "visible"},
                                                      {"selector", "#never"},
                                                      {"timeout", 30}})})},
              {"return_intermediate", true}});
    check(steps.at("steps")[0].at("success") == true &&
              steps.at("steps")[0].at("attempts") == 1 &&
              steps.at("last_result").at("passed") == false,
          "nonthrowing assertion failure remains an execution success with "
          "explicit failed assertion result");
    check(steps.at("steps")[0].at("brief").get<std::string>().starts_with(
              "failed:"),
          "brief reports failed assertion rather than hiding it");
    auto blocked = action(
        "workflow_batch",
        {{"actions",
          JsonDoc::array({action("element_fill", {{"selector", "#person"},
                                                  {"text", "wrong"}})})}});
    blocked["optional"] = true;
    steps = call(
        "workflow_steps",
        {{"steps", JsonDoc::array({blocked, action("element_read",
                                                   {{"selector", "#person"},
                                                    {"type", "value"}})})}});
    check(steps.at("failed") == 1 &&
              steps.at("last_result").at("value") == "After step deadline",
          "nested workflow refusal prevents concealed child mutation");
    steps = call(
        "workflow_steps",
        {{"steps", JsonDoc::array({action("page_evaluate",
                                          {{"script", "'😀'.repeat(200)"}})})},
         {"return_intermediate", false}});
    check(!steps.at("steps")[0].contains("result") &&
              steps.at("last_result").at("result").get<std::string>().size() ==
                  800,
          "summary mode keeps complete last result beyond brief length");
    check(steps.at("steps")[0].at("brief").get<std::string>().size() == 600,
          "brief clipping keeps UTF-8 scalar boundaries");
    batch = call(
        "workflow_batch",
        {{"actions", JsonDoc::array({action("workflow_retry",
                                            {{"tool", "page_evaluate"},
                                             {"args", {{"script", "'nested'"}}},
                                             {"max_retries", 1}})})}});
    check(batch.at("results")[0].at("result").at("result").at("result") ==
              "nested",
          "batch and retry nest using validated native dispatcher");

    call("page_navigate", {{"url", std::string(site) + "/frame-host.html"}});
    call("page_wait", {{"type", "function"},
                       {"expression", "readyFrames.length>=6"},
                       {"timeout", 5000}});
    steps = call(
        "workflow_steps",
        {{"steps",
          JsonDoc::array(
              {action("frame_enter", {{"selector", "#crossFrame"}}),
               action("element_fill",
                      {{"selector", "#frame-input"}, {"text", "Workflow OOP"}}),
               action("element_click", {{"selector", "#frame-button"}}),
               action("element_read",
                      {{"selector", "#frame-clicks"}, {"type", "text"}})})},
         {"max_step_retries", 0},
         {"return_intermediate", true}});
    check(steps.at("failed") == 0 && steps.at("last_result").at("text") == "1",
          "step sequence enters and operates in actual OOP frame");
    check(eval("document.querySelector('#frame-input').value") ==
              "Workflow OOP",
          "workflow frame scope persists between children");
    check(steps.at("page").at("title") == "Frame host",
          "final page summary refers to root despite selected frame");
    call("frame_reset");
    check(eval("document.querySelector('#frame-input').value") == "ROOT",
          "workflow child input leaves parent untouched");
    const auto metrics = call("browser_metrics");
    check(metrics.at("byTool").at("retry").at("count").get<unsigned>() >= 10 &&
              metrics.at("byTool").at("eval").at("errors").get<unsigned>() >= 5,
          "runtime metrics include workflow parents and failed child attempts");
    call("tab_close");
    std::cout << passed << " live workflow checks passed; " << failed
              << " failed\n";
    return failed ? 1 : 0;
  } catch (const std::exception &error) {
    std::cerr << step << "\n" << error.what() << '\n';
    return 1;
  }
}
