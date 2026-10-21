# Diagnosis of the 10 Hz Station suite failures

## Historical diagnosis — not a current fidelity claim

This records the original first failures, not proof that every remaining failure
was test-only. Subsequent physical comparisons found **another production bug**:
right-endpoint profile sampling changes integrated work with the Station step.
Solar/load/crew/crop/algae profile forcing now splits at authored transitions
in both the host and Lua mirror. Analytical material-movement and non-overlapping
solar/demand checks cover every supported ratio 1–60. The former broad invariance
registry claims were not backed by complete comparators; mixed aggregates are
now explicitly unverified. Legacy assertion migration and coupled-physics
convergence remain open. Read the counts/classification below as historical.

## Scope and result

This diagnoses all **71 failed CTest entries** from the 246-entry Release Station
run recorded in [station-test-timings.md](station-test-timings.md), with Station
runner ratio 6 and World at 60 Hz. `smoke-station` and `station-lifecycle` are two
entries exercising the same check, not two independent defects.

**Two confirmed production defects account for four failing entries**:
starvation detection (one) and inconsistent history stride metadata (three).
**The other 67 recorded failures need test/fixture/harness updates**, including
four checks whose ratio-1 expectations depend on sampled profiles. That does
not prove all assertions currently hidden behind the first failure will pass.
The long-run stability check demonstrably has both a runtime issue and a later
outdated counter assertion.

The initial diagnosis included no production fixes or test relaxations; all
temporary probes were restored. The two runtime defects have since been fixed
as described below. The earlier 10 Hz default changes are preserved.

## Runtime repair status

Both confirmed defects are fixed without loosening the conservation or
stability assertions:

- Host and bundled Lua stress detection recognize roundoff-sized drawable feed
  and medium. The threshold scales with native-unit stock, protected reserve
  and one-step demand using double machine epsilon; it is not an absolute
  inventory cutoff. Residual stock is not clamped, deleted, or made available
  through a reserve, and exhausted stock cannot fund biological healing.
- One integer observation stride now controls both actual sampling and the
  recorder's double-precision cadence product. Coarsened metadata and the
  retained timestamps therefore use the same clock arithmetic.

New `microbialSupplyRoundoff` and `historyCadencePrecision` checks own explicit
ratio-1 and ratio-6 CTest registrations. All four failed before the fixes and
pass afterwards. They exercise host/Lua exhaustion, kg/g units, reserves,
meaningful-stock positive controls, preserved residual inventory, funded
recovery, multiple sampling intervals, coarsening and the unchanged audit.

Release validation (`-j 16`): the four new regressions passed, the original
ratio-6 microbial-condition/path-equivalence/reset-isolation checks passed
(3/3), and nearby ratio-1 bioconversion/conservation/stability and timing-lint
checks passed (14/14). Logs: `/tmp/pf-runtime-fixes-originals.log` and
`/tmp/pf-runtime-fixes-nearby.log`. Test-only migration remains separate; this
is not a claim that the full ratio-6 suite is green.

## Clock contract used to assess correctness

`StationSystemsRuntime::advanceTo(T)` commits whole steps until Station elapsed
seconds are >= T. Thus a snapshot may be ahead of World time by less than one
Station step. At ratio 6, legacy `worldTick` and `completedTicks` count Station
steps, not World ticks. Per-step amounts remain doses for a Station step;
telemetry rates divide by the Station duration. Authored scheduled occurrences
are evaluated at Station time. These are the current production and ADR 0025
contracts, not assumptions introduced to excuse the failures.

The runner's `--step-ratio` override applies to every runtime constructor,
including fixtures that explicitly request another ratio. Editor checks do not
use that override; their failures were caused by the new authored default 6.

## Confirmed production defects

### B1 — microscopic feed residues suppress microbial starvation

`station-microbialConditionRecovery` starts with 0.003 kg feed, processes at
0.001 kg/s, and expects condition degradation after exhaustion. At ratio 6:

