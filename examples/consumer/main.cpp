#include <iostream>
#include <pagepilot/tool_runtime.hpp>
using namespace pagepilot;
int main(int argc, char **argv) {
  try {
    ToolCatalog catalog;
    if (catalog.list(false).size() != 54 || catalog.list(true).size() != 75)
      throw BridgeError("catalog mismatch");
    if (argc == 1) {
      std::cout << "Installed catalog: 54 canonical / 75 compatibility names\n";
      return 0;
    }
    if (argc != 2)
      return 2;
    ToolRuntime runtime(static_cast<unsigned>(std::stoul(argv[1])));
    auto call = [&](const std::string &name, JsonDoc args = JsonDoc::object()) {
      return runtime.invoke(catalog.resolve(name, args));
    };
    unsigned checks = 2;
    auto check = [&](bool okay) {
      if (!okay)
        throw BridgeError("installed consumer behavior mismatch");
      ++checks;
    };
    check(call("tab_create").contains("target"));
    check(call("page_evaluate",
               {{"script",
                 "document.body.innerHTML='<button id=go>Go</button><input "
                 "id=field>';window.presses=0;document.querySelector('#go')."
                 "onclick=()=>presses++;true"}})
              .at("result") == true);
    check(call("element_click", {{"selector", "#go"}}).at("clicked") == true);
    call("element_fill",
         {{"selector", "#field"}, {"text", "Installed consumer"}});
    check(call("page_evaluate",
               {{"script", "presses===1&&document.querySelector('#field')."
                           "value==='Installed consumer'"}})
              .at("result") == true);
    check(call("tab_close").at("closed") == true);
    std::cout << checks << " installed consumer checks passed; 0 failed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
