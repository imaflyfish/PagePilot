# Build, install and connect

PagePilot is a native MCP stdio server that attaches to an existing Chrome
DevTools endpoint on loopback. It does not start or close your browser. The
tested platform is macOS arm64 with Chrome 152 and 153. Linux and Windows have not been
built or run; Windows also needs a port of the current POSIX file/stdin code.

## From source

Use a C++20 compiler and standard library/SDK that provide `std::stop_source`
and `std::stop_token`, CMake 3.24 or later, Ninja, Boost headers 1.85 or later
and nlohmann/json 3.12 or later. CMake checks actual cancellation-library support;
the C++20 language flag alone is insufficient. Local validation used Xcode 27.0,
and hosted CI selects Xcode 26.6. These are tested toolchains, not minimum-version
claims. Select the matching compiler and SDK before configuring a fresh build.
Dependency versions are recorded in `dependencies.lock.json`. From this source
checkout, on the tested Homebrew environment:

```sh
brew install cmake ninja boost nlohmann-json
cmake --preset release
cmake --build --preset release
ctest --preset release --verbose
cmake --install build/release --prefix "$PWD/dist/PagePilot-macos-arm64"
dist/PagePilot-macos-arm64/bin/page-pilot --version
```

The installation includes the executable, static library, public headers,
relocatable CMake package, consumer example, documents and licenses. DOM and key
mapping resources are embedded; they need no runtime installation. Node,
Playwright and Python are not production dependencies. Python and Pillow are
used only by the integration tests. The macOS executable uses system libraries;
the headers/library consumer additionally needs the C++ dependencies above.

## Browser and MCP host

Start a separate browser profile with remote debugging, or use a browser you
already started with these flags:

```sh
"/Applications/Google Chrome.app/Contents/MacOS/Google Chrome" \
  --remote-debugging-address=127.0.0.1 --remote-debugging-port=9222 \
  --user-data-dir="$HOME/Library/Application Support/PagePilotProfile"
```

Reusing this directory retains that profile's browser state. It is separate from
your normal Chrome profile; PagePilot does not copy accounts or sessions into
it. If you use an existing debug-enabled browser, inspect `tab_list` and explicitly
select the intended tab with `tab_activate` before changing a page. The selected
page is not a promise to follow the operating system's foreground tab. The
recorded tests use disposable profiles and local pages, not personal accounts.

Configure an MCP host to launch the **absolute** installed executable path with
arguments `--port 9222`. A host using the common `mcpServers` configuration shape
can use the following, replacing both placeholder paths:

```json
{
  "mcpServers": {
    "page-pilot": {
      "command": "/absolute/path/PagePilot-macos-arm64/bin/page-pilot",
      "args": ["--port", "9222", "--allow-root", "/absolute/path/browser-files"]
    }
  }
}
```

Create the directory supplied to `--allow-root` before launching the server.
`--allow-root` may repeat and replaces the default home/temp roots. These roots
control uploads and saved screenshots, not arbitrary page scripts. Only trusted
local clients should access the browser's debugging port. Standard output is
reserved for newline-delimited MCP JSON; diagnostic errors use standard error.
The host initializes MCP, lists tools and calls them over the child's stdin.

Default tool names include `page_navigate`, `element_click`, `element_type` and
`page_capture`. Add `--compat-tools` to advertise all 75 old names. Inspect the
schemas without connecting to Chrome using `--catalog` or `--compat-catalog`.
`PAGEPILOT_PORT` takes priority over legacy `CDP_PORT`; `--port` takes priority
over environment values. Only the selected value is validated; an invalid value
in an overridden variable cannot block it. An empty selected value is invalid.
`--help`, `--version` and either catalog flag work independently of port variables.
Supported MCP lifecycle versions are 2024-11-05 and
2025-11-25. See [COMPATIBILITY.md](COMPATIBILITY.md) before migrating callers.

[The local workflow example](LOCAL_WORKFLOW.md) shows an actual page, tool
arguments and expected result. The `human` input mode uses native pacing; it
does not reproduce the old random Bézier paths, typo injection or timing
distribution. Its name is retained for caller compatibility.

## Library consumer and tests

```sh
cmake -S examples/consumer -B build/consumer \
  -DCMAKE_PREFIX_PATH="$PWD/dist/PagePilot-macos-arm64"
cmake --build build/consumer
build/consumer/pilot-consumer
```

The example links `PagePilot::pagepilot` through `find_package(PagePilot)`.
With no port, the command above checks the installed catalog only. To exercise
its seven live browser checks in an owned temporary profile:

```sh
python3 tests/integration/with_chrome.py --evidence build/consumer-evidence \
  build/consumer/pilot-consumer
```

For the full integration suite, first prepare Python's image-decoding dependency:

```sh
python3 -m venv build/browser-python
build/browser-python/bin/python -m pip install Pillow
PATH="$PWD/build/browser-python/bin:$PATH" \
  build/browser-python/bin/python tests/integration/run_suite.py --build build/release \
  --evidence /absolute/path/test-evidence/release
```

The harness creates its own temporary headless Chrome profile and serves only
local fixtures. It runs browser/MCP checks and bounded socket fault scenarios,
records each exit and cleans up its own browser. The default Chrome path is the
macOS application shown above. Both `run_suite.py` and `with_chrome.py` accept
`--chrome /absolute/path/to/Chrome` for another installed Chrome build. A full
count combines the `ctest` contracts with the browser/MCP and fault checks from
`run_suite.py`; the consumer and CLI checks are additional. A successful build or
a catalog-only consumer alone does not validate browser behavior.
