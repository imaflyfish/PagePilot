#include <array>
#include <charconv>
#include <pagepilot/mcp_transport.hpp>
#include <pagepilot/tool_runtime.hpp>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <poll.h>
#include <unistd.h>
int main(int argc, char **argv) {
  using namespace pagepilot;
  std::signal(SIGPIPE, SIG_IGN);
  try {
    ToolCatalog catalog;
    unsigned port = 9222;
    std::optional<std::string> requested_port;
    bool compatibility = false;
    std::vector<std::filesystem::path> file_roots;
    auto parse_port = [&](const std::string &value) {
      unsigned result = 0;
      const auto parsed =
          std::from_chars(value.data(), value.data() + value.size(), result);
      if (parsed.ec != std::errc() ||
          parsed.ptr != value.data() + value.size() || !result ||
          result > 65535)
        throw BridgeError("invalid DevTools port");
      port = result;
    };
    for (int i = 1; i < argc; ++i) {
      const std::string option = argv[i];
      if (option == "--version") {
        std::cout << "PagePilot 1.0.0\n";
        return 0;
      }
      if (option == "--catalog" || option == "--compat-catalog") {
        std::cout << catalog.list(option == "--compat-catalog").dump(2) << '\n';
        return 0;
      }
      if (option == "--compat-tools")
        compatibility = true;
      else if (option == "--port" && i + 1 < argc)
        requested_port = argv[++i];
      else if (option == "--allow-root" && i + 1 < argc)
        file_roots.emplace_back(argv[++i]);
      else if (option == "--help") {
        std::cout
            << "page-pilot [--port PORT] [--compat-tools] [--allow-root DIR "
               "...]\nMCP stdio server; "
               "connects to existing loopback Chrome.\n"
               "See the installed README and compatibility guide for setup.\n";
        return 0;
      } else
        throw BridgeError("unknown or incomplete option: " + option);
    }
    // Validate the selected value only: an overridden environment variable
    // must not prevent an explicit CLI value or the preferred variable.
    if (!requested_port) {
      if (const auto *environment = std::getenv("PAGEPILOT_PORT"))
        requested_port = environment;
      else if (const auto *environment = std::getenv("CDP_PORT"))
        requested_port = environment;
    }
    if (requested_port)
      parse_port(*requested_port);
    ToolRuntime runtime(port, file_roots.empty() ? PathGuard::defaults()
                                                   : file_roots);
    RequestPump requests(
        [&](const ToolInvocation &invocation) {
          return runtime.invoke(invocation);
        },
        [](const JsonDoc &reply) {
          std::cout << reply.dump() << '\n' << std::flush;
          if (!std::cout)
            throw BridgeError("cannot write MCP stdout");
        },
        compatibility);
    LineReader framing;
    std::array<char, 8192> buffer{};
    while (true) {
      requests.check_failure();
      pollfd input{STDIN_FILENO, POLLIN, 0};
      const auto ready = ::poll(&input, 1, 50);
      if (ready < 0) {
        if (errno == EINTR)
          continue;
        throw BridgeError("cannot poll MCP stdin");
      }
      if (!ready)
        continue;
      const auto count = ::read(STDIN_FILENO, buffer.data(), buffer.size());
      if (count < 0) {
        if (errno == EINTR)
          continue;
        throw BridgeError("cannot read MCP stdin");
      }
      const auto lines = framing.feed(
          std::string_view(buffer.data(), static_cast<std::size_t>(count)),
          count == 0);
      for (const auto &line : lines) {
        requests.submit(line);
      }
      if (count == 0)
        break;
    }
    requests.finish();
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "page-pilot: " << error.what() << '\n';
    return 2;
  }
}
