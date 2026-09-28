#pragma once
#include <pagepilot/core.hpp>
#include <functional>
#include <memory>
namespace pagepilot {
JsonDoc discover_browser(unsigned port, MsDuration timeout = MsDuration(5000));
JsonDoc decode_cdp_message(const std::string &text);
class CdpChannel {
public:
  explicit CdpChannel(unsigned port,
                           MsDuration timeout = MsDuration(5000));
  ~CdpChannel();
  CdpChannel(const CdpChannel &) = delete;
  CdpChannel &operator=(const CdpChannel &) = delete;
  JsonDoc call(const std::string &method, const JsonDoc &parameters = JsonDoc::object(),
            const std::string &session = {},
            MsDuration timeout = MsDuration(10000),
            std::function<void()> progress = {});
  std::vector<JsonDoc> drain_events();
  bool connected() const;
  void disconnect();

private:
  struct Engine;
  std::unique_ptr<Engine> engine_;
};
} // namespace pagepilot
