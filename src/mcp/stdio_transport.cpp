#include <pagepilot/mcp_transport.hpp>
namespace pagepilot {
namespace {
JsonDoc error(const JsonDoc &id, int code, const std::string &message) {
  return {{"jsonrpc", "2.0"},
          {"id", id},
          {"error", {{"code", code}, {"message", message}}}};
}
} // namespace
StdioTransport::StdioTransport(Handler handler, bool compatibility)
    : handler_(std::move(handler)), compatibility_(compatibility) {}
JsonDoc StdioTransport::parse_failure() const {
  return error(nullptr, -32700, "Invalid JSON message");
}
std::optional<JsonDoc> StdioTransport::receive(const JsonDoc &message) {
  if (!message.is_object())
    return error(nullptr, -32600, "Expected a JSON-RPC object");
  const auto id = message.value("id", JsonDoc(nullptr));
  const bool request = message.contains("id");
  if (message.value("jsonrpc", JsonDoc()) != "2.0" ||
      !message.contains("method") || !message.at("method").is_string() ||
      (request && !id.is_string() && !id.is_number_integer() &&
       !id.is_number_unsigned()))
    return error(nullptr, -32600, "Invalid JSON-RPC request");
  const auto method = message.at("method").get<std::string>();
  const auto parameters = message.value("params", JsonDoc::object());
  if (!request) {
    if (method == "notifications/initialized" && initialized_ &&
        parameters.is_object())
      ready_ = true;
    return {};
  }
  auto response = [&](JsonDoc result) {
    return JsonDoc{{"jsonrpc", "2.0"}, {"id", id}, {"result", std::move(result)}};
  };
  try {
    if (!parameters.is_object())
      return error(id, -32602, "params must be an object");
    if (method == "initialize") {
      if (initialized_)
        return error(id, -32600, "Session is already initialized");
      if (!parameters.contains("protocolVersion") ||
          !parameters.at("protocolVersion").is_string() ||
          !parameters.contains("capabilities") ||
          !parameters.at("capabilities").is_object() ||
          !parameters.contains("clientInfo") ||
          !parameters.at("clientInfo").is_object() ||
          !parameters.at("clientInfo").contains("name") ||
          !parameters.at("clientInfo").at("name").is_string() ||
          !parameters.at("clientInfo").contains("version") ||
          !parameters.at("clientInfo").at("version").is_string())
        return error(
            id, -32602,
            "initialize requires protocolVersion, capabilities and clientInfo "
            "with string name and version");
      const auto requested =
          parameters.at("protocolVersion").get<std::string>();
      version_ = requested == "2024-11-05" ? requested : "2025-11-25";
      initialized_ = true;
      return response({{"protocolVersion", version_},
                       {"capabilities", {{"tools", JsonDoc::object()}}},
                       {"serverInfo",
                        {{"name", "page-pilot"}, {"version", "1.0.0"}}}});
    }
    if (method == "ping")
      return response(JsonDoc::object());
    if (!ready_)
      return error(id, -32002, "MCP initialization is not complete");
    if (method == "tools/list") {
      if (parameters.contains("cursor"))
        return error(id, -32602, "Tool listing has no pagination cursor");
      return response({{"tools", catalog_.list(compatibility_)}});
    }
    if (method == "tools/call") {
      if (!parameters.contains("name") || !parameters.at("name").is_string())
        return error(id, -32602, "tools/call requires a tool name");
      if (parameters.contains("arguments") &&
          !parameters.at("arguments").is_object())
        return error(id, -32602, "tools/call arguments must be an object");
      const auto name = parameters.at("name").get<std::string>();
      if (!catalog_.contains(name, compatibility_))
        return error(id, -32602, "Unknown tool: " + name);
      try {
        auto invocation = catalog_.resolve(
            name, parameters.value("arguments", JsonDoc::object()), compatibility_);
        auto value = handler_(invocation);
        JsonDoc result = {{"content", JsonDoc::array({{{"type", "text"},
                                                 {"text", value.dump()}}})},
                       {"isError", false}};
        if (version_ != "2024-11-05" && value.is_object())
          result["structuredContent"] = std::move(value);
        return response(std::move(result));
      } catch (const RequestAborted &) {
        throw;
      } catch (const std::exception &failure) {
        return response(
            {{"content", JsonDoc::array({{{"type", "text"},
                                       {"text", std::string("Error: ") +
                                                    failure.what()}}})},
             {"isError", true}});
      }
    }
    return error(id, -32601, "Method not found");
  } catch (const RequestAborted &) {
    throw;
  } catch (const std::exception &failure) {
    return error(id, -32602, failure.what());
  }
}
std::vector<std::string> LineReader::feed(std::string_view bytes, bool end) {
  std::vector<std::string> messages;
  std::size_t start = 0;
  while (start < bytes.size()) {
    const auto position = bytes.find('\n', start);
    const auto length = position == std::string_view::npos
                            ? bytes.size() - start
                            : position - start;
    if (length > maximum_ || pending_.size() > maximum_ - length)
      throw BridgeError("MCP line exceeds size limit");
    pending_.append(bytes.substr(start, length));
    if (position == std::string_view::npos)
      break;
    if (!pending_.empty() && pending_.back() == '\r')
      pending_.pop_back();
    if (pending_.find_first_not_of(" \t\r") != std::string::npos)
      messages.push_back(std::move(pending_));
    pending_.clear();
    start = position + 1;
  }
  if (end && !pending_.empty()) {
    if (pending_.find_first_not_of(" \t\r") != std::string::npos)
      messages.push_back(std::move(pending_));
    pending_.clear();
  }
  return messages;
}
} // namespace pagepilot
