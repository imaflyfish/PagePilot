# Request cancellation and input scheduling

The native stdio server reads input independently of one execution worker.
`RequestPump` preserves operation order; browser and workflow state is only
accessed by that worker. A queued operation cannot run concurrently with another
browser operation merely because a client pipelines requests.

`notifications/cancelled` with a matching `requestId` cancels an active or queued
request. Numeric and string IDs are distinct. Unknown, already completed,
malformed and repeated cancellation notifications are ignored. Initialization
cannot be cancelled. Null request IDs are rejected before dispatch. A cancelled
request produces no response, and the notification itself receives no response.
If completion wins the race before cancellation is observed, its normal response
may already be sent.

```json
{"jsonrpc":"2.0","id":"long-wait","method":"tools/call","params":{"name":"page_wait","arguments":{"ms":30000}}}
{"jsonrpc":"2.0","method":"notifications/cancelled","params":{"requestId":"long-wait"}}
```

Cancellation applies to both supported protocol versions and to compatibility
tool names. The server does not advertise MCP tasks or implement task-augmented
requests; this mechanism is ordinary request cancellation.

## Execution and cleanup

`CancelScope` carries a request's `std::stop_token` on its worker thread.
Waits check it in short intervals. HTTP discovery, WebSocket handshake, pending
CDP responses, DOM polling, workflow delays, input pacing and bounded filesystem
or image-processing loops have cancellation checkpoints. `RequestAborted` is
separate from ordinary tool failures; optional steps, batch error handling and
retry wrappers cannot swallow it and continue execution.

Queued cancelled operations never reach the runtime. Active cancellation stops
further workflow children and drops the pending host-side CDP response mapping.
A late Chrome response cannot become the response to a different request.
Runtime deadline scopes and dispatch depth unwind before the next root call.

DOM actions and upload/capture actions also bind to the page selected on their
first browser access. Closing that page during a multi-command action fails the
remaining work instead of sending it to another tab. This includes slow typing,
locator polling and multi-field forms. A later explicit request may select the
surviving tab normally. The binding is lazy, so a time-only wait still needs no
browser connection. It does not prevent navigation within the original tab or
undo input already dispatched there.

Bounded cleanup masks cancellation temporarily so native held mouse buttons,
drag interception, keyboard key-up, scoped input observers, bindings and remote
object handles can be released. Failed or cancelled session initialization
discards the incomplete session; a failed cold connection resets browser setup.
WebSocket shutdown explicitly stops its event loop after failing pending callers,
so an outstanding handshake timer does not delay cancellation until its original
timeout. Client shutdown only disconnects its protocol connection; it does not
close Chrome.

Cancellation is cooperative. It does not undo earlier page changes, completed
file writes or dispatched input. It does not terminate arbitrary page JavaScript
or stop a browser navigation already in progress. Chrome may finish that work
later. Synchronous OS calls, JSON serialization and individual bounded CPU work
can delay the next checkpoint; the tests do not establish a universal latency
guarantee. Ordinary EOF drains accepted requests. A fatal input/output failure or
queue destruction instead cancels pending work and joins the worker.

## Queue boundaries

- At most 128 active/queued messages and 64 MiB of encoded pending input are
  accepted by default; each input line remains limited to 16 MiB.
- Duplicate pending request IDs and excess requests receive a JSON-RPC error
  before execution. Notification overflow is a fatal session input error.
- Parsing/refusal responses may appear before an earlier slow request's result.
  Clients correlate responses by ID. Ordinary accepted browser operations retain
  their input order.
- Sink calls are serialized. Output failures propagate to the session owner;
  diagnostics remain on stderr. Cancellation reasons are not echoed into page
  scripts, protocol results or diagnostic logs.

These limits bound encoded input and queue length, not the exact allocator heap
size. The library's browser/runtime classes remain single-executor objects;
`RequestPump` is the scheduler used by the CLI, not a claim that arbitrary
concurrent calls into one `ToolRuntime` are supported. Custom handlers must
cooperate with cancellation checkpoints; the queue cannot forcibly interrupt
arbitrary callbacks. Sink callbacks must not reenter or destroy their queue.

## Evidence

`cancellation_contract.cpp` has 22 checks covering scoped cancellation/cleanup,
active and queued work, typed IDs, malformed/late notifications, initialization,
duplicate/count/byte limits, destruction, EOF draining, sink failure and nested
workflow propagation. The MCP contract additionally rejects null IDs.

`mcp_cancellation.py` has 25 interactive checks. A second native MCP client
independently observes the owned browser to prove when a script or workflow has
actually started. Cases include fragmented cancellation, an unresolved promise
and its later CDP reply, cancelled queued input, batch/optional-step/retry tails,
held mouse-button release, subsequent native click, OOP-frame scope and input,
legacy MCP calls, and deliberately stalled discovery/handshake endpoints.

Two mistakes are easy to repeat when extending these tests: referencing a tab
name that is not advertised, and requesting a wait too long to fit inside its
parent workflow, so the step finishes before cancellation can reach it. Both
produce a green-looking run that proves nothing. A third case, the real handshake
timer shutdown delay; the later accepted runs contain its fix. An initial LLVM
test build also required an explicit standard `<thread>` include.

References: [MCP 2025-11-25 cancellation](https://modelcontextprotocol.io/specification/2025-11-25/basic/utilities/cancellation)
and [MCP 2024-11-05 cancellation](https://modelcontextprotocol.io/specification/2024-11-05/basic/utilities/cancellation).
