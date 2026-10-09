# Coroutine behaviour integration branch

Issues #527–#530 implement coroutine execution, fixture migration, the
Host API v3-only contract cut and pure-time sleep of #525 on `feature/coroutine-behaviours`.
The branch now accepts only `api_version = 3` and `promethium.v3` imports.
Factories must return a coroutine function(context), not a callback table;
legacy versions and callback-table factories receive migration diagnostics.
Full integration into `master` remains a subsequent slice.

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

The initial resume supplies the host context. Event resumes supply
one immutable semantic event, returned by `wait()`. Sleep completion resumes
supply no event; `sleep(ticks)` returns no values. Context command proxies
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
existing structured diagnostic / teardown / pre-tick pause policy. The existing `callbacksPerBoundary`, `commandsPerCallback` and
`instructionsPerCall` limits also govern coroutine resumes. Teardown closes
suspended threads with Lua 5.4 close semantics under an instruction budget;
it does not resume their bodies.

## Pure-time sleep (#530)

`sleep(ticks)` (or `context.sleep(ticks)`) suspends for a positive integer number
of simulation ticks. A sleep begun at boundary N completes at boundary N + ticks.
Zero, negative, fractional, non-numeric and missing durations are refused.
Events cannot interrupt sleep. They remain in the bounded pending-event queue,
and subsequent `wait()` calls receive them exactly once in event-sequence order.
Starting another sleep while draining a queue retains all undelivered events.
Sleep completion itself consumes one resume from the per-boundary budget.

Pause freezes simulation time; deactivation freezes the remaining sleep duration
and clears pending events. Inactive instances accumulate no events; reactivation
queues an activation event without interrupting any remaining sleep. Returning,
unassignment, Reset and reload discard sleeping runtime state as usual.

The named-timer API (`set_timer` / `cancel_timer`), timer-expiry events,
per-instance timer budget and staged-timer machinery are removed. There is no
polling or timer bridge. Use `wait()` for events and `sleep(ticks)` for pure time.

## Fixture and resource migration (#528)

Executable behaviour fixtures and the bundled marker patrol and random wander
resources now use v3 coroutine factories. Old callback tables remain only as
explicitly malformed contract inputs. Scheduling fixtures, patrol and wander
express their journey and arrival-delay loops sequentially through `wait()`
and `sleep(ticks)`.

Regression expectations follow the coroutine contract: deactivated Agents
publish public transitions but receive no resumes; startup context snapshots
remain immutable; event ticks describe subsequent resumes; failed threads may
publish both `resume` and `close` diagnostics. Teardown checks assert suspended
work is not resumed after failure, unassignment, reload or World close, rather
than expecting the removed `on_stop` callback. The former v1/v2 route-loss
selectors are now `routeLossAndTopologyLifecycle` and
`routeLossAndTopologyLifecycleReplay`; planning replacement checks exercise
independent v3 Agents rather than legacy version-dependent movement semantics.

#528 migrated fixtures and resources; #529 removed callback dispatch and legacy
Host API acceptance; #530 removed named timers and migrated dependent fixtures
to sleep. Final ADR/authoring documentation (#533) remains a later slice.

Validation: Release `pf-smoke-behaviours` and the behaviours CTest functional,
CLI and concurrency contracts. The new `RuntimeCoroutines.cpp` checks cover
independent instances, wait/event delivery and ordering, fire-and-forget
completion, unassignment, failures, budgets, activation and queue overflow.
