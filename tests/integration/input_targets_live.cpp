#include <pagepilot/tool_runtime.hpp>
#include <cstdlib>
#include <iostream>
using namespace pagepilot;
namespace {
unsigned checks = 0;
void check(bool value, const std::string &label) {
  if (!value) throw BridgeError(label);
  ++checks;
}
} // namespace
int main(int argc, char **argv) {
  if (argc != 2) return 2;
  try {
    const auto *fixture = std::getenv("PAGEPILOT_FIXTURE_URL");
    if (!fixture) throw BridgeError("missing owned fixture URL");
    const std::string site = fixture;
    ToolRuntime runtime(static_cast<unsigned>(std::stoul(argv[1])));
    ToolCatalog catalog;
    auto call = [&](const std::string &name, JsonDoc arguments = JsonDoc::object()) {
      return runtime.invoke(catalog.resolve(name, arguments, true));
    };
    auto eval = [&](const std::string &expression) {
      return call("page_evaluate", {{"script", expression}}).at("result");
    };
    call("tab_create");
    call("page_navigate", {{"url", site + "/input-targets.html"}});
    const auto vectors = eval("fetch(" + JsonDoc(site + "/input-target-values.json").dump() +
                              ").then(r=>r.json())").at("vectors");
    check(vectors.size() == 15, "fifteen actual baseline target-selection vectors");
    for (const auto &mode : {"fast", "slow", "human"}) {
      for (const auto &vector : vectors) {
        eval("document.querySelectorAll('input,textarea').forEach(e=>e.value='');true");
        auto arguments = vector.at("arguments");
        arguments.update({{"text", "owned"}, {"mode", mode}, {"delay", 0}, {"timeout", 1000}});
        const auto result = call(std::string(mode) == "fast" ? "element_type" : "type", arguments);
        auto expected = vector.at("result");
        if (expected.contains("mode")) expected["mode"] = mode;
        check(result == expected, "target-selection result fields: " + arguments.dump());
        const auto values = eval("Object.fromEntries([...document.querySelectorAll('input,textarea')].map(e=>[e.id,e.value]))");
        check(values == vector.at("values"), "target precedence and document order: " + arguments.dump() + " actual=" + values.dump());
      }
    }
    auto cross = site;
    cross.replace(cross.find("127.0.0.1"), 9, "localhost");
    for (const auto &url : {site + "/input-targets.html", cross + "/input-targets.html"}) {
      eval("document.querySelectorAll('input,textarea').forEach(e=>e.value='');"
           "document.querySelector('iframe')?.remove();"
           "new Promise(resolve=>{const f=document.createElement('iframe');f.id='owned-frame';"
           "f.style='width:600px;height:500px';f.onload=()=>resolve(true);f.src=" +
           JsonDoc(url).dump() + ";document.body.append(f)})");
      call("frame_enter", {{"selector", "#owned-frame"}});
      auto result = call("element_type", {{"selector", "#third"}, {"index", 1}, {"text", "frame-index"}});
      check(result.at("inFrame") == true && result.at("currentValue") == "frame-index",
            "visible-input index is scoped to the selected frame");
      check(eval("document.querySelector('#middle').value") == "frame-index" &&
                eval("document.querySelector('#third').value") == "",
            "mixed input/textarea order is preserved inside same/cross-origin frames");
      call("frame_reset");
      check(eval("[...document.querySelectorAll('input,textarea')].every(e=>e.value==='')") == true,
            "selected-frame input never falls back to the parent controls");
    }
    call("tab_close");
    std::cout << checks << " input-target checks passed; 0 failed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
