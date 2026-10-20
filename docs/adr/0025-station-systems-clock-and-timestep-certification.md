# Give Station systems an independent clock and certify timestep claims

Status: accepted

Station systems evolve on seconds-to-hours timescales, while World simulation
must retain its 60 Hz tick for Agent movement, traversal coordination, device
timing, behaviours, and rendering. Sharing that tick made long Station runs
unnecessarily expensive and hid which reported results depended on the chosen
integration interval.

## Decision

`StationSystemsRuntime` owns a count-derived Station clock. Its authored
`systemsStepWorldTicks` ratio is an integer in `[1, 600]`, defaults to `1`, and
multiplies the embedding base quantum to derive one Station-step duration. The
runtime is driven with `advanceTo(simulatedSeconds)` and performs whole
catch-up steps; it never accumulates a floating-point elapsed duration. The
SimulationCoordinator still calls that seam once at each World tick boundary,
but the runtime does not read the World timestep and standalone tests can drive
Station systems without a World.

Published Station observables are classified in the production invariance
registry as Exact, Tolerance (with declared convergence order), or Bound. The
cross-step test lanes compare the ratio-1 reference with ratios 6, 30, and 600;
ratio 1 and the certified ratio 6 are PR-gated, while the extremes and
long-duration reference are exhaustive. A measured performance check reports
wall-clock time per simulated hour at ratios 1 and 6 and rejects a coarse run
that is slower per simulated second.

The certified ratio is **6**. Its certified guarantee is specifically the
rate-aware admitted-work protection margin: predicted gross excursions are
considered before a coarse step commits. It is not a clamp on physical
environmental state and does not assert that undersized equipment meets its
service limits. Ratios above 6 remain legal, deterministic, finite, and
conserving, but are **uncertified**: authors must not claim the certified
protection bound for their results.

## Consequences

- World and Station time remain separate: Agent-side timing stays owned by the
  World tick and Station systems uses its own clock.
- Event-split integration and saturation-crossing resolution preserve exact
  scheduled occurrences and bounded inventories at coarse ratios.
- New public snapshot observables require an invariance classification and a
  named verification owner before release.
- The certification is evidence-backed rather than a blanket fidelity claim;
  its current protection measurements are recorded in
  `docs/tickets/771-rate-aware-protection.md`.
