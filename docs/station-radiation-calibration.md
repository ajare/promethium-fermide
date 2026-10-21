# Radiator SI calibration correction (#783)

## Correct units

Host and bundled Lua now use the SI Stefan–Boltzmann constant
`5.670374419e-8 W/(m² K⁴) / 1000 = 5.670374419e-11 kW/(m² K⁴)`.
The former `5.670374419e-14` value was 1000× too small for a kW rate.

The physical path is:

1. Convert authored Celsius temperatures to kelvin with `+273.15`.
2. Evaluate `emissivity * viewFactor * areaM2 * sigmaKW * (T⁴ - T_sink⁴)`.
3. Apply existing physical connection limits/losses and the finite sink floor.
4. Integrate the resulting kW rate over seconds to obtain kJ; divide the net
   heat by reservoir kJ/K capacity to update temperature.

Negative net rejection represents absorption from a warmer environment. There
is no additional W/kW or J/kJ conversion in the integration path. Conservation
tolerances, thermal service limits, connection ratings and sandbox budgets are
unchanged.

## Existing documents and explicit reference recalibration

**User-authored areas remain square metres, unchanged on load/save/reset.**
There is no schema bump, automatic area migration, coefficient compatibility
mode or hidden scale factor. An unchanged radiator now has 1000× its former
unconstrained radiative potential at the same temperatures. Connection limits
or the finite available heat can still limit actual exchange. Existing Worlds
may therefore cool/warm faster or change service availability when rerun.

If an area was calibrated to reproduce the old incorrect model rather than a
physical surface, explicitly dividing that area by 1000 preserves the old
`sigma * area` gain. This is an authoring decision, not an automatic loader rule.
Custom lifecycle scripts must likewise use correctly converted physical units.

The bundled reference is a provisional small-load demonstration, not a fixed
engineering surface specification. Its areas were explicitly recalibrated:

| Reference radiator | Previous area | Corrected area |
|---|---:|---:|
| `radProcess` | 6000 m² | **6 m²** |
| `radGen` | 500 m² | **0.5 m²** |

This retains its prior thermal balance and scenario purpose under the corrected
coefficient. The YAML provenance records the change, and `referenceBundled`
asserts the explicit values. The normal scenario, reset/document replay and
shortage/rejection scenario pass with the new data.

The nonlinear physical-fidelity fixture similarly changes its area from
1,000,000 to **1000 m²** to preserve its deliberately chosen thermal timescale.
Its continuous ODE oracle and truncation bound now use an independent SI
coefficient, not the production constant. The previously recorded temperature
errors and bounds remain unchanged and were revalidated. The thermal file-replay
fixture was likewise explicitly resized from 5000 to 5 m² to retain its intended
gently warming scenario; its existing state and persistence assertions remain
unchanged.

## Regression and validation

`station-fidelity-radiatorSI` covers ratios **1, 6, 7, 30, 60**, both exchange
directions, and black/grey emissivity/view factors. For a 1 m² blackbody at
300 K facing 100 K, its independent oracle is:

`5.670374419e-8 * (300⁴ - 100⁴) / 1000 = 0.45362995352 kW`.

The original code reproducibly reported `0.00045362995352 kW`. The check also
asserts kJ integration, reservoir temperature and unchanged authored area.
Twenty cases exercise the ordinary host/runtime path, then twenty exercise an
independent assertion injected around the bundled Lua model. A deliberate Lua
coefficient mutation back to the old value must fail with the SI diagnostic
and leave committed elapsed time unchanged; a closed internal energy ledger
alone is not accepted as calibration evidence.

Incremental Release builds of `pf-smoke-station` and `pf-smoke-editor` succeeded.
**19/19 targeted checks passed in 43.83 s**, run with CTest `-j 16`: SI and coupled
fidelity, thermal/coolant/chiller/buffer behavior, thermal observations and
connections, material observations, protection and gross excursions, thermal
seams, reference scenarios, and radiator/reservoir editor workflows. Existing
`flowRadiatorWorkflow` now has its own CTest registration.

A further three lint/metadata checks and the standalone configuration and
thermal file-replay checks passed: **24 distinct focused checks have recorded
latest passes**, not a full-suite aggregate. The old warming fixture initially
failed under the stronger physical radiator; its explicit area recalibration
resolved the failure without weakening its assertions.

Evidence: `/tmp/pf-783-red.log`, `/tmp/pf-783-affected.log`,
`/tmp/pf-783-contracts.log`, `/tmp/pf-783-files.log`,
`/tmp/pf-783-files-final.log`.
Reproduce the focused regression with:

```sh
ctest --test-dir build-linux -C Release -j 16 --output-on-failure \
  -R '^station-fidelity-radiatorSI$'
```

This validates the radiation unit conversion, not all Station physical
coefficients or universal timestep independence. The earlier workload benchmark
is retained as historical measurement, not relabelled as a fresh #783 run.
