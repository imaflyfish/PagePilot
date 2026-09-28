# Native clicks and CSS selection

`element_click` and legacy `click` share a native C++ executor in
`src/tools/click_tool.cpp`. Coordinates take priority when both are supplied;
otherwise nonempty text precedes a nonempty selector. An empty text argument does
not hide a selector. A missing `if_exists` target returns before `hover_first`.
An empty hover selector is ignored. Element targeting remains scoped to the
selected document; result URL/title describe the root page.

Before pressing, the executor requires a visible, enabled, uncovered target,
scrolls it into view, and compares geometry across two animation frames. After
paced pointer movement it checks identity, geometry and hit testing again. An
observed detached node can be reacquired before pressing. A frame owner that
moves requires new projection into root coordinates. Shadow targets also check
outer host/document overlays. Input is pinned to the original tab.

Once a press is attempted, the action does not reacquire or replay it. Double and
triple clicks revalidate the same node before later presses. Partial actions can
therefore fail after their first effect. Held buttons and temporary input
observations receive bounded cleanup after failure. A last geometry observation
and a later CDP input command are not atomic; arbitrary page mutation between
them cannot be ruled out. Preparatory remote-object failures may terminate the
action instead of reacquiring. Returned element text is sampled before input.

The CSS parser separates top-level comma branches while respecting escapes,
quotes, comments, brackets and parentheses. A trailing unescaped `:visible` on
each branch is a visibility filter; `#literal\\:visible` remains a literal CSS
identifier. Union results use document/host traversal order and remove duplicates.
Visible input indices use the same ordered input/textarea union, including open
shadow roots. Native CSS is queried in each root. Extended Playwright selector
engines, cross-shadow ancestry selectors and arbitrary nested-shadow ordering
equivalence are not claimed.

## Pinned vectors and test design

`click-values.json` contains 28 result/native-event vectors, covering all five
click types, three modes, coordinates, target priority, conditional hover, menus
and scrolling. `css-list-values.json` contains 16 selection vectors, including
escaped punctuation, nested CSS functions, visibility, document/shadow order and
mixed input indices. Both were observed against a reference server running in an
isolated owned Chrome with Playwright 1.50.0.

`pilot-click-tests` checks those vectors and adds moving, replaced and animated
targets, native AX role reacquisition, new overlays and disabled controls, no
replay after press, long-press deadline cleanup, shadow overlays, moving
cross-process frame owners and independent tab closure.

Hover comparison is deliberately narrow: incidental hovers along a random human
pointer path are not compared, only the hover effects of explicit `hover_first`
vectors. Trusted click event sequences remain compared for every vector.

Timing assertions are the fragile part of this suite. A preparatory
animation-frame callback can be delayed far enough that a short total allowance
expires before any press happens — a test precondition failure that looks like a
click defect. The long-press failure test therefore allows enough time for the
first press under instrumented builds, then checks its bounded release.

The replacement test uses the ordinary click allowance and requires both
the specific missing-acknowledgement error and exactly one press. The long-press
test likewise uses the ordinary allowance, requests a hold longer than that
allowance, and requires the specific delay-deadline error plus exactly one
press/release pair. Neither action is retried. Button cleanup and the replacement
case's receipt-binding cleanup remain required. Failure diagnostics include
error type/code/message, elapsed time, actual events, visibility, browser clock
and pointer state. The short pre-press refusal cases remain separate. Results
vary with the Chrome build in use; a timing failure on one version is not by
itself a product defect.