| Committed step | Feed before processing (kg) | Starving |
| --- | ---: | --- |
| 30 | 9.9999848753212504e-5 | false |
| 31 | 1.3552527156068805e-20 | false |
| 32 | 3.009265538105056e-36 | false |
| 40 | 1.778206999588062e-161 | false |
| 50 | 5.1806537865363094e-318 | false |

The uniform scaled draw leaves a positive floating-point tail. The stress gate
in `StationTickProcessing.cpp` tests drawable feed <=0, so the culture stays at
condition **1.0**, with empty stressors and no recovery, at Station time
5.000000260770321 s. The stock finally underflows to zero on that last step,
after its stress decision. The original ratio-1 check passes. This is a real
model defect: negligible stock postpones starvation by many steps; it is not
nitrogen creation, a missing heater, or a tolerance issue in the assertion.

A fix must reconcile actual feed exhaustion/admission with condition stress,
without deleting material to force a clamp or fabricating recovery. Regression
coverage should include finite positive tails and protected reserves at both
ratios. The test's starvation assertion should remain meaningful.

### B2 — history stride rounded differently from actual timestamps

`StationSystems.cpp:1004-1007` converts the double Station duration to float,
then computes the recorder cadence in float. Timestamp products stay double.
After history coarsening, a reported 16-second stride becomes:

- Published stride: **16.000001907348633 s**.
- Actual consecutive timestamp gap: **16.000000834465027 s**.

This is a **1.072883606e-6 s metadata discrepancy**, sufficient to violate the
existing audit at early retained samples. No negative inventory, energy drift,
or unstable thermal oscillation was reported by this failure.

A minimal standalone enabled runtime, ratio 6, observation interval 1 s,
retention 10000 s, and `advanceTo(2100)` reproduces it in **0.85 s**, without a
World or the reference equipment. Temporarily changing only the recorder's
cadence product to use the double Station duration makes that probe pass,
with published stride and actual gap both 16.000000834465027 s.

With that same temporary metadata correction:

- `station-stabilityPathEquivalence` **passes**, including all update paths.
- `station-stabilityResetIsolation` **passes**.
- `station-stabilityLongRun` gets past the clean audit, then fails its separate
  obsolete check `completedTicks == World ticks` (`Stability.cpp:310-311`).

The production fix should derive sample cadence once from an integer number of
Station steps and share its double product between metadata and timestamps.
Widening the stability tolerance would hide the wrong metadata.

## Important false alarms and counterprobes

- **Nitrogen residual -2.321739:** `StationBiologyChecks.cpp` adds
  `lastFeedConsumedKg` in a loop advancing one World tick at a time. Five out of
  six iterations can see the same committed Station step. Temporarily skipping
  unchanged Station stamps makes the **entire nutrientConservation check pass**
  at ratio 6, without changing production physics or its conservation audits.
- **Clock partial-step error / sparse-reporting error:** clearing the global
  runner override for the fixed-ratio matrix, and explicitly authoring the fine
  reporting reference as ratio 1, makes both checks pass. Production was running
  ratio 6 while the tests calculated expected results for 1 or 600.
- **Hot/cold process refusal:** coarse mode deliberately uses the reason
  `current or predicted pre-step temperature is outside the qualified operating window`.
  Probes confirmed rejected nutrient processing with eligibility false,
  feed=0 and power=0, and rejected carbon processing with cooling false,
  CO2 consumed=0 and power=0. The failing assertions require old substrings.
- **Recovery surge:** the observed coarse sample is normal=4 kW and
  surge=0.999999947845939 kW, within the 5 kW pool. The 0.05-second surge is
  prorated inside the 0.1-second step; the test assumes a full 2 kW for the
  enclosing step. Compare funded energy and remaining pool, not a hardcoded
  normal-service ceiling of 3 kW.
