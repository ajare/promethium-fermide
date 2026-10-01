# Local simulation metrics

Metrics are opt-in. The endpoint binds **only to 127.0.0.1**, with no remote-bind
option, authentication, TLS, CORS, or mutation endpoints. Do not forward this port
to an untrusted network. No metrics history is written to disk.

## Run

```sh
prometheum-fermide-headless --metrics --metrics-port 9464
curl http://127.0.0.1:9464/metrics
curl http://127.0.0.1:9464/healthz
```

The headless metrics mode runs until SIGINT/SIGTERM. Without arguments, the normal
smoke suite still runs and exits. The default metrics World contains one empty
Room. To observe an authored document (including its Agent behaviours):

```sh
prometheum-fermide-headless --metrics --metrics-world resources/test-worlds/lift-test-1.world.yaml
```

In the GUI, use **View → Metrics** to toggle the endpoint and edit its port.
These preferences are saved in `imgui.ini`. `editor --metrics-port 9464` overrides
the saved port and enables metrics. The endpoint follows the current World;
changing Worlds starts a fresh collector. A port conflict prints a diagnostic
without stopping simulation. Toggle off/on or change the port to retry.

```yaml
scrape_configs:
  - job_name: fermide
    static_configs:
      - targets: ['127.0.0.1:9464']
```

`GET /metrics` returns `text/plain; version=0.0.4`; `GET /healthz` returns `ok`.
Unknown paths return 404 and non-GET methods return 405. Request bodies are not
accepted. Shutdown stops and joins the HTTP worker before destroying its collector.

## Series and cardinality

All families start with `pf_`. Counters end in `_total`; durations are seconds.
Histograms use boundaries 0.1, 0.5, 1, 2.5, 5, 10, 30, 60, 300, and +Inf.
HELP/TYPE lines precede samples; families and label sets have deterministic order.

Resource-scoped metrics always carry `resource`, `resource_id`, and
`resource_type` labels, so queues, state, occupancy, capacity, utilization, and
transition counters remain separate for each authored resource even when two
resources have the same display name. Bounded child identities such as Lift Stops,
Shuttle carriages, and access zones are also always labelled. Physical Lift
positions and Stop indexes are therefore available in default mode. No label ever
contains an Agent identity.

`--metrics-detail=sector,queue` additionally enables named Sector, interaction
point, and queue-position detail in either executable. The registry caps
exposition at 4096 samples (including histogram expansion). New series beyond the
cap are omitted. Existing counters remain cumulative. Detail mode is intended for
small Worlds; default cardinality grows with authored resources and their bounded
Stops, carriages, and access zones, but never with the number of Agents.

Implemented families cover:

- simulation ticks, current tick, pause, phases, Agent counts and build info;
- Agent states, activation/lifecycle, route loss, interactions, committed
  traversals, queue waits, traversal durations and transport journey durations;
- Sector counts/occupancy;
- Door state, opening ratio, leases, presence/obstruction, activation, queues,
  crossings, state transitions and open durations; Window traversability;
- Lift/platform lift phases, direction, occupants, capacity, admission queues,
  boardings/alightings, trips, served stops, dwell, request owners and utilization;
- Shuttle carriage occupants/capacities/utilization, boardings/alightings and
  access-zone queues;
- Ladder capacity/occupancy, admission reservations/queues, direction, waiting,
  batching, admissions and direction reversals; Stairwell occupancy/capacity;
- Staircase/Escalator committed crossings;
- extensible-resource state, leases, pending retraction and extension duration;
- traversal request states/transitions/denials, active permits, expiry and queue,
  crossing-lane and capacity-position occupancy;
- interaction requests and device operations/commands/transitions.

Families with no observations may be absent. Counters start at attachment, not
World creation; durations that started before attachment are not fabricated.
Deleted resources lose their gauges; their counters remain until collector reset.
Observations are tick-resolution: intermediate changes that leave no event and
return to the same snapshot state within one tick cannot be reconstructed.

### Snapshot limitations

Bulkhead Doors currently share the `door` resource type: the resource snapshot
does not identify their subtype. Stairwell directional capacity is not exposed
by the snapshot and is omitted rather than guessed. Timing observations start
when the collector first sees the corresponding transition. External clients
using the collector should call `refresh()` when changing paused/editing state
without advancing ticks; the GUI does this while paused.

## Architecture and verification

`core::SimulationObserver` is a non-owning, optional World registration. Register
and unregister it on the simulation thread, before destroying the observer.
Callbacks run after tick publication, before the phase clears; they must be
`noexcept`, must not mutate the World, and must not consume its event queue.
The coordinator retains no observer state (ADR 0004).

`core::SimulationMetricsCollector` has no I/O or worker threads. It reads one
snapshot per observed tick. HTTP workers copy its value registry under a mutex,
then format outside that lock. The endpoint uses pinned MIT cpp-httplib in the
separate `pf-metrics` library. The collector also catches observation failures so
they cannot interrupt simulation.

`src/headless/smoke/metrics/Metrics.cpp` checks exposition escaping, non-finite
values, normalized family collisions, HTTP routing/content type, port conflicts,
concurrent scrapes, lifecycle counters, and tick sampling across a multi-tick
update through the standard smoke-module interface. The 500-Agent scale smoke
compares event/snapshot digests with and without a collector. Run
`ctest --test-dir build-linux -R '^smoke-(metrics|simulation)' --output-on-failure`.
