# Scripted actions regression conversion (#466)

## Scope

All nine checked-in regression Furniture catalogues are native `.furniture.lua`
modules; their YAML predecessors are deleted. UUIDs, definition and usable-point
keys, artwork, explicit vertices/edges, Local depths, fractional positions and
routing scenarios are preserved. Independent catalogue/World baselines moved
from `fixtures/legacy-furniture` to `src/headless/smoke/fixtures/furniture`.
Worlds remain YAML/binary; intended seated/lying fixture journeys explicitly
select Use furniture. Geometry-only fixtures retain no callbacks.

Every generated variant is Lua source, including malformed geometry, relative
depths, independent complete-Path oracle layouts, reordered/removed/new point
reconciliation, composed/attached routes, palette paging and render projections.
The source-composition helper only reads and writes text; it does not evaluate
Lua or introduce a low-level Lua/Vertex API. Each variant goes through existing
production catalogue and World/document loading. YAML manipulation remains only
for World documents and the application resource manifest, not catalogues.
Temporary dependency copying, resource-managed lookup and editor picker tests
use Lua basenames. No external-user conversion tool or loader was added.

## Owning coverage

- **World:** `furniture/actions`, `bundledLua`, `luaObjects`, `demo`, `chair`,
  `layouts`, `deskRoutes`, `attachments`, `composition` preserve identity,
  geometry, support/overlap and placement refusal contracts. Converted dependent
  Worlds are loaded and simulated, with explicit use versus omitted Idle checked.
- **Routing:** `furniture/depthContinuity`, `sectorDepthContinuity`, `seatRouting`
  preserve complete-Path cost/depth oracles, occupied-destination exclusion,
  occupied-waypoint circulation and ordinary Route loss.
- **Persistence:** `furniture/documents` preserves YAML/binary Worlds, relative
  dependency portability, current-definition reconciliation, stable identities,
  saved Path/behaviour references and atomic incompatible-dependency refusal.
- **Simulation:** converted arrival/Bed/seated-edit/occupancy checks explicitly
  request use. `furniture/explicitIdle` keeps omitted and explicit Idle Standing
  with no claim before, on and after arrival, despite available Use furniture.
  Existing Marker Action checks cover replacement/departure, pause/deactivation,
  unreachable planning, structural edits, finish failures and callback-free
  load/reset/history. `markerActions/furnitureUseAtomicity` adds simultaneous
  independent seats and rollback of Furniture-use Pose/claim/logs on Lua error
  or instruction exhaustion, no installed finish lifecycle and subsequent access.
- **Behaviours:** `furnitureUseOutcomes` selects Use furniture from Lua, observes
  ordinary route-time/coincident-arrival occupancy failures without a script
  crash, chooses the other sofa seat and asserts exactly one failure/success,
  independent claims and coherent Poses.
- **Render/Editor:** converted artwork, depth ordering, plan-grid/graph and palette
  variants retain CPU-only rendering, selection, placement, edit, deletion,
  refusal, saved dependencies and Undo/Redo checks.

New checks remain in their existing domain modules, with CLI inventory contracts
updated. No duplicate legacy aggregate CTest registration was introduced.

## Final contract handoff (#467)

#466 left production migration scaffolding for #467. That final slice now removes
the YAML parser/suffix acceptance, optional closed-enum point action and implicit
arrival adapter. No old-format regression caller, generated catalogue path or
checked-in YAML Furniture catalogue remains. Unsupported external YAML is rejected
without rewriting it. Historical validation below and in prior slice documentation
describes the state at that time; see [final integration evidence](scripted-actions-integration.md).

## Validation

Focused Release checks covered all converted fixtures/builders and new semantics.
The affected World, Routing, Persistence, Simulation, Behaviours, Render and Editor
functional/CLI/isolation milestone passed after updating the new check inventories.
Final source validation uses the repository's supervised default-inventory build
and unfiltered final CTest lane for both Debug and Release, without display servers
or dialogs.

| Configuration | Default build run | Unfiltered final CTest run | Result |
| --- | --- | --- | --- |
| Release | `05f2e85f44f248e9887fc8125cc9212a` | `b8a10c3c0dc7440e8bbbe382fd3749ff` | 109 passed, one optional GUI capability skipped; 144.30 s |
| Debug | `d285b673f12648d1be6eaa92bc1c397a` | `d9b3cbb461ed4916aec1e82dcd1440c6` | 109 passed, one optional GUI capability skipped; 796.16 s |

Both default core/headless/editor builds and unfiltered final CTest supervisors
exited 0. The sole skip is `willpower_resource_manager_gui_smoke`; CPU Editor,
Render and controlled Startup checks passed. `git diff --check` passed. Native
Windows validation is not claimed. The first full runs caught a missed Bed
fixture request at Marker 5; its Action was corrected, focused checks passed in
both configurations, and the complete final lanes above were rerun successfully.
