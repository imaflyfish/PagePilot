# Legacy tool compatibility

`page-pilot --compat-tools` advertises the baseline's 54 primary names and
21 aliases. Canonical names also remain callable in that process. Compatibility
is chosen by the name actually called, including each workflow child; a JSON
argument cannot turn compatibility on. The default process accepts canonical
names only. The table below maps every legacy entry to the new API.

The legacy names come from an older JavaScript browser-automation server. Its
observed behavior was captured by running it under Node with Playwright 1.50.0
against an owned Chrome profile; the resulting vectors are pinned in
`tests/fixtures/` and are what the compatibility layer is checked against.
The new executable does not load Playwright or Node. Isolated baseline probes
use both; two embedded keyboard mapping resources retain Playwright's Apache-2.0
attribution. See `THIRD_PARTY_NOTICES.md` and [KEYBOARD.md](KEYBOARD.md).

## Result and behavior contracts

MCP initialization requires string `clientInfo.name` and `clientInfo.version`.
Notifications receive no response, and malformed notification parameters cannot
complete initialization. For `tools/call`, omitted `arguments` mean `{}`; an
explicit `null`, array or scalar is a malformed request and receives JSON-RPC
error `-32602`. Unknown tool names use that protocol error too. Invalid values
inside an existing tool's argument object and browser execution failures remain
tool results with `isError: true`. These rules apply to both supported protocol
versions and do not change the canonical/legacy name mapping. See the official
[MCP request schema](https://modelcontextprotocol.io/specification/2025-11-25/schema)
and [tool error categories](https://modelcontextprotocol.io/specification/2025-11-25/server/tools#error-handling).

| Legacy call | Preserved behavior | Canonical API |
|---|---|---|
| `eval` | `result` contains a JSON **string**, including quoted string values. The expression runs once in the selected document. Function expressions are returned as undefined, not invoked. | `page_evaluate` returns the raw JSON value; special non-JSON numbers have an `unserializable` field. |
| `new_tab` | Create one tab, select it, clear frame scope, wait for supplied URL to load, then return its index and final URL. | `tab_create` returns the new target ID, index and requested URL; it does not add a navigation readiness wait. |
| `list_tabs` | Stable zero-based indices and one `active` tab. | `tab_list` additionally exposes target `id` and the synonymous `current` flag. |
| `close_tab` | Default to the **last** tab; `closed` is the closed index, including zero. Return only after target removal is observed. | `tab_close` defaults to the selected tab and returns boolean `closed`. Both accept an explicit index. |
| `get_page`, `get_url`, `get_title` | URL/title come from the root page even within a selected frame. Text/source come from that selected frame. Viewport is `null`, matching the baseline's CDP attachment with no emulated viewport. Source includes the doctype. | `page_read` reads the selected document and returns its actual viewport dimensions. |
| `navigate` | Requested/final URL, title, redirect/frame flags and `hasDialog` for the first matching visible dialog/modal/popup. | `page_navigate` exposes the navigation receipt without the extra modal lookup. |
| `stop_loading` | Call `window.stop()` in the selected document. | `page_stop` sends the root page stop-loading command. |
| `exit_frame` | Empty stack returns `exited:false` with `reason:"Not in iframe"`; otherwise return new depth. | `frame_leave` also supplies these fields. |
| `console_logs` | Bounded 100-entry buffer; text, type and timestamp; requested trailing rows; `total` is the buffered count **before** clear. Object/array previews and browser resource errors are included. | `page_console` also returns argument previews, session/source metadata and returned-row `count`. |
| `cleanup` | Return `cleaned`, `before.consoleLogs`, `before.requestStats` and cleared `after` counts. | `browser_cleanup` returns the full previous metrics. |
| `type` | Prefer a nonempty label, then nonempty placeholder, then the global visible-input index, then selector. Label/placeholder choose their first match, ignoring index. An index addresses visible `input` and `textarea` elements together in document order. | `element_type` follows the same target precedence in its selected document/frame. |

Default fast/default/long timeouts are 3000/5000/10000 ms. Explicit values and
parent workflow deadlines bound the entire action, including connection setup.
The baseline's independently reset waits could take longer; those timings are
not reproduced. Extra target IDs and diagnostic fields are additive.

`tests/fixtures/legacy-values.json` contains 29 observed baseline evaluation
vectors. They cover primitives, Unicode, undefined/nonfinite values, bigint,
shared/cyclic objects, symbols/functions, thrown scripts with an execution
counter, date/regexp/error objects, window/document/node references, function
and symbol fields, custom `toJSON`, throwing getters, maps and arrays.
Notable legacy results include `"__undefined__"`, `"__circular__"`, bigint suffix
`n`, function array entries becoming `null`, and invalid dates becoming the Unix
epoch string. These quirks are confined to the legacy serializer. Serialization
traversal has a depth limit of 64 and a 100,000-visit limit; reaching either is
an explicit tool error. It does not silently return partial data.

For text input, `{"selector":"#third","index":1}` addresses the second visible
input/textarea in the selected document, even when it does not match `#third`.
If `label` or `placeholder` is nonempty, it takes priority and selects its first
match. Empty label/placeholder strings are skipped. With `if_exists`, a missing
preferred target returns `typed:false`; it does not fall back to a lower-priority
selector. Hidden controls are excluded from the global index. Input and textarea
order is preserved instead of grouping by tag. These rules apply in all three
typing modes and stay scoped to the selected same-origin or cross-origin frame.

## Deliberate differences and limits

- The original live probe listed the same newly created Playwright page twice,
  with two active rows. PagePilot tracks unique Chrome target IDs and does not
  preserve that bookkeeping defect. Indices refer to its actual unique tab list.
- Snapshots declare native `ax-yaml`, not Playwright `aria-yaml`. The selected
  document's accessibility tree is used, including selected frames. Consumers
  of the old snapshot grammar must migrate; see [PAGE_SERVICES.md](PAGE_SERVICES.md).
- Metrics count native dispatches, including workflow children, grouped by
  resolved operation. The baseline counted outer MCP calls by incoming name.
  Average times use integer division. Both report their values before recording
the metrics call itself. Cleanup is counted after clearing the old counters.
- `get_config.cdpPort` is an integer; the old environment-derived field could
  be a string. Timeouts, booleans and array members now have strict bounds and
  types, and invalid values produce tool errors. Unknown root parameters are
  discarded; nested caller-owned data is preserved.
- Literal storage keys such as `__proto__` and empty cookie values work without
  the baseline's truthiness/prototype losses. Legacy evaluation also preserves
  literal object keys rather than recreating an unsafe normal-object assignment.
- Text matching excludes script/style/head source from ancestor text. A script
  containing a future display string cannot satisfy a visible-text wait early.
- Human/slow input and pointer modes use bounded native pacing; they do not
  reproduce random typo injection or the original random timing distribution.
- Keyboard chords send actual modifier down/up events. Literal names keep their
  specified character (`Shift+a` stays `a`), while physical names apply Shift
  (`Shift+KeyA` becomes `A`). Keypad releases preserve location 3, correcting the
  old release location 1. F13–F24 and `Ctrl`/`Command` aliases are additive. Chords
  are bounded to 16 keys/1024 bytes and fully validated before sending input.
- Console observation covers attached page/frame sessions. Worker logs and
  activity in tabs never attached by this client are not covered. Previews are
  Chrome's bounded remote-object previews, not a full object serializer.
- Error messages describe native CDP failures rather than Playwright stack
  traces. Resource bounds, cancellation and stronger file policies can reject
  requests that the old program accepted. See [FILES_AND_CAPTURE.md](FILES_AND_CAPTURE.md),
  [WORKFLOWS.md](WORKFLOWS.md), [NAVIGATION.md](NAVIGATION.md) and
  [CANCELLATION.md](CANCELLATION.md) for exact side-effect and cleanup boundaries.

Click priority, conditional hover order, all five click types and CSS list parsing
now have original-server vectors and 118 native checks. See [CLICKS.md](CLICKS.md)
for the pre-press recovery policy, no-replay boundary and CSS grammar limits.

## Verification

`tests/integration/mcp_compatibility.py` starts the actual native MCP process and
calls all 75 advertised names. It checks page effects with independent canonical
evaluation, complete upload/capture results, both closing rules, root/frame
scope, ordered compositions, console resource errors, reconnect persistence,
and the 29 pinned baseline value vectors. A name is counted only after an actual
call; the recorded name set must equal the advertised set. This is primary-path
coverage, not a claim that every parameter combination or page layout has been
tested.

`tests/fixtures/input-target-values.json` contains 15 observed old-server target
and result vectors. `pilot-input-target-tests` checks their values/results in
fast, slow and human modes (including canonical and legacy names), plus selected
same-origin/cross-origin frame isolation: 97 checks. Selector and index
precedence is part of that contract, not an incidental ordering.

Every vector in `tests/fixtures/` was observed against an owned temporary Chrome
profile and a local fixture server, never a personal profile. A native run writes
complete MCP stdout/stderr and a `coverage.json` naming each called tool and
check label, so a run can be audited without trusting a summary.

## Name mapping

The preset column lists arguments supplied by an alias. Other arguments retain
their published schema. Protocol field names and compatibility names are kept
because callers depend on them; C++ modules and canonical API names are new.

| Legacy name | Canonical name | Alias presets |
|---|---|---|
| `status` | `browser_status` | — |
| `get_config` | `browser_settings` | — |
| `set_config` | `browser_configure` | — |
| `reconnect` | `browser_reconnect` | — |
| `cleanup` | `browser_cleanup` | — |
| `health_check` | `browser_health` | — |
| `set_debug` | `browser_debug` | — |
| `request_stats` | `browser_metrics` | — |
| `navigate` | `page_navigate` | — |
| `reload` | `page_reload` | — |
| `go_back` | `page_back` | — |
| `go_forward` | `page_forward` | — |
| `stop_loading` | `page_stop` | — |
| `click` | `element_click` | — |
| `type` | `element_type` | — |
| `fill` | `element_fill` | — |
| `fill_form` | `form_fill` | — |
| `wait` | `page_wait` | — |
| `scroll` | `page_scroll` | — |
| `check` | `element_check` | — |
| `assert` | `page_assert` | — |
| `get` | `element_read` | — |
| `get_page` | `page_read` | — |
| `new_tab` | `tab_create` | — |
| `switch_tab` | `tab_activate` | — |
| `close_tab` | `tab_close` | — |
| `list_tabs` | `tab_list` | — |
| `enter_frame` | `frame_enter` | — |
| `exit_frame` | `frame_leave` | — |
| `exit_all_frames` | `frame_reset` | — |
| `list_frames` | `frame_list` | — |
| `press_key` | `keyboard_press` | — |
| `hotkey` | `keyboard_chord` | — |
| `mouse` | `pointer_action` | — |
| `hover` | `element_hover` | — |
| `drag` | `pointer_drag` | — |
| `select` | `form_select` | — |
| `checkbox` | `form_check` | — |
| `upload_file` | `form_upload` | — |
| `dialog` | `page_dialog` | — |
| `cookies` | `browser_cookies` | — |
| `storage` | `page_storage` | — |
| `find` | `element_find` | — |
| `screenshot` | `page_capture` | — |
| `eval` | `page_evaluate` | — |
| `highlight` | `element_highlight` | — |
| `batch` | `workflow_batch` | — |
| `retry` | `workflow_retry` | — |
| `run_steps` | `workflow_steps` | — |
| `console_logs` | `page_console` | — |
| `snapshot` | `page_snapshot` | — |
| `focus` | `element_focus` | — |
| `blur` | `element_blur` | — |
| `count` | `element_count` | — |
| `get_text` | `element_read` | `{"type":"text"}` |
| `get_html` | `element_read` | `{"type":"html"}` |
| `get_attribute` | `element_read` | `{"type":"attribute"}` |
| `get_url` | `page_read` | `{"type":"url"}` |
| `get_title` | `page_read` | `{"type":"title"}` |
| `exists` | `element_check` | `{"state":"exists"}` |
| `is_visible` | `element_check` | `{"state":"visible"}` |
| `wait_for` | `page_wait` | `{"type":"element"}` |
| `wait_for_text` | `page_wait` | `{"type":"text"}` |
| `get_cookies` | `browser_cookies` | `{"action":"get"}` |
| `set_cookie` | `browser_cookies` | `{"action":"set"}` |
| `clear_cookies` | `browser_cookies` | `{"action":"clear"}` |
| `get_storage` | `page_storage` | `{"action":"get"}` |
| `set_storage` | `page_storage` | `{"action":"set"}` |
| `clear_storage` | `page_storage` | `{"action":"clear"}` |
| `move_mouse` | `pointer_action` | `{"action":"move"}` |
| `mouse_down` | `pointer_action` | `{"action":"down"}` |
| `mouse_up` | `pointer_action` | `{"action":"up"}` |
| `select_option` | `form_select` | — |
| `set_checked` | `form_check` | — |
| `toggle_checkbox` | `form_check` | `{"toggle":true}` |
