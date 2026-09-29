#include <limits>
#include <pagepilot/cdp_channel.hpp>
namespace pagepilot {
JsonDoc decode_cdp_message(const std::string &text) {
  auto message = parse_message(text, 32 * 1024 * 1024);
  auto reject = [] { throw BridgeError("Invalid DevTools message envelope"); };
  if (!message.is_object())
    reject();
  if (message.contains("sessionId") &&
      (!message.at("sessionId").is_string() || message.at("sessionId").empty()))
    reject();
  if (message.contains("id")) {
    const auto &identity = message.at("id");
    if (!identity.is_number_integer())
      reject();
    constexpr std::uint64_t maximum = 9007199254740991ULL;
    if (identity.is_number_unsigned()) {
      if (!identity.get<std::uint64_t>() ||
          identity.get<std::uint64_t>() > maximum)
        reject();
    } else if (identity.get<std::int64_t>() < 1 ||
               identity.get<std::int64_t>() >
                   static_cast<std::int64_t>(maximum)) {
      reject();
    }
    if (message.contains("method") || message.contains("params") ||
        message.contains("result") == message.contains("error"))
      reject();
    if (message.contains("result")) {
      if (!message.at("result").is_object())
        reject();
    } else {
      const auto &error = message.at("error");
      if (!error.is_object() || !error.contains("code") ||
          !error.at("code").is_number_integer() || !error.contains("message") ||
          !error.at("message").is_string())
        reject();
      const auto &code = error.at("code");
      if (code.is_number_unsigned()) {
        if (code.get<std::uint64_t>() >
            static_cast<std::uint64_t>(std::numeric_limits<int>::max()))
          reject();
      } else if (code.get<std::int64_t>() < std::numeric_limits<int>::min() ||
                 code.get<std::int64_t>() > std::numeric_limits<int>::max()) {
        reject();
      }
    }
  } else {
    if (!message.contains("method") || !message.at("method").is_string() ||
        message.at("method").empty() || message.contains("result") ||
        message.contains("error") ||
        (message.contains("params") && !message.at("params").is_object()))
      reject();
  }
  return message;
}
} // namespace pagepilot
