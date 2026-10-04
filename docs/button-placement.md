# Mixed-button placement demonstration (#437)

Open `resources/test-worlds/mixed-buttons.world.yaml` (bundled resource
`MixedButtonsDemo`). It is an ordinary World document, not a special allocator
fixture. No invalid layout is saved. World X coordinates below are absolute;
Levels are numbered from 0. Switch the viewed Layer to inspect its controls.

| Location | Layer / Level | What to inspect |
| --- | --- | --- |
| 2-button stack | 1 / 2 | X=2: Platform lift call below Ladder extension |
| 3-button stack | 1 / 2 | X=6: Door, Platform lift, Ladder, bottom to top |
| 4-button stack | 1 / 2 | X=10: Door, Platform lift, lower Ladder's upper control, upper Ladder's lower control |
| Side reassignment | 1 / 0 | Doors at X=13 and 14 move left together; Ladder control remains at 15. Separation beats stacking. The left shared wall is removed. |
| Independent Stops | 0 / 0, 2, 4 | Platform lift at X=19: controls at 20, 20, 19 respectively, following permanent support at each Stop |
| Inset left/right | 0 / 0 | Bulkhead controls at 25.75 and 26.25; light switch at 24.5 |
| Permanent bridge supports | 0 / 1 | Force Bridge spans 31–32; controls at 30.75 and 33.25 remain on permanent support |
| Atomic support refusal | 1 / 0–4 | Ladder at X=37 and Platform lift at 38; upper controls use the permanent Walkway at Level 4 |

The front Door controls on Layer 0 remain independent even where their X and
Level coincide with the Layer 1 stacks. Stack spacing is one Button height plus
a 25% gap. Every stack has one normal-height graph approach, not four vertically
separated Agent destinations.

## Operating protected members

Select **Caller** in the 4-button Location, view Layer 1, and run the simulation.
Click the second Button from the bottom at X=10, Level 2: only the Platform lift
call is requested. Caller has `Platform call`, not `Ladder extension`; clicking
either upper Ladder Button cannot extend a Ladder. Select **Climber** to operate
those upper controls instead; Climber has only `Ladder extension`, so cannot call
the Platform lift. The bottom Door control is unprotected. Pause before editing.

Click the actual Button shape, not the shared graph vertex. Graph co-location
is not a combined operation or permission grant. Agents stay at Level 2 while
operating any of these members. Routing to a traversal uses its particular
control, not whichever Button happens to be bottom-most. Device commands remain
separate from traversal admission (ADRs 0001 and 0015).

## Reproducing atomic refusals

Start each experiment from a fresh open of the bundled World; pause first.
All operations below are also executed headlessly by
`buttonPlacement/demonstration`, including document/history no-op assertions.

* **Fifth Button:** copy the ordinary controlled Door at Layer 0, Level 2, X=9
  and try placing it at X=10 on that Layer/Level. Its back-side control would be
  an unavoidable fifth member at X=10 in the 4-button Location. Placement is
  refused: no Door, Button, graph change, permission change or undo entry remains.
* **Wall refusal:** remove the 4-button Location's left shared wall at Level 2
  (world X=9). Now the same Door paste at X=10 is feasible: the first Door can
  move to X=9 while four controls remain at X=10. Try restoring that shared wall.
  Restoration is refused atomically because it would force five controls at 10.
  Removing the pasted Door allows restoration again.
* **Support refusal:** in `Atomic support refusal`, try adding a Walkway at
  Level 2, X=37 (local cell 0). It would shorten the Ladder and move its upper
  endpoint to Level 2. The right candidate at X=38 lacks permanent support there;
  the left candidate at X=37 straddles the retained shared wall. The entire
  Walkway edit is refused, leaving the Level-4 endpoint and graph intact.

You need not save the experimental states. Undo/redo of valid wall and control
edits uses canonical reconstruction rather than previous placement retention.

## Reproduction and regression coverage

