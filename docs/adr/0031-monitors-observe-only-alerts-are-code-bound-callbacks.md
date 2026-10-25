# Monitors observe only; Alerts are code-bound callbacks, never authored actions

The station already carries three overlapping "something is wrong" nouns — Broken (traversal devices), Fault/Failure (adverse installation state), Trip (a machine's own protective isolation) — plus scattered internal alarm scalars in the tick code. We needed a user-facing oversight layer without blurring those boundaries. We decided a **Monitor** is a purely read-only authored rule (a named Metric + an Alert Rule + an enabled flag, disabled by default) that never actuates: it cannot isolate, shut down, curtail, or create a Fault. When a rule matches, the Monitor creates an **Alert**, and an Alert is implemented in code as a bound callback function per alert type — so what an Alert *does* (dashboard entry, event-stream announcement, future integration) is an implementation decision, never an authored action. Authoring stays the Rule alone: threshold with direction, trigger-on time, trigger-off time.

## Considered options

- **Alerts as authored actions/hooks** (user wires what fires): rejected — reopens the Monitor/Trip boundary, makes authored data executable, and contradicts the purely-observational intent.
- **Reuse the existing alarm scalars**: rejected — they are audit-internal, untyped, and not user-authorable; they stay as-is, with a recorded follow-up to migrate/unify them into Monitors.

## Consequences

- Four distinct nouns coexist with hard boundaries: *alarm* (internal scalar), *Fault* (state), *Trip* (self-protection), *Alert* (observation result). Code review should refuse cross-use.
- Because disabled Monitors skip evaluation, enabling one mid-run monitors from that tick forward with no backfill — consistent with the "commands take effect at the time they name" rule.
- Alert handling flexibility lives entirely in the callback registry; adding a new alert behavior never changes the authored schema.