- **Endurance:** the measured net rate is -2.53 kg/s, not the test's fine-grid
  -2.505 kg/s, because the existing endpoint-sampled profile has different
  boundary weights at ratio 6. This is a declared sampled-model sensitivity,
  not evidence that reading the forecast advances simulation. Test matching
  intervals and integration convention, and retain a separate accuracy check.
  This diagnosis does **not** certify blanket all-observable convergence or
  justify widening an existing cross-step accuracy tolerance.

## Exhaustive failure inventory

Source anchors below are relative to `src/headless/smoke/station/` unless they
start with `core/` (relative to `src/`) or `editor/` (relative to
`src/headless/smoke/`). The grouping records the cause of the **observed first
failure** for every entry; later assertions may have additional issues.

### Test: step-count assumptions (9 entries)

Snapshot worldTick/completedTicks are Station steps, not World ticks; compare elapsed seconds and unchanged reads instead.

- `smoke-station`
- `station-lifecycle`
- `station-trends`
- `station-fileRoundTrip`
- `station-writeFailure`
- `station-structuralReplay`
- `station-forecast`
- `station-coupledNominal`
- `station-referenceNominal`

Evidence: `StationCoreChecks.cpp:103; StationTrendChecks.cpp:325; Documents.cpp:545,625,729; StationForecastChecks.cpp:162; Coupled.cpp:151; Reference.cpp:182`.

### Test: mismatched observation instant (5 entries)

Assertions use World time while the committed Station snapshot is up to one coarse step ahead. Events/counts must be checked at the snapshot elapsedSeconds or at common Station boundaries; the reported conservation assertion also bundles a stale expected occurrence count.

- `station-scheduledTransfers`
- `station-recurringTransfers`
- `station-scheduledPantry`
- `station-deliveries`
- `station-wasteEjections`

Evidence: `ScheduledTransfers.cpp:54-67,379-385,444-452; StationLogisticsChecks.cpp:119-127,531-540`.

### Test: wrong amount duration (18 entries)

Expected per-step amounts/energy are computed using 1/60 s or literal ratio-1 amounts, while the committed Station step is 6/60 s. Derive dose from the actual Station duration; keep rate and cumulative-total assertions distinct.

- `station-connectionTransfers`
- `station-connectionConversions`
- `station-connectionThermal`
- `station-connectionPower`
- `station-connectionBlockedZeroEfficiency`
- `station-connectionBatteryTelemetry`
- `station-transfers`
- `station-compounds`
- `station-thermal`
- `station-chillers`
- `station-buffers`
- `station-crews`
- `station-waterContamination`
- `station-compoundWaterLegacy`
- `station-containment`
- `station-fuelCells`
- `station-bioconversionTracer`
- `station-bioconversionCarryover`

Evidence: `Connections.cpp:106,150,204,261,299; StationCoreChecks.cpp:420; StationCompoundPowerChecks.cpp:202; StationThermalChecks.cpp:201; StationGenerationChecks.cpp:483; StationLogisticsChecks.cpp:1151; StationWaterChecks.cpp:544,1857; StationAtmosphereChecks.cpp:1011; CompoundWater.cpp:766; Bioconversion.cpp:83,149`.

### Test: repeated committed dose (4 entries)

A loop samples every World tick and reuses the last Station dose/rate without detecting whether a new Station step committed. Compare successive committed snapshots once, using their elapsed delta; do not accumulate the same dose six times.

- `station-nutrientConservation`
- `station-compoundWaterPartial`
- `station-coolant`
- `station-atmospheres`

Evidence: `StationBiologyChecks.cpp:649-674; CompoundWater.cpp:151-170; StationThermalChecks.cpp:929-941; StationAtmosphereChecks.cpp:659-667`.

### Test: wrong rate divisor (9 entries)

The runtime correctly reports per-second telemetry using the Station duration; tests divide snapshot per-step amounts by the World duration and expect a sixfold larger rate.

