# PagePilot

**PagePilot 1.0.0** is a C++20 MCP-to-Chrome DevTools bridge. It connects to an
existing loopback Chrome and provides 54 canonical browser actions or 75 legacy
compatibility names. Native transport, sessions, navigation, inputs, files,
workflows and cancellation run without a Node or Playwright runtime. Small
embedded JavaScript adapters handle DOM access and browser-side value conversion.

Start with [GETTING_STARTED.md](docs/GETTING_STARTED.md) for installation, a
separate Chrome profile, MCP host configuration and the C++ library example.
For a concrete first task, [fill a local form, click once and read its result](docs/LOCAL_WORKFLOW.md).
The example uses the included page and shows the expected MCP result.

| Task | Canonical tools |
|---|---|
| Open and inspect a page | `tab_create`, `page_navigate`, `page_read`, `page_snapshot` |
| Fill and check a form | `element_fill`, `element_click`, `element_read`, `page_assert` |
| Work within a frame | `frame_list`, `frame_enter`, `frame_leave` |
| Capture or compose actions | `page_capture`, `workflow_batch`, `workflow_steps` |

An intermittent transformed-frame hover timeout in cross-process frames remains
an open investigation; a passing run does not establish its cause.

```sh
cmake --preset release
cmake --build --preset release
ctest --preset release --verbose
cmake --install build/release --prefix "$PWD/dist/PagePilot-macos-arm64"
dist/PagePilot-macos-arm64/bin/page-pilot --port 9222
```

Build dependencies: a C++20 compiler **and a standard library/SDK implementing
`std::stop_source` and `std::stop_token`**, CMake, Ninja, Boost headers and
nlohmann/json. CMake compiles and links a cancellation probe before building;
accepting `-std=c++20` alone is insufficient. If the probe fails, upgrade the
compiler together with its standard library/SDK and configure a fresh build
directory. On macOS, select a newer Xcode or compatible LLVM installation;
changing the compiler executable alone may still leave an older SDK in use.
Local validation used Xcode 27.0; the macOS CI configuration selects Xcode 26.6.
These identify the local and CI toolchains, not a claimed minimum Xcode version.
Validated dependency versions are in `dependencies.lock.json`. The macOS executable uses
system dynamic libraries. Build and install from this source tree. A [validation summary](validation/README.md)
records the locally accepted version and its verification boundaries.

Default names describe their domain: `page_navigate`, `element_click`,
`element_type`, `frame_enter`, `page_capture`, `workflow_steps`. Use `--catalog`
to inspect schemas or `--compat-tools` to advertise the original 75 names. The
server exits by disconnecting; it does not close your browser. Repeated
`--allow-root DIR` flags replace default home/temp upload and screenshot roots.

- [COMPATIBILITY.md](docs/COMPATIBILITY.md): all names, legacy result formats,
  preserved contracts and intentional differences.
- [CLICKS.md](docs/CLICKS.md), [KEYBOARD.md](docs/KEYBOARD.md),
  [FRAMES.md](docs/FRAMES.md): native input, targeting and frame scope.
- [NAVIGATION.md](docs/NAVIGATION.md), [PAGE_SERVICES.md](docs/PAGE_SERVICES.md):
  navigation, recovery, dialogs, cookies, storage and accessibility.
- [FILES_AND_CAPTURE.md](docs/FILES_AND_CAPTURE.md),
  [WORKFLOWS.md](docs/WORKFLOWS.md), [CANCELLATION.md](docs/CANCELLATION.md):
  file limits, composition, deadlines and side effects.
- [validation/README.md](validation/README.md): how to run the suites and what
  they do and do not establish.

The test suites cover contract checks that need no browser, owned-Chrome and MCP
checks in a throwaway profile, and bounded socket-fault scenarios against a local
fixture server. Port-value precedence and MCP request-shape validation are part of
the contract: a protocol error and a tool execution error are reported distinctly.
Closing a page during an action stops remaining typing; directory uploads wait for
their own completion event without repeating the selection. A protocol fuzz target
is available in the sanitizer build.

Exercised on macOS arm64 with Chrome 152 and 153. Linux/Windows have not been run;
Windows needs adaptation of POSIX file/stdin handling. Arbitrary page layouts,
extended Playwright selectors and every possible parameter combination are not
claimed equivalent. Drag is within one selected document; snapshots explicitly
use native `ax-yaml`. See the linked guides before migrating a caller.

PagePilot connects to a Chrome debug port and can run page script and read or
change cookies and storage, so point it only at a browser you own. Pinned probe
vectors supply the 29 evaluation, 30 keyboard, 15
input-target, 28 click and 16 CSS vectors pinned in `tests/fixtures/`.

Implementation copyright 2026 imaflyfish, MIT. Upstream attribution and
all dependency license texts are retained in `LICENSE`, `THIRD_PARTY_NOTICES.md`
and `licenses/`. Embedded Playwright keyboard mapping data retains Apache-2.0
attribution; it is not newly authored mapping data or a runtime dependency.

How to run the suites and what they cover: [validation/README.md](validation/README.md).
