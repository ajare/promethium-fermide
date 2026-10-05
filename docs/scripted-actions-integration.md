# Scripted-actions final integration (#467)

## Delivered contract

Furniture catalogues are native `.furniture.lua` modules only. The YAML parser,
optional closed-enum usable-point action and implicit arrival adapter are removed.
The file picker already selects `furniture.lua`; resource loading, basename
validation, attachment, World dependency resolution and reload now enforce that
contract. External YAML reports that conversion to Lua is required, with no
mutation, fallback parsing or silent rewrite. Renaming YAML to `.furniture.lua`
is not conversion. World YAML/binary persistence is unchanged.

Bundled catalogues/teaching Worlds (#465) and independent regression fixtures and
generated catalogues (#466) retain catalogue UUIDs, definition/usable-point keys
and Marker identities. Authored Action assignments and package references remain
ordinary document data. Executable Lua, VM state and active Furniture use are not
persisted. No low-level test-only Lua or Vertex seam was added.

Typed Device commands and Traversal resources still own operation and admission;
routing, Access permissions and Mobility authority are unchanged. Device UI
migration is outside this delivery. Parent #455 is neither modified nor closed.

## Public-workflow verification

The existing domain-owned checks and their exhaustive/CLI/isolation contracts
verify the integrated workflow rather than introducing another test API:

- World `furniture/bundledLua` and `furniture/actions` load bundled and regression
  dependencies, preserve identities and verify explicit/default Idle semantics.
  `furniture/luaObjects` covers validated Lua geometry/functions, safe capabilities,
  budgets, immutable accepted packages, reconciliation and YAML/binary reopening.
  It now rejects `.furniture.yaml`, `.furniture.yml`, `.yaml` and `.yml` through
  loader, attachment, reload and World dependency workflows, asserting conversion
  diagnostics, unchanged live state and unchanged external bytes.
- Editor `furniture/cataloguePicker` now verifies conversion diagnostics and no
  document/history/file mutation through the real CPU-only panel selection seam.
  `furniture/luaWorkflow` and `markerActions/workflow`, `reloadWorkflow`,
  `behaviourConfiguration` and `furnitureUseWorkflow` cover history, references,
  visible Action options, request selection and reload.
- Simulation `markerActions/registry`, `execution`, `failures`, `logging`,
  `atomicEffects`, `deviceEffects`, `claimCompetition` and Furniture-use/lifecycle
  checks cover immutable built-ins, safe scripting, atomic effects, deterministic
  outcomes, explicit use, old-function finishing, cleanup/failure containment and
  transactional reload. `documents` and `furnitureUseDocuments` verify authored
  persistence without executable Lua or active use.
- Behaviours, Routing, Persistence, Render and World checks retain existing
  movement, permission/Mobility, occupancy, identity, support, document/history
  and artwork/resource contracts. All remain included in unfiltered CTest.

## Final Linux validation

Reused GUI-enabled `build-linux-validation/release` and `debug` trees. Displays
were unset; Editor/Render checks use CPU ImGui and Startup uses a controlled
unavailable driver. No native window or dialog was required. Default inventory
builds include core, headless tools/modules and the GUI editor.

Focused Release `furniture/luaObjects` and `furniture/cataloguePicker` passed.
Affected World, Editor, Persistence, Simulation, Behaviours, Routing and Render
fast functional/CLI/isolation milestone passed (`14aa951fd3744c4f839dfb90a39fa004`).

Final commands:

```sh
PF_VALIDATION_JOBS=4 scripts/validate_linux_smoke.sh --config Release --lane final --build-only all
python3 scripts/validate_ctest_lane.py --build-tree build-linux-validation/release --config Release --lane final --parallel 8
PF_VALIDATION_JOBS=4 scripts/validate_linux_smoke.sh --config Debug --lane final --build-only all
python3 scripts/validate_ctest_lane.py --build-tree build-linux-validation/debug --config Debug --lane final --parallel 2
```

| Configuration | Default build run | Successful unfiltered final CTest run | Result |
| --- | --- | --- | --- |
| Release | `6eb950ae238e459aaa31a0a57b9b5db7` | `c5ffe3d45f6348aa870e91d9797599a7` | 109 passed, one optional GUI capability skipped; 146.60 s |
| Debug | `3d69e489da84407792460c90c24bff5d` | `ba7a0380178a4689a23ce5b474691f2f` | 109 passed, one optional GUI capability skipped; 794.56 s |

Logs, phase records and results are retained under each tree's
`.pf-validation/run-<id>/`. The sole skip is the vendored optional
`willpower_resource_manager_gui_smoke`; production Editor, Render and Startup
coverage passed. Native Windows validation is not claimed.

The initial Debug final lane at eight jobs, concurrent with Release, timed out
only `smoke-simulation` at its unchanged 60-second limit
(`059fb68e8ca04b8e96269c4a1677c46c`). Guarded focused recovery at one job passed in
56.80 s (`05c7b63810a84655b9ec6c70bc280c30`), then the complete unfiltered Debug
lane at two jobs passed, including that module in 54.88 s. No assertions,
coverage or timeouts were weakened and no source repair was necessary.

Build diagnostics retain the existing Make jobserver warning and unrelated
`render/SecurityScanners.cpp:231` narrowing warnings. `git diff --check` passed;
source/resource audit found no checked-in YAML Furniture catalogue, generated
old-format catalogue path, legacy enum or implicit adapter. Historical per-slice
validation in earlier documents remains historical, superseded by this final
integration evidence.
