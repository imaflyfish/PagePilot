#pragma once
#include <chrono>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <vector>
namespace pagepilot {
using JsonDoc = nlohmann::json;
using MsDuration = std::chrono::milliseconds;
class RequestAborted : public std::runtime_error {
public:
  RequestAborted() : std::runtime_error("Request was cancelled") {}
};
// Request-local cooperative cancellation. An empty token deliberately masks
// cancellation during bounded cleanup; nested scopes restore the prior token.
class CancelScope {
public:
  explicit CancelScope(std::stop_token token = {});
  ~CancelScope();
  CancelScope(const CancelScope &) = delete;
  CancelScope &operator=(const CancelScope &) = delete;

private:
  std::stop_token previous_;
};
void cancellation_point();
void interruptible_pause(MsDuration duration);
class BridgeError : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};
class DeadlineReached : public BridgeError {
public:
  using BridgeError::BridgeError;
};
class ActionLimitReached : public BridgeError {
public:
  using BridgeError::BridgeError;
};
class WireFailure : public BridgeError {
public:
  explicit WireFailure(const JsonDoc &detail)
      : BridgeError("DevTools: " + detail.dump()), code(detail.value("code", 0)),
        description(detail.value("message", std::string())) {}
  int code;
  std::string description;
};
JsonDoc parse_message(const std::string &text,
                   std::size_t limit = 16 * 1024 * 1024);
JsonDoc normalize_arguments(const JsonDoc &arguments, const JsonDoc &schema);
std::string read_text(const std::filesystem::path &path,
                      std::size_t maximum = 16 * 1024 * 1024);
} // namespace pagepilot
