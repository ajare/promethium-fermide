# Run Installed Agent behaviours as coroutines

Status: accepted

Sequential Agent intent was fragmented across callback tables and named timers.
Host API v3 replaces that contract with a per-Agent factory-produced Lua coroutine,
so movement, event handling and time delays read top-to-bottom using `wait()` and
`sleep(ticks)`. This supersedes the callback and named-timer portions of
[ADR 0008](0008-run-agent-behaviours-in-deterministic-lua-runtimes.md), not its
sandbox, deterministic boundaries, failure containment or prohibition on
serializing live Lua state: coroutines are **run, never persisted**.

## Considered options

- **Keep callbacks and named timers.** Rejected because sequential work requires
  hand-written state and timer bookkeeping spread across unrelated callbacks.
- **Accept both contracts indefinitely.** Rejected in favour of a hard v2 → v3
  cut: callback-table factories and v1/v2 imports fail preflight; bundled and test
  behaviours migrate with the runtime. There is one waiting vocabulary.
- **Store the closure on the Agent-type instance or merge the Lua states.**
  Rejected because Agent types and behaviours deliberately occupy separate
  per-World runtimes. The behaviour adapter owns the host-mediated Installed
  behaviour association; this does not introduce the cross-script method API
  deferred by [ADR 0019](0019-script-backed-agent-types-with-live-instances-and-frozen-baselines.md).
- **Poll every tick or serialize suspended stacks.** Rejected: semantic events
  and tick-based sleeps suffice; opaque Lua execution state is not authored data.

## Consequences

- Each assigned Agent has an independent module environment, factory closure and
  coroutine. Unassigned or completed work installs a host-recognized default
  no-op that is never resumed. Returning retains the authored assignment without
  restarting the work, applies final-resume commands and leaves already-issued
  movement running (fire-and-forget). Successor assignment remains host-side,
  authored and paused-only.
- `wait()` returns the next immutable semantic event. `sleep(ticks)` accepts a
  positive whole-tick duration and returns no values when it expires. Events
  cannot interrupt sleep; a bounded per-instance queue retains them for later
  waits in event-sequence order. Named timers and callback tables are removed.
- Resumes run synchronously in stable Agent-ID/event order at simulation
  boundaries. Successful staged commands apply through the World facade after
  all resumes, before intent collection. At most one movement intent is allowed
  per resume; commands do not bypass Mobility or traversal admission constraints
  from [ADR 0011](0011-enforce-mobility-profiles-as-routing-constraints.md).
- The per-World memory budget, per-execution instruction budget and command cap
  remain; boundary resumes and pending events are bounded. Errors or budget/queue
  exhaustion publish structured diagnostics, tear down the instance, restore the
  default and pause interactive simulation before the next tick; headless runs
  fail. Lua and sol2 remain confined behind the runtime adapter.
- Pause preserves execution state. Deactivation freezes remaining sleep, clears
  pending events and permits no resumes or accumulation; reactivation delivers
  `activated` without restarting the coroutine or interrupting remaining sleep.
- Teardown uses Lua 5.4 coroutine-close semantics for scoped `__close` cleanup,
  under an instruction budget, rather than `on_stop`. It does not resume the
  body; cleanup failure cannot retain an instance or veto a lifecycle operation.
  Reset, load, reload and deleted-Agent restoration start fresh closures from
  authored configuration. Only authored assignment/configuration persists.
- `behaviour` is reserved on `.agent.lua` instances and rejected in preflight and
  construction. The reservation adds no Agent-type API version bump, type-provided
  default behaviour or access to type-instance internals from behaviours.

Implemented by #526–#532 under #525; this decision and the
[Host API v3 authoring guide](../agent-behaviour-packages.md) are recorded by #533.
