# Sources and dependencies

New C++ implementation: copyright 2026 imaflyfish, MIT licensed.
The 75 compatibility tool names and their parameter shapes exist so that callers
written against an older JavaScript browser-automation server keep working. They
are an interoperability surface, not the recommended API; the MIT notice covering
that carried-over material is preserved in `licenses/legacy-compat-MIT.txt`.
Browser-executed DOM expressions will remain minimal protocol adapters where
page-side JavaScript is necessary; they will not host the main workflow logic.

Dependencies used by the current native build:

| Dependency | Local version | License |
|---|---|---|
| Boost.Beast / Asio headers | 1.92.0 | BSL-1.0 |
| nlohmann/json | 3.12.0 | MIT |

License texts are included in licenses/. Exact records are in dependencies.lock.json.
Chrome is a separately installed test/host application and is not redistributed.
Node v25.9.0 and Playwright 1.50.0 were also used in an isolated live baseline
probe to record old result contracts. Neither runtime is included in the native
distribution. The new production program does not load Node or Playwright.

The embedded `resources/key_layout.json` and `resources/editing_commands.json`
contain mapping data derived from Playwright 1.50.0's `usKeyboardLayout.js` and
`macEditingCommands.js`, respectively. Copyright 2017 Google Inc. All rights
reserved. Modifications copyright (c) Microsoft Corporation. These two data
resources remain Apache-2.0 licensed; the complete license is preserved in
`licenses/Playwright-Apache-2.0.txt`, alongside its `Playwright-NOTICE.txt`. The original file hashes and transformation
notices are included in each resource. PagePilot changed the data schema,
selected generic virtual-key codes and removed insert commands/trailing colons
from editing bindings. The native chord planner and execution/cleanup code are
new C++ implementations; this data reuse is not a claim of newly authored key maps.

The small legacy-value serializer runs in the browser to inspect values that
cannot survive JSON transport directly; its compatibility behavior is documented
in docs/COMPATIBILITY.md and pinned by observed baseline vectors.
