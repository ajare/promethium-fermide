# Station Monitors: oxygen tracer bullet (#866)

The first registered Monitor subject/Metric pair is `atmosphere` /
`oxygenPartialPressureKPa`. Other subjects and Metrics, default sets, dashboard
and editor controls belong to the later #864 slices. A Monitor observes only:
it cannot change supplies, operate a hatch, trip equipment, or create Faults.

## Authoring

Station configuration schema 122 adds this optional `stationSystems` collection:

```yaml
monitors:
  - id: room-oxygen-low
    subjectType: atmosphere
    subjectId: room
    metric: oxygenPartialPressureKPa
    rule:
      threshold: 17.5
      direction: Below
      triggerOnSeconds: 30
      triggerOffSeconds: 60
    severity: Critical
    enabled: true
```

`subjectId` must name an existing atmosphere compartment. IDs must be non-empty
and unique; thresholds must be finite; both times must be finite and non-negative.
Directions are `Above` and `Below`; severities are `Advisory`, `Warning` and
`Critical`. `enabled` defaults to false. Old documents without Monitors retain
their existing wire schema. YAML and binary save only the authored rules, not
Alert history or persistence timers.

## Evaluation and commands

Each successful Station step evaluates its published snapshot once. Strict
inequality is outside; equality is inside. Each sample represents that completed
step's duration. Fire/clear occurs at the first sample completing the required
continuous duration, without interpolation or a value deadband. A contrary
sample resets that pending duration. Zero times fire/clear at the first matching
sample, never just from querying the snapshot.

Use `StationSystemsRuntime::updateSettings` (headless) or
`World::updateStationSystemsSettings` (World facade) with a changed per-Monitor
`enabled` flag. Validation happens before any mutation. Enabling starts fresh
with the next Station step, with no credit for previous disabled time. Disabling
immediately closes an active Alert at current Station time with
`MonitorDisabled`, and drops pending timers. A changed rule/subject/severity
starts fresh too, closing an active Alert with `MonitorChanged`; removing a
Monitor closes it with `MonitorDisabled`.

`StationSystemsSnapshot::activeAlerts` and `closedAlerts` publish Monitor identity,
subject, Metric, latest observed value, threshold, severity and `firedAtSeconds`.
Closed entries also carry `clearedAtSeconds` and `clearReason` (`AutoClear`,
`MonitorDisabled`, or `MonitorChanged`). Code-bound callbacks record the ledger
and fire/clear announcements in the bounded `alert` event stream. The ledger is
run-scoped; reset/load starts fresh, including when physical Fault state is
restored by replay.

## Headless demonstration

Run the focused Release scenario:

```sh
ctest --test-dir build-linux -C Release -j 16 -R '^station-monitorDemo$' --output-on-failure
```

The scenario authors a room at 100 m³, 20 °C with 20 kg O₂ and 80 kg N₂
(pO₂ about 15.2 kPa), the rule above, and a closed 25 m² pathway to a second
100 m³ compartment with 80 kg O₂. It demonstrates no Alert before 30 sim-seconds,
then a Critical Alert at 30 seconds. A validated live settings command opens the
pathway; real gas diffusion replenishes room oxygen. After 60 consecutive
sim-seconds of recovered samples the Alert auto-clears. The test asserts the
actual published pO₂ and ledger, not callback internals.
