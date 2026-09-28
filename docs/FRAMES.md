# Frame execution and input

The current implementation selects frames by the same element selectors used by
other actions. `frame_enter` requires an actual iframe/frame owner, pushes one
scope, and reports the selected frame ID and whether it uses a separate CDP
session. `frame_leave` pops one scope; `frame_reset` returns to the selected page.
Entry failure leaves the prior scope unchanged. Nesting is bounded at 32 levels.

`frame_list` starts at the selected page. It combines same-process Page frame
trees with discovered out-of-process iframe targets whose parent already belongs
to that page. It does not enumerate iframe targets belonging to unrelated tabs.
The inventory contains index, ID, URL, name and parent; its current depth refers
to the selected scope, even when the inventory is requested from inside a frame.

## Documents and sessions

`BrowserSession` records default execution contexts from Runtime events. Page
evaluation and DOM selection run in the frame's actual default world, so page
variables and handlers are available. An isolated replacement world is not used.
Runtime evaluation uses Chrome's unique context identity; DOM node resolution also
uses the corresponding numeric context where required by the protocol.

`NodeLease` retains both the remote object ID and its creating session. Its
calls and file-input commands use that original session even if a library caller
changes the selected tab or frame while holding the lease. A destroyed session
or object reports failure; its command is never retried against a different
object. This also allows a still-live parent or OOP frame lease to be read after
selection changes. The workspace and its leases remain single-executor objects.

The same frame can move between a page process and a separate iframe process
during navigation. Frame identity stays selected while its session/context is
resolved again. Failed attachment after a target disappears and a missing-session
response before evaluation are recoverable within the action deadline. An
arbitrary page script error is never retried. The missing-session retry cannot
reissue a script that reached a renderer.

Removing a selected frame makes subsequent frame actions fail. They do not silently
fall back to an identical selector in the parent document. Explicit leave/reset
can recover the parent scope. Explicit page navigation/tab changes clear scopes.

## Coordinates and event completion

DOM rectangles are local to a document; CDP content quads are relative to a local
process root. `frame_geometry.cpp` projects points through frame-owner content
quads and normalizes same-process ancestors so their offsets are not counted twice.
It accounts for frame borders and transformed owners. Current real tests include
rotation and scale; exhaustive perspective, animation and device-scale coverage
remains part of the final browser matrix.

Before pointing, the owner chain and element are revealed and browser rendering
turns are allowed to commit. Ancestor hit tests refuse input through a parent
overlay. Element bounds are reported in root-page coordinates; viewport checks
include ancestor viewport clipping. Root coordinate clicks retain page coordinates.

Mouse routing across processes is asynchronous. `input_observer.cpp` registers a
scoped native Runtime binding and a temporary DOM listener. The global binding
property is removed immediately after capturing its function. Only trusted matching
events produce a receipt; a later task marks ordinary event propagation settled.
The client receives receipts independently of the JavaScript object lifetime, so
click-triggered navigation can destroy the old renderer without losing the event.

Completion requires a settled receipt, or an observed trusted event followed by
the original document/context disappearing. A harmless context probe distinguishes
document loss from an unrelated protocol failure. A probe timeout alone is not
completion. Mouse input is never resent to recover a missing receipt.

Listeners, native bindings and remote objects are cleaned up after success or
failure. A page-side watchdog removes the listener if the client disappears.
Cleanup is bounded and may extend beyond the action deadline. These receipts are
an execution mechanism, not a security attestation about an untrusted page.

## Evidence

`tests/integration/frame_live.cpp` checks real three-level same-origin and
cross-origin trees, independent OOP sessions, default-world variables, Unicode
input, keyboard, hover, native clicks, transforms, overlays, global bounds,
viewport clipping, inventory, enter/leave/reset, both process-swap directions,
click navigation and detached-frame failure without parent mutation.

`tests/integration/mcp_live.py` exercises a separate native MCP process through a
cross-origin frame and its nested child. Both use the owned local fixtures
`frame-host.html` / `frame-branch.html`, not an existing user's browser profile.

Protocol references: [Chrome browser protocol](https://github.com/ChromeDevTools/devtools-protocol/blob/master/json/browser_protocol.json)
and [Chrome Runtime protocol](https://github.com/ChromeDevTools/devtools-protocol/blob/master/json/js_protocol.json).
The scoped numeric `Runtime.addBinding.executionContextId` parameter is deprecated
upstream but present and tested in Chrome 153. No broader Chrome-version claim is made.

## Page-service stage follow-up

The current frame suite has 58 checks per Debug/ASan build. Ten checks were added
for four alternating same-process/OOP replacements, exactly one script execution
in each replacement document, and no retry of a script exception after a side
effect. A sanitizer build run exposed `-32602: uniqueContextId not found` between
context selection and evaluation; only this pre-execution identity lookup failure
(and the earlier missing-session failure) may be retried. The old identity is
removed without erasing a newly published context.

The separate page-service and pointer suites add real OOP storage, dialog,
highlight, snapshot and transformed-frame HTML drag/drop cases. See
`PAGE_SERVICES.md` for their boundaries.

The later condition suite covers pending predicates whose document disappears
during root/frame navigation, both frame process-transition directions, and
target closure. A detached OOP session can leave Chrome's old promise request
unanswered. The native waiter now detects context loss; conditions can continue
polling, while arbitrary scripts fail without replaying prior effects. See
[NAVIGATION.md](NAVIGATION.md) for the actual trace and 19 targeted checks. This
does not change the original 58-check frame suite's count.
