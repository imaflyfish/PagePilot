#include <iostream>
#include <pagepilot/mcp_transport.hpp>
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
int main() {
  try {
    unsigned calls = 0;
    StdioTransport endpoint([&](const ToolInvocation &value) {
      ++calls;
      return JsonDoc{{"operation", value.operation}};
    });
    auto request = [](std::string method, JsonDoc params = JsonDoc::object()) {
      return JsonDoc{{"jsonrpc", "2.0"},
                     {"id", 17},
                     {"method", method},
                     {"params", params}};
    };
    check(endpoint.receive(request("tools/list"))->at("error").at("code") ==
              -32002,
          "tools require completed initialization");
    check(endpoint.receive(request("initialize"))->at("error").at("code") ==
              -32602,
          "initialization parameters checked");
    const auto init =
        request("initialize",
                {{"protocolVersion", "2026-01-01"},
                 {"capabilities", JsonDoc::object()},
                 {"clientInfo", {{"name", "fixture"}, {"version", "1"}}}});
    for (const auto &client :
         std::vector<JsonDoc>{JsonDoc::object(),
                              {{"name", "fixture"}},
                              {{"name", 5}, {"version", "1"}},
                              {{"name", "fixture"}, {"version", 1}}}) {
      auto malformed = init;
      malformed["params"]["clientInfo"] = client;
      check(endpoint.receive(malformed)->at("error").at("code") == -32602,
            "malformed client information cannot initialize the session");
    }
    check(endpoint.receive(init)->at("result").at("protocolVersion") ==
              "2025-11-25",
          "unsupported version negotiates declared supported version");
    check(endpoint.receive(request("tools/list"))->contains("error"),
          "initialized notification is required");
    for (const auto &parameters :
         std::vector<JsonDoc>{nullptr, JsonDoc::array(), true, "invalid"}) {
      const auto ignored =
          endpoint.receive({{"jsonrpc", "2.0"},
                            {"method", "notifications/initialized"},
                            {"params", parameters}});
      check(
          !ignored &&
              endpoint.receive(request("tools/list"))->at("error").at("code") ==
                  -32002,
          "malformed notification is silent and cannot complete "
          "initialization");
    }
    check(!endpoint.receive(
              {{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}}),
          "notification never receives a response");
    check(endpoint.receive(request("tools/list"))
                  ->at("result")
                  .at("tools")
                  .size() == 54,
          "MCP lists canonical tools");
    check(endpoint.receive(init)->contains("error"),
          "repeated initialize rejected");
    auto reply = endpoint.receive(
        request("tools/call", {{"name", "page_navigate"},
                               {"arguments", {{"url", "about:blank"}}}}));
    check(reply->at("result").at("structuredContent").at("operation") ==
                  "navigate" &&
              calls == 1,
          "validated action reaches handler");
    reply =
        endpoint.receive(request("tools/call", {{"name", "page_navigate"}}));
    check(reply->at("result").at("isError") == true && calls == 1,
          "invalid action arguments cannot reach browser");
    reply = endpoint.receive(request("tools/call", {{"name", "__proto__"}}));
    check(reply->at("error").at("code") == -32602 && calls == 1,
          "unknown tool is a protocol error and cannot reach handler");
    for (const auto &arguments :
         std::vector<JsonDoc>{nullptr, JsonDoc::array(), "invalid", 5, true}) {
      reply =
          endpoint.receive(request("tools/call", {{"name", "browser_settings"},
                                                  {"arguments", arguments}}));
      check(reply->at("error").at("code") == -32602 && calls == 1,
            "non-object tool arguments cannot reach handler");
    }
    reply = endpoint.receive(request("tools/call", {{"name", "get_url"}}));
    check(reply->at("error").at("code") == -32602 && calls == 1,
          "legacy-only name is unknown outside compatibility mode");
    reply =
        endpoint.receive(request("tools/call", {{"name", "page_navigate"},
                                                {"arguments", {{"url", 5}}}}));
    check(reply->at("result").at("isError") == true && calls == 1,
          "known tool input value errors remain tool execution errors");
    check(!endpoint.receive({{"jsonrpc", "2.0"},
                             {"method", "tools/call"},
                             {"params", {{"name", "browser_settings"}}}}) &&
              calls == 1,
          "tool-call notifications never execute a handler or receive a reply");
    reply =
        endpoint.receive(request("tools/call", {{"name", "browser_settings"}}));
    check(!reply->at("result").at("isError").get<bool>() && calls == 2,
          "omitted arguments remain an empty object after malformed requests");
    check(endpoint.receive(request("missing"))->at("error").at("code") ==
              -32601,
          "unknown request error");
    check(!endpoint.receive({{"jsonrpc", "2.0"}, {"method", "missing"}}),
          "unknown notification ignored without response");
    check(endpoint.receive(JsonDoc::array())->at("error").at("code") == -32600,
          "JSON-RPC batch arrays refused");
    auto bad = request("ping");
    bad["id"] = true;
    check(endpoint.receive(bad)->at("error").at("code") == -32600,
          "boolean request ID refused");
    bad["id"] = nullptr;
    check(
        endpoint.receive(bad)->at("error").at("code") == -32600,
        "null request ID cannot create an unaddressable cancellable operation");
    bad = request("ping");
    bad["id"] = "request-中文";
    check(endpoint.receive(bad)->at("id") == "request-中文",
          "string request ID preserved");
    check(endpoint.parse_failure().at("error").at("code") == -32700,
          "parse failure code");
    LineReader lines(64);
    check(lines.feed("{\"id\":").empty(), "partial frame buffered");
    auto decoded = lines.feed("1}\n{\"id\":2}\r\n \nlast", true);
    check(decoded.size() == 3 && decoded[0] == "{\"id\":1}" &&
              decoded[1] == "{\"id\":2}" && decoded[2] == "last",
          "chunked framing CRLF and trailing EOF frame");
    bool refused = false;
    try {
      lines.feed(std::string(65, 'x'));
    } catch (const BridgeError &) {
      refused = true;
    }
    check(refused, "unterminated line is bounded");
    StdioTransport legacy(
        [](const ToolInvocation &) { return JsonDoc{{"ok", true}}; }, true);
    auto older = init;
    older["params"]["protocolVersion"] = "2024-11-05";
    check(legacy.receive(older)->at("result").at("protocolVersion") ==
              "2024-11-05",
          "baseline protocol remains supported");
    legacy.receive(
        {{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}});
    check(legacy.receive(request("tools/list"))
                  ->at("result")
                  .at("tools")
                  .size() == 75,
          "compatibility MCP advertises all old names");
    reply = legacy.receive(request("tools/call", {{"name", "get_url"}}));
    check(!reply->at("result").contains("structuredContent") &&
              !reply->at("result").at("isError").get<bool>(),
          "legacy response contains supported content form");
    check(legacy.receive(request("tools/call", {{"name", "missing"}}))
                  ->at("error")
                  .at("code") == -32602,
          "older protocol also reports an unknown tool as a protocol error");
    check(legacy.receive(request("tools/call",
                                 {{"name", "get_url"}, {"arguments", nullptr}}))
                  ->at("error")
                  .at("code") == -32602,
          "older protocol also rejects non-object tool arguments");
    check(!legacy.receive(request("tools/call", {{"name", "page_read"}}))
               ->at("result")
               .at("isError")
               .get<bool>(),
          "canonical names remain callable in compatibility mode");
    std::cout << passed << " MCP checks passed; " << failed << " failed\n";
    return failed ? 1 : 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