- `station-algaeExchangeObservations`
- `station-microbialExchangeObservations`
- `station-cropExchangeObservations`
- `station-carbonExchangeObservations`
- `station-waterGasExchangeObservations`
- `station-crewAtmosphereExchangeObservations`
- `station-editor-microbial-observations`
- `station-editor-carbon-observations`
- `station-editor-water-gas-observations`

Evidence: `MaterialObservations.cpp (algae/crop/microbial/carbon/water-gas/crew verifiers); editor/StationSystems.cpp:3807-3829,3894-3910,3961-3984`.

### Test harness: ratio override (2 entries)

The runner silently replaces explicitly authored ratios. The clock test thinks it is testing ratio 1/30/600 while actually running 6; the reporting sparse fixture thinks it is 600 while actually 6. Give fixed-ratio comparisons scoped control of the override; explicitly author the fine reference as 1 after the default change.

- `station-clock`
- `station-reportingSystemsTime`

Evidence: `Main.cpp; core/StationSystems.cpp:877; StationClockChecks.cpp:18,47-50; StationTrendChecks.cpp:43,114-119`.

### Test: stale legacy fixture generation (3 entries)

Tests lower the serialized schema but strip only the literal systemsStepWorldTicks: 1. The new default serializes 6, leaving a new-schema field inside an old-schema fixture (correctly rejected), or expecting a now-invalid exact string. Construct genuinely old documents and explicitly expect legacy ratio 1.

- `station-materialIdentity`
- `station-wasteEjectionFiles`
- `station-editor-connection-design`

Evidence: `Documents.cpp:204,211,1293; editor/StationConnections.cpp:152-154; core/StationReference.cpp:2133-2144`.

### Test: hardcoded UI values (3 entries)

UI tests expect 60 completed steps after one second or per-step quantity strings 1.000/0.400. The correct coarse values are 10 steps or sixfold doses. Check the actual Station snapshot and duration rather than ratio-1 literals.

- `station-editor-window`
- `station-editor-transfers`
- `station-editor-compounds`

Evidence: `editor/StationSystems.cpp:5017-5020,5130,5292-5295`.

### Test: coarse refusal wording (6 entries)

Coarse protection deliberately returns the generic current-or-predicted temperature-window reason, not the ratio-1 cooling/dormancy substring. Physical refusals remain intact. Assert refusal, no work/feed/power, eligibility and the coarse reason; do not relax safety conditions. Preserving specific gate details would be a useful diagnostic improvement, not a demonstrated admission failure.

- `station-generators`
- `station-gas`
- `station-crops`
- `station-protectedServices`
- `station-nutrientRecovery`
- `station-carbonRecovery`

Evidence: `core/StationSystemsImpl.h:1711-1715; StationGenerationChecks.cpp:1067-1072; StationAtmosphereChecks.cpp:1745-1750; StationBiologyChecks.cpp:1339-1345; StationResilienceChecks.cpp:944-949; StationRecoveryChecks.cpp:655-660,1244-1249`.

### Test/model sampling: profile grid (4 entries)

Fixtures assume ratio-1 endpoint sampling. At ratio 6, rapid flicker aliases, a pre-boundary World instant can expose the later Station endpoint, and profile-weighted totals depend on the sample grid. Use meaningful coarse fixtures and matching simulated instants; keep a separate cross-step accuracy test rather than widening tolerances.

- `station-eventBound`
- `station-power`
- `station-batteriesEclipse`
- `station-endurance`

Evidence: `StationCoreChecks.cpp:141-156; StationCompoundPowerChecks.cpp:850-856; StationBatteryChecks.cpp:230-237; StationTrendChecks.cpp:1025-1030`.

### Test: startup phase assumption (1 entries)

The first coarse step changes Stopped to Starting but does not yet crank. At 0.1 s the fixture expects the cranking draw seen after six fine steps. Observe/drive an explicit startup phase and separately verify the energy/time contract.

- `station-electricalExchangeObservations`

Evidence: `MaterialObservations.cpp:913,932-937; core/StationTickGeneration.cpp:924-956`.

