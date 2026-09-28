# Navigation completion

Explicit `page_navigate`, `page_reload`, `page_back` and `page_forward` bind
completion to the destination document. They clear selected iframe scope and
operate on the root page. They do not select another tab when the target session
disappears during the operation.

For navigation, Chrome's `Page.navigate` returns the destination loader identity.
The native waiter observes that loader in `Page.getFrameTree` before checking
readiness. Same-document navigation omits a new loader identity, so the waiter
retains the existing document. Redirect results include the final URL and title.
Protocol navigation failures and responses that start a download are errors,
not a successful result using the old page's title.

Reload captures the current loader and supplies it to `Page.reload`, then requires
a different committed loader. The old document's `readyState:complete` cannot
complete a delayed reload. History navigation checks the desired history entry
identity before accepting readiness. This handles fragment/pushState entries and
cross-document history, including a restored cached document.

Readiness and URL/title are read from the committed frame's unique default
execution context. After the read, the waiter checks that both loader and context
are unchanged. A known context-lookup race retries only this read-only observation;
the navigation action is never automatically resent.

| Readiness | Completion condition |
|---|---|
| `domcontentloaded` | Destination document is interactive or complete |
| `load` | Destination document is complete |
| `networkidle` | Destination is complete, with no tracked requests for 500 ms |

Network tracking records request frame and loader ownership. Committing a
replacement document removes stale requests from the discarded document;
detached frames/sessions also discard their request records. This matters when
Chrome never emits `loadingFinished` for an unread fetch body in a discarded
document. Requests in attached OOP sessions belonging to the page are included in
the quiet check. This is not proof of network activity in unattached targets,
workers, service workers or persistent socket traffic.

One absolute deadline covers connection, session setup, command dispatch,
commit/readiness polling and result extraction. Nested scopes preserve the exact
parent time point; converting each scope through truncated milliseconds had
allowed an extra failed workflow attempt near expiry and has been corrected. Nested workflow allowances may shorten it. Timeout
stops host-side waiting; it does not cancel an already-issued browser navigation.
Use `page_stop` before deliberately replacing a pending navigation when needed.
Deadline scopes restore for the next request.

## Conditions during document replacement

`page_wait` with `type:function` is a polling operation. If its pending predicate
loses the selected document, the native waiter abandons that request and checks
the replacement document within the original deadline. It observes the unique
default context, including attached cross-process frame sessions. The two
observed Chrome errors `Inspected target navigated or closed` and
`Execution context was destroyed.` also permit condition polling to continue.
The original page target is pinned: closing it fails the condition instead of
moving the expression to a different tab.

Ordinary `page_evaluate`/`eval` does **not** replay a script after observed context
loss: prior effects may have occurred. It fails promptly and abandons the pending
response. Only the existing pre-execution missing-session/context lookup failures
can retry ordinary evaluation. Condition expressions, by contrast, may execute
multiple times by their polling contract, so callers should not use them as a
one-shot mutation API. Cancellation and deadlines still propagate without retry.

An owned trace reproduced Chrome leaving a Runtime.evaluate request unanswered
after its OOP session detached: detachment arrived about 557 ms after submission,
the replacement context about 562 ms, but the old code waited until the 1500 ms
timeout. Its following evaluation saw the replacement marker. This explains a
concrete lost-context failure mode; it does not prove every timeout has the same
cause. Production diagnostics remain argument-free.

`pilot-condition-tests` adds 19 actual-browser checks: pending promises across root
navigation and both directions of frame process replacement, preservation of
frame scope, prompt arbitrary-script failure with exactly one observed effect,
and independent closure of the awaited target without evaluation in a survivor
tab. A separate 48-swap diagnostic run passed but did not replace the targeted
lost-promise counterexample. Earlier condition attempts exposed both Chrome
error variants above. Note that the new-tab result field is `target`, not `id`;
reading the wrong field makes a passing test meaningless.

## Actual cases

The owned fixture server can delay response headers, images and fetch bodies,
redirect to another local document, return 204, or close a connection without a
response. Native checks cover slow commit/load, delayed reload, DOM readiness,
network quiet and discarded requests, redirects, fragment/pushState history,
cross-document history, two top-level process changes, timeout/stop/recovery,
network failure, no-content responses and invalid readiness without mutation.

An actual MCP child separately checks slow navigation/reload, redirect/history
results and a parent workflow deadline preventing subsequent steps. Server-side
document sequence numbers independently prove that reload returned a new response.

The first navigation attempt found stale unread-fetch requests retaining the
previous document. Its failing evidence and temporary diagnostics are retained in
`navigation-debug-initial/` and `navigation-debug-diagnostic/`; diagnostics were
removed from production source. The parent-deadline regression is worth
watching when changing this code: a sanitizer build can pass it by timing
accident while a debug build fails it, so a single green run is not evidence that
the deadline is honored.

The final recovery suite adds 13 actual cases: delayed navigation triggered by
click and `wait_after`, beforeunload acceptance/dismissal with one-shot rules,
superseding navigation, explicit target closure, subsequent navigation recovery,
and original-target closure during generic load waiting. The last case failed
before the fix: waiting could accept a survivor tab. Generic waiting now pins
the original target and includes setup in its absolute deadline.

Generic load waits do not receive a `Page.navigate` loader receipt or predict
future timer-scheduled navigation. Explicit navigation returns only for its own
loader/context; a superseding navigation fails within the original allowance.
Cold discovery/handshake and 56 other connection scenarios have separate wire
fault evidence. The experimental reload `loaderId` guard is tested with Chrome
153; older Chrome releases are not established by these results.

Protocol reference: [Chrome DevTools Page domain](https://chromedevtools.github.io/devtools-protocol/tot/Page/).
The official JSON definitions for that domain are published with the protocol
itself and are not vendored here.
