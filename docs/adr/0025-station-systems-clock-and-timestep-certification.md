# Give Station systems an independent clock and certify timestep claims

Status: accepted

Station systems evolve on seconds-to-hours timescales, while World simulation
must retain its 60 Hz tick for Agent movement, traversal coordination, device
timing, behaviours, and rendering. Sharing that tick made long Station runs
unnecessarily expensive and hid which results depended on integration duration.

## Decision

`StationSystemsRuntime` owns a count-derived Station clock. The authored
`systemsStepWorldTicks` is an integer in `[1, 60]`: nominal Station frequencies
from 60 Hz to 1 Hz with the unchanged 60 Hz World. New settings default to `6`
(10 Hz); documents without the field retain legacy ratio 1. The earlier maximum
of 600 is withdrawn: values above 60 now fail validation rather than silently
clamping. The allowed frequencies are integer multiples of the World quantum,
not arbitrary real-valued durations.

The runtime is driven with `advanceTo(simulatedSeconds)` and takes whole steps
until its committed time reaches/exceeds the target. It does not accumulate a
floating-point elapsed duration. A target can leave Station ahead of World by
less than one Station step. Legacy `worldTick` and `completedTicks` count Station
steps, not World ticks. Snapshot `systemsStepWorldTicks` and
`systemsTimestepSeconds` expose the effective clock, including a runner override.

Physical comparisons must use the same authored configuration and **matching
committed elapsed instants**. Compare inventories, cumulative exchanges and
service outcomes, not equal step counts or raw last-step amounts. A last-step
quantity must be interpreted over its actual Station duration. Re-reading an
unchanged snapshot must not count its last-step dose again.

Piecewise-constant solar, load, crew, crop and algae profiles split integration
at their authored transitions, including period wrap. Every positive interval
samples inside that interval, not at its right endpoint. Supply and demand use
the same timeline; independently averaging them could invent usable power when
their active windows do not overlap. The host and bundled Lua mirror both split
profile forcing. This is event-driven subdivision, not a hidden 60 Hz integrator.
An unrepresentable profile occurrence index or over 65536 profile transitions
in one Station step refuses the step transactionally rather than looping forever.

## Evidence and remaining work

The invariance registry is a **coverage inventory**, not blanket certification.
Mixed aggregates remain `Unverified` with no invented tolerance or convergence
order. Step counts and clock metadata are `StepDependent`. Verified leaf entries
name the actual comparator and its fixture scope. `verifiedThroughRatio` records
that tested scope; it does not populate the global `certifiedRatio`. Conservation audit tolerances
are unchanged. Nonlinear feedback needs its own physical convergence evidence;
passing a constant-transfer fixture is not evidence for biological or thermal
fidelity.

Fast CTest lanes compare a ratio-1 reference against the requested candidate at
ratios 1 and 6. Extended lanes sample ratios 7, 30 and 60, including a non-divisor
and the coarse bound, and verify profile-integrated material movement against
an analytical answer. The supported range `[1,60]` is a validation bound, not a
requirement to enumerate every value. Component repeats sample 1, 7 and 60;
ordinary registrations already exercise the production default. The profile test also checks non-overlapping solar
supply and load demand cannot perform work. Each comparison checks its effective
runtime clock so the CLI override cannot collapse a matrix to repeated 60 Hz runs.
Without `--step-ratio`, the runner respects authored settings; it no longer
forces ratio 1. Explicit-ratio tests restore the caller's override on every
exit path.

The admitted-work **protection** certification remains ratio 6, as recorded in
`docs/tickets/771-rate-aware-protection.md`. That bound is not a statement that
undersized equipment meets its service limits, nor proof of all-observable
convergence. A separate measured performance case compares empty runtimes at
ratios 1 and 6; it is not a full-station performance or fidelity result.

The analytical generator gate exposed 30.3333, 30.0 and 20.0 kJ at ratios 1, 6 and
60 instead of 30.4 kJ. Host and Lua now integrate startup and affine ramp work on
physical time; all three produce 30.4 kJ. Representative gates cover ratios
1, 6, 7, 30 and 60, delayed solar and generator-funded cranking. Manual generator/solar-only fleets
split at crank power-qualification crossings so averaged power cannot invent an
early start. Endpoint output is distinct from mean delivered output; dispatch
uses the latter to reserve backup capacity. These are scoped analytical fixture
results, not proof for arbitrary coupled configurations. See the
[verification checkpoint](../station-timestep-verification.md).

Remaining work includes complete timestep-aware migration of legacy assertions
and convergence verification for fuel depletion/decay, recovery controllers and
coupled electrical, thermal, atmospheric and biological workloads. Until those pass, do not advertise the whole Station test
suite or all physical observables as timestep-independent.
