# Coroutine behaviour integration branch

Issue #527 is the expansion slice of #525, on `feature/coroutine-behaviours`.
Do not merge this dual-contract state into `master`: the hard migration and
full integration are subsequent slices. Existing v1/v2 callback tables remain
supported on this branch; v3 factories return a function, not a callback table.

```lua
local host = require("promethium.v3")
return {
  api_version = host.api_version,
  factory = function(configuration)
    return function(context)
      assert(context.move_to("Mess Hall").accepted)
      local event = wait() -- context.wait() is equivalent
      assert(event.type == "destination_reached")
    end
  end
}
```

The initial resume supplies the host context. Each subsequent resume supplies
one immutable semantic event, returned by `wait()`. Context command proxies
retain the per-instance host scope; their command validation is refreshed on
each resume. Snapshot fields in a retained context are startup observations,
not live mutable World objects. Event ticks describe subsequent outcomes.

Each Agent has its own module environment, factory closure and coroutine.
Events are delivered in stable Agent-ID / event-sequence order; successful
resume commands apply only after all boundary resumes. A second movement
intent in one resume is an error. Returning completes the installation without
revoking movement, including commands staged by the final resume. The unchanged
authored assignment is retained but does not restart completed work.

Deactivated instances are not resumed and retain no pending events; activation
is delivered on reactivation. `pendingEventsPerInstance` defaults to 64. Queue
overflow, instruction exhaustion, resume-cap exhaustion and Lua errors use the
existing structured diagnostic / teardown / pre-tick pause policy. During this
expansion the existing `callbacksPerBoundary`, `commandsPerCallback` and
`instructionsPerCall` limits also govern coroutine resumes. Teardown closes
suspended threads with Lua 5.4 close semantics under an instruction budget;
it does not resume their bodies.

## Temporary timer bridge for #528

V3 coroutines can temporarily retain `context.set_timer(name, ticks)` and
`context.cancel_timer(name)`. Expiry resumes `wait()` with an immutable event:
`{ type = "timer_expired", name = name, tick = expiryBoundary, sequence = sequence }`.
Use `event.tick`, not the retained startup context's `tick`, for expiry time.

Semantic outcomes are delivered before due timers, allowing an outcome resume
to cancel or replace a timer before expiry is collected. Due timers form a
lexically ordered, one-shot batch; all names are removed before its first
resume. Cancelling a timer already in that batch is a no-op; re-arming its name
schedules a new expiry no earlier than the next boundary. Deactivation freezes
remaining timer durations. Timer events use the World event sequence and obey
pending-event and per-boundary resume budgets, including normal failure
containment. Completed and unassigned coroutines receive no further expiry.

`sleep(ticks)`, removal of callback/timer APIs, bundled-resource migration and
the final ADR/authoring documentation belong to later issues, not #527.

Validation: Release `pf-smoke-behaviours` and the behaviours CTest functional,
CLI and concurrency contracts. The new `RuntimeCoroutines.cpp` checks cover
independent instances, wait/event delivery and ordering, fire-and-forget
completion, unassignment, failures, budgets, activation and queue overflow.
