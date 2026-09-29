#include <cstdint>
#include <cstdlib>
#include <pagepilot/cdp_channel.hpp>
#include <pagepilot/mcp_transport.hpp>
using namespace pagepilot;
namespace {
void invariant(bool value) {
  if (!value)
    std::abort();
}
std::pair<bool, std::vector<std::string>> lines(const std::string &text,
                                                std::size_t chunk) {
  LineReader framing(4096);
  std::vector<std::string> result;
  try {
    for (std::size_t at = 0; at < text.size(); at += chunk) {
      auto batch = framing.feed(std::string_view(text).substr(at, chunk));
      result.insert(result.end(), batch.begin(), batch.end());
    }
    auto final = framing.feed({}, true);
    result.insert(result.end(), final.begin(), final.end());
    return {true, result};
  } catch (const BridgeError &) {
    return {false, {}};
  }
}
} // namespace
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data,
                                      std::size_t size) {
  if (!size || size > 65536)
    return 0;
  const auto variant = data[0];
  const std::string text(reinterpret_cast<const char *>(data + 1), size - 1);
  if ((variant & 3) == 2) {
    invariant(lines(text, std::max<std::size_t>(1, text.size())) ==
              lines(text, 1 + (variant >> 2)));
    return 0;
  }
  JsonDoc parsed;
  try {
    parsed = parse_message(text, 65536);
  } catch (const BridgeError &) {
    return 0;
  }
  if ((variant & 3) == 0) {
    try {
      const auto decoded = decode_cdp_message(text);
      invariant(decode_cdp_message(decoded.dump()) == decoded);
    } catch (const BridgeError &) {
    }
  } else if ((variant & 3) == 1) {
    static const ToolCatalog catalog;
    if (!parsed.is_object() || !parsed.contains("name") ||
        !parsed.at("name").is_string())
      return 0;
    try {
      const auto action = catalog.resolve(
          parsed.at("name"), parsed.value("args", JsonDoc::object()), true);
      invariant(!action.operation.empty() && action.arguments.is_object());
    } catch (const BridgeError &) {
    }
  } else {
    StdioTransport endpoint(
        [](const ToolInvocation &action) {
          return JsonDoc{{"operation", action.operation},
                         {"arguments", action.arguments}};
        },
        (variant & 4) != 0);
    if (variant & 8) {
      (void)endpoint.receive({{"jsonrpc", "2.0"},
                              {"id", "init"},
                              {"method", "initialize"},
                              {"params",
                               {{"protocolVersion", "2025-11-25"},
                                {"capabilities", JsonDoc::object()},
                                {"clientInfo", JsonDoc::object()}}}});
      (void)endpoint.receive(
          {{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}});
    }
    const auto batch = parsed.is_array() ? parsed : JsonDoc::array({parsed});
    if (batch.size() > 32)
      return 0;
    for (const auto &message : batch) {
      const auto response = endpoint.receive(message);
      if (response) {
        invariant(response->is_object() && response->at("jsonrpc") == "2.0" &&
                  response->contains("id") &&
                  response->contains("error") != response->contains("result"));
        invariant(parse_message(response->dump()) == *response);
      }
    }
  }
  return 0;
}
