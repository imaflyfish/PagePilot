# Native workflows

`workflow_batch`, `workflow_retry` and `workflow_steps` execute through the same
C++ catalog, argument validation and runtime as individual calls. Compatibility
names are `batch`, `retry` and `run_steps`. Children inherit the server's name
policy; an `allow_legacy` or `_depth` field in JSON cannot change it. Each actual
child dispatch contributes to operation statistics.

## Ordered batch

```json
{"actions":[
  {"tool":"element_fill","args":{"selector":"#person","text":"Example"}},
  {"tool":"element_click","args":{"selector":"#submit"},"stopOnError":true},
  {"tool":"element_read","args":{"selector":"#result","type":"text"}}
]}
```

The result contains `executed` and ordered `results`, each with `tool`, `success`
and either `result` or `error`. A failed row continues unless its `stopOnError`
is true. Empty batches are allowed; a batch has at most 1,000 rows. Canonical
calls validate the row structure up front. The legacy name retains per-row
diagnostics for malformed rows, then validates each child before execution.

## Retrying a child

`tool` and `args` identify a child. **`max_retries` is the total number of attempts**,
matching the old API despite its name: default 3, range 0–10. Zero validates the
child but never executes it. `delay_ms` applies between attempts, not after the
last attempt. Success stops immediately and returns `success`, `attempts` and
the complete child `result`. Exhaustion returns `success:false`, `attempts` and
the final exception if the final attempt threw.

Without `success_check`, a nonthrowing child succeeds even when its result includes
`passed:false`. With `success_check`, the named top-level result field must be
truthy: null, false, zero and an empty string are false; arrays and objects are
true, including empty ones. A missing field is false. This is a field check,
not a JavaScript expression.

## Step workflow

`steps` contains 1–50 `{tool,args}` entries. A row may also have `optional`,
`timeout`, `wait_before` and `wait_after`. Global controls:

| Parameter | Default | Meaning |
|---|---:|---|
| `max_step_retries` | 2 | **Extra** attempts after the first; integer 0–10 |
| `retry_on_fail` | true | False forces one attempt regardless of retry count |
| `auto_wait` | true | Adds a 500 ms delay between failed attempts |
| `step_timeout` | 10000 | Per-attempt allowance in milliseconds |
| `stop_on_error` | true | Stops after a required row fails |
| `return_intermediate` | false | Includes complete successful results in each row |

A positive row `timeout` replaces `step_timeout`; row zero uses the global value,
as in the baseline. Global `step_timeout:0` fails the attempt before executing it.
`wait_before` and `wait_after` are capped at 5,000 ms. `auto_wait` only controls
retry delay; normal element readiness checks remain active.

An optional failure remains in the output and permits the next row. With
`stop_on_error:false`, other failures also continue. Row validation failures have
no attempts. Nested batch/retry/step tools are refused inside steps, through both
canonical and compatibility names, before the nested child can run.

The result includes `total_steps`, `executed`, `succeeded`, `failed`, `steps`,
`last_result` and `total_time_ms`. Successful rows may include a concise `brief`;
UTF-8 text is clipped at scalar boundaries. `last_result` remains complete even
when intermediate results are omitted. Root-page URL/title are included when
available within the remaining allowance; they do not replace the selected
frame. A nonthrowing assertion with `passed:false` is an execution success, with
an explicit failed-assertion brief and unchanged full assertion result.

## Bounds, failures and effects

Every composition has a total `timeout` (default 60,000 ms, range 1–600,000 ms).
Nested actions and per-step limits can shorten, never extend, the parent deadline.
An elapsed parent deadline or a requested delay that cannot fit stops further
children and sets `timed_out:true`. Per-step timeout scope is restored before
the next step. Small bounded remote-object/input cleanup may outlast the allowance.

Batch/retry wrappers allow at most five nested levels below the root. A root
request has at most 1,024 dispatches, including composition wrappers. Workflow
results are limited to 64 MiB. Depth, dispatch and output limits are fatal to the
whole composition, so an outer retry cannot hide a resource-limit failure.
The next root request receives a fresh dispatch budget.

Workflows are sequential operations, not transactions. Earlier successful page
changes remain after failure. Explicit retries may repeat side effects that
occurred before an error. A timed-out page script or asynchronous task already
started in Chrome may still act later; host-side deadlines do not roll back or
terminate arbitrary page JavaScript. Client-request cancellation exits the whole composition without retrying or
executing remaining children; see `CANCELLATION.md` for its independent evidence
and the distinction between stopping host processing and undoing page effects.

## Evidence

`workflow_contract.cpp` checks 44 catalog/sequencing/resource contracts, including
counterexamples for depth, dispatch count and aggregate output. `workflow_live.cpp`
checks 41 real-page cases: effects, exact script counts, failure policy, deadlines,
Unicode results and OOP-frame scope. `mcp_workflows.py` checks 53 actual MCP
process cases across the two declared protocol versions, including full result
transport, compatibility names and invalid child handling. Listing handlers is
not coverage; run the suites described in
[validation/README.md](../validation/README.md) and read their output.