### Test: embedded Lua step literals (2 entries)

Injected Lua still interprets its step argument as 60-Hz World ticks: failures are delayed until step 61, and an outage assertion checks steps 300..900 instead of elapsed seconds 5..15. Update the test scripts, not production failure containment or scheduling.

- `station-failures`
- `station-outages`

Evidence: `StationOperationsChecks.cpp:33-40,1451-1459`.

### Test: prorated surge (1 entries)

A 0.05 s, 2 kW surge inside a 0.1 s step is correctly reported as about 1 kW average. Normal service 4 kW plus the average surge stays within the 5 kW pool; the test incorrectly requires normal <=3 as if the full 2 kW surge occupied the whole step.

- `station-recovery`

Evidence: `StationResilienceChecks.cpp:2034-2059; core/StationTickProtection.cpp:384-465`.

### Runtime bug: starvation detection (1 entries)

Microscopic positive feed residues prevent the exact-zero starvation gate from activating after effective exhaustion. Culture condition stays 1 rather than degrading; this is not an assertion to weaken.

- `station-microbialConditionRecovery`

Evidence: `core/StationTickProcessing.cpp:783-818 and scaled feed draw; StationRecoveryChecks.cpp:1526-1529`.

### Runtime bug: history stride precision (3 entries)

Recorder cadence is rounded via float while timestamps use double count-derived Station time. Downsampling amplifies the metadata discrepancy until the stability audit correctly rejects it. Use one integer observation stride multiplied by the same double Station duration. The long-run check also has a subsequent ratio-1 counter assertion to update.

- `station-stabilityLongRun`
- `station-stabilityPathEquivalence`
- `station-stabilityResetIsolation`

Evidence: `core/StationSystems.cpp:1004-1007; core/StationHistory.cpp:108-113; core/StationStability.cpp:152-165; station/Stability.cpp:304-311,391-393,441`.

## Evidence and reproduction

Original full run: `/tmp/pf-station-10hz-ctest/run.log` and `timing.json`.

Focused deterministic red loop (original code, no diagnostic edits):

```sh
ctest --test-dir /tmp/pf-station-10hz-ctest -C Release \
  -R '^station-(connectionTransfers|nutrientConservation|clock|scheduledTransfers|materialIdentity)$' \
  --output-on-failure -j 16
```

All five original failures reproduced in 0.39 s. Temporary probes used the
same Release executable through CTest, with `-j 16`; no submodule tests or
Debug runs were used. Their retained local output is under
`/tmp/pf-10hz-diagnosis/`:

- `probes.log`: corrected test-side accounting/override counterprobes and
  measured rejection, surge and endurance values.
- `starvation-detail.log`: positive-feed-tail trace and failure at ratio 6.
- `history-starvation.log`: minimal history probe before the metadata correction.
- `counterprobes.log`: minimal history probe after the one-expression
  correction and original starvation check passing at ratio 1.
- `stability-counterprobe.log`: path/reset checks passing after the metadata
  correction; long-run then exposing the separate World-step assertion.
- `restored-build.log` and `restored-reproduction.log`: rebuilt the original
  code after removing all probes; clock, nutrient-conservation and starvation
  failures reproduce again at ratio 6 (three checks in 0.31 s).

## Recommended repair order

1. Fix and regress starvation detection without concealing material loss.
2. Fix history cadence precision, leaving stability bounds intact.
3. Introduce scoped test ratio ownership and a Station-seconds scenario helper.
4. Migrate timing, dose/rate, repeated-snapshot and UI expectations using the
   public Station clock. Generate genuine legacy fixtures, retaining ratio-1
   compatibility tests explicitly.
5. Repair fast/exhaustive lane ownership and add meaningful coarse-profile
   and cross-step accuracy coverage; do not describe existing lane names as
   proof of full observable certification.
6. Re-run the full Release suite at both ratios. The previous 94.87-second run
   remains failure-shortened timing, not a passing performance baseline.