The deterministic builder is `buildButtonDemo` in
`src/headless/smoke/editor/ButtonDemo.h`. The Editor check builds all mixed owners
in opposite orders, writes a normal document to its invocation's temporary root,
and compares the bundled document against the builder. Comparisons use centre,
vertical rank, control name and authored Permission requirements, never Button,
Interaction point, graph vertex or device allocation IDs. It verifies YAML and
binary reload, construction replay, clipboard option readback/reconstruction,
undo/redo, graph sharing, hit targeting, independently authorized commands,
runtime stability and the three refusals above. All checks are non-interactive.

Focused invocation after building `pf-smoke-editor`:

```sh
env -u DISPLAY -u WAYLAND_DISPLAY \
  build-linux-validation/debug/bin/x64/Debug/pf-smoke-editor \
  --check buttonPlacement/demonstration
```

The final placement regression also includes existing production-seam coverage:

| Lane / source | Contract |
| --- | --- |
| World `two-sided-buttons` / `TwoSidedButtons.cpp` | All owner families, candidate/support/wall rules, cross-Location rejection, geometry/role/type ties, independent landings, mixed structural reflow, authorization and stable runtime placement |
| World `completeOptimisation` within that check | Exhaustive four-tier oracle; mixed Door/Ladder public authoring proves maximum-stack minimisation, preference count and canonical type tie-breaks in both creation orders; front Door footprints still obstruct columns |
| Editor `buttonPlacement/demonstration` and Door panel checks | Bundled workflow, clipboard reconstruction, history and atomic refusals |
| Persistence mixed reconstruction / BoothWindows checks | Every family in valid/invalid YAML and binary, historical permission handling, repeated replay and transactional failure |
| Render Door Button checks | Production command-stream stack spacing, visible fills/outlines and independently targetable mixed artwork |
| Permissions, Simulation, Transports | Selected protected operations, traversal separation, routing and runtime device behavior |

The opposite-order checks exposed asymmetric column preflight: a Door's back
approach could be added after a Ladder or Platform lift but was incorrectly
considered an obstruction when added first. Preflight now admits those back
approaches in either order, while retaining front-doorway footprint rejection.

Artwork redesign and additional object-specific state presentation are **not**
part of this placement verification. The parent #425 remains the authoritative
placement specification, not a claim that those separate presentation tasks are
complete.

## Final validation

Final GUI-enabled Linux validation used the existing incremental trees and
six-job supervised lanes, with `DISPLAY` and `WAYLAND_DISPLAY` unset:

```sh
PF_VALIDATION_JOBS=6 scripts/validate_linux_smoke.sh --config Debug --lane final --build-only all
python3 scripts/validate_ctest_lane.py --build-tree build-linux-validation/debug --config Debug --lane final --parallel 6
# Repeat both commands with Release and build-linux-validation/release.
```

| Configuration | Core/headless/GUI default build | Unfiltered final CTest |
| --- | --- | --- |
| Debug | Passed | 110 entries, zero failures; 589.98 s |
| Release | Passed | 110 entries, zero failures; 124.89 s |

Each configuration had one supported optional skip:
`willpower_resource_manager_gui_smoke` (no X11 display). CPU Render/Editor checks,
graphics startup/failure probes, ownership, CLI, concurrency and exhaustive
contracts passed. Windows execution is not claimed (the Linux workflow delegates
it separately). `git diff --check` passed; no C++ compiler warnings were reported
in final builds (the build-isolation contract emitted a make jobserver warning).

Supervised evidence under each tree's `.pf-validation/`:
Debug build `run-6163dff1087f4da38ff3b7c83b0dc554`, tests
`run-424bf9cfab9c489cb23b510c95348b46`; Release build
`run-bc369d21504d452d82eb1f5c14fd6bfb`, tests
`run-f7e0c23c779c4320b104204c54ab5e13`.

The earlier affected World/Editor/Render/Persistence milestone passed all
functional checks; its explicit Editor CLI inventory was updated for the added
check, and focused repair passed before both final matrices. See
[Linux validation](linux-smoke-validation.md) and
[validation recovery](validation-recovery.md).
