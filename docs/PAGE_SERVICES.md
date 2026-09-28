# Pointer and page services

These operations are implemented by the C++ runtime and real CDP sessions.
`page_services.cpp` owns cookies, storage, dialog rules and accessibility output;
`pointer_state.cpp` owns page-specific pointer state and native drag interception.
The DOM adapter only performs the necessary page-local outline manipulation.

## Pointer and drag

`pointer_action` retains coordinates and the left/right/middle button bitmask
between calls, independently for each tab. `down` and `up` use the saved position;
`move` interpolates from that position. Numeric coordinates are root viewport CSS
pixels, including while a frame is selected. Zero steps is treated as one move,
so a successful move always reaches the returned coordinates.

`pointer_drag` accepts source/destination selectors, absolute coordinates or
relative offsets. Selector endpoints use the same actionable hit testing and
frame projection as element clicks. Both selector endpoints must be visible
together after revealing them; auto-scroll during a drag is not implemented.
The pointer follows 16 native moves and releases at the requested endpoint.

For HTML drag-and-drop, Chrome's actual `Input.dragIntercepted` payload is used
for `dragEnter`, `dragOver` and `drop`. No DOM `dispatchEvent` substitutes for
trusted input. Plain mouse listeners instead receive the usual pressed-button
mouse moves. A successful dispatch does not assert that a site's drop handler
accepted the content; callers can inspect the resulting page state.

Errors/deadlines and disconnect release held buttons, cancel intercepted drags
and disable interception. Cleanup uses separately bounded requests. The server
still does not terminate the browser. Pointer position resets on reconnect.

Actual tests cover separate move/down/up calls, interpolation, native event
button masks, middle/right clicks, multiple buttons, coordinate/offset drags,
trusted HTML dataTransfer, subsequent normal clicks, per-tab state, cleanup
and a long-click deadline. An HTML drag inside a rotated/scaled OOP frame also
produces a trusted drop and the expected payload.
Cross-frame drags, files dragged from outside Chrome, touch and pen input are
not claimed by these tests.

## Dialogs

`page_dialog` arms one acceptance or dismissal for the active page. Prompt text
is passed literally. The rule follows that page across tab switches and applies
to its selected subframes too. An unarmed alert/confirm/prompt is dismissed.
Replacing an armed rule replaces the previous rule.

Pending CDP calls pump events, allowing a JavaScript evaluation or native click
that opens a modal to finish. Actual tests exercise prompt/confirm/alert,
one-shot consumption, per-tab isolation, Unicode prompt text, native click
receipts and same-process/OOP frames. Beforeunload acceptance/dismissal and rule
consumption are covered by the final recovery suite.

## Cookies and storage

Cookie operations address the browser context selected on connection, using
`Storage.getCookies`, `Storage.setCookies` and `Storage.clearCookies`. Filtered
deletion passes the actual name/domain/path and optional partition key through
`Network.deleteCookies`. Empty cookie values are accepted. Expiration values are
Unix seconds. The tool does not bypass Chrome's own cookie policy.

The context ID reported by a page is kept for tab filtering and private-context
isolation. Before a cookie command or new-tab command, `Target.getBrowserContexts`
identifies the default profile explicitly. Only a matching
`defaultBrowserContextId` is omitted from the command: Chrome 152 exposes that
internal ID in target metadata but rejects it in `Storage` calls. Private or
unrecognized IDs remain explicit, including after their context is disposed;
failure never triggers a retry against the default profile. Omitting the ID on
a private page session is not an isolation mechanism: `Storage` still addresses
the default profile in the tested versions.

Local/session storage operates in the selected document's real default context.
Keys and values are literal JSON data; empty keys, Unicode, `__proto__` and
`constructor` roundtrip without prototype semantics. Missing reads return null.
The get-all result preserves all keys. Same-origin frames share local storage;
the owned cross-origin frame test proves origin isolation.

Tests cover cookie aliases, path coexistence/deletion, HttpOnly/secure flags,
empty values, expiration, browser-context isolation, explicit clear, and local
versus session storage with reload persistence. Partitioned-cookie deletion and
third-party-cookie restrictions still need dedicated cases.

The separate 18-check `cookie_context_live.cpp` regression exercises default
cookie access, private get/set/clear/delete isolation, tab creation within the
selected private context, and rejection after that context is disposed while
the default profile remains intact. Its browser is owned by the test harness;
it deliberately replaces test tabs to select the private context unambiguously.

## Highlight and accessibility

`element_highlight` validates the supplied color, temporarily applies an outline
and restores each original inline property and priority. Overlapping highlights
share the original values; a later page edit to a property is preserved. The
temporary element symbol/timer is removed when the highlight expires. It does
not install a page-global property.

The duration schedules a browser timer; delayed renderer scheduling can postpone
its callback beyond that many milliseconds on the client clock. The service
test observes actual lease completion before asserting the original outline and
priority. Separate controlled-callback cases execute the real cleanup closures
in a known order, including an already-cancelled older callback, and verify
overlap ownership and preservation of full or partial page edits. A failed
assertion records the inline style, priority, lease, visibility and browser clock.

`page_snapshot` reads the selected document with `Accessibility.getFullAXTree`.
It returns a structured `tree` and a readable YAML mapping in `snapshot`, with
`format: "ax-yaml"`. Roles, computed names, values, descriptions and primitive
states are preserved; ignored structural wrappers are flattened and hidden
subtrees/duplicate inline text are omitted. Bounds are 10,000 AX nodes, traversal
depth 128 and 4 MiB of YAML. Oversized output fails rather than truncating.

This is an explicit output-format difference from the old Playwright
`aria-yaml` shorthand, including when called through the legacy `snapshot`
name. It is not intended as a Playwright snapshot assertion file. Selected OOP
frame tests show child content without root-page content. Exhaustive large-tree
and accessibility widget compatibility is outside the validated matrix.

Public arguments are filtered to each tool's declared properties before alias
presets are applied. Arbitrary nested form field names remain intact. Real fill
and MCP cases prove an unknown `field` argument cannot redirect the public
selector through an internal DOM helper field.

Protocol reference: [Chrome DevTools protocol definitions](https://github.com/ChromeDevTools/devtools-protocol/blob/master/json/browser_protocol.json).
How to run the browser suites is described in
[validation/README.md](../validation/README.md).

Beforeunload accept/dismiss and one-shot rule consumption are additionally checked
by the final 13-case recovery suite; see `NAVIGATION.md`.
