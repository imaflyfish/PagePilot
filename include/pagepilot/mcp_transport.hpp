#pragma once
#include <functional>
#include <memory>
#include <optional>
#include <pagepilot/tool_catalog.hpp>
#include <string_view>
namespace pagepilot {
class StdioTransport {
public:
  using Handler = std::function<JsonDoc(const ToolInvocation &)>;
  StdioTransport(Handler handler, bool compatibility = false);
  std::optional<JsonDoc> receive(const JsonDoc &message);
  JsonDoc parse_failure() const;

private:
  ToolCatalog catalog_;
  Handler handler_;
  bool compatibility_ = false, initialized_ = false, ready_ = false;
  std::string version_;
};
class LineReader {
public:
  explicit LineReader(std::size_t maximum = 16 * 1024 * 1024)
      : maximum_(maximum) {}
  std::vector<std::string> feed(std::string_view bytes, bool end = false);

private:
  std::string pending_;
  std::size_t maximum_;
};
// One execution worker preserves browser operation order while the caller
// continues reading cancellation notifications. Sink calls are serialized.
class RequestPump {
public:
  using Sink = std::function<void(const JsonDoc &)>;
  RequestPump(StdioTransport::Handler handler, Sink sink,
              bool compatibility = false, std::size_t maximum_requests = 128,
              std::size_t maximum_bytes = 64 * 1024 * 1024);
  ~RequestPump();
  RequestPump(const RequestPump &) = delete;
  RequestPump &operator=(const RequestPump &) = delete;
  void submit(const std::string &line);
  void finish();
  void check_failure() const;

private:
  struct Engine;
  std::unique_ptr<Engine> engine_;
};
} // namespace pagepilot
