# Launcher application

`PromethiumFermide` is a minimal application DLL using AppLib. It exports the
Launcher DLL interface and registers `Controller` as its initial state. Controller
uses AppLib's suspend/resume flow to run:

`Load -> MapLoad -> Play -> MapUnload -> Unload -> exit`

There is no map-transition state. The lifecycle steps run synchronously on their
first update and return to Controller:

- Load scans the application resource manifest.
- MapLoad acquires, creates, and loads `World/World` and its World resource dependency,
  then runs a post function to acquire, create, and load the Sector/Object atlas
  ImageSets and their images on the application thread before returning to Controller.
- Play keeps the Map alive, starts its simulation at normal (1×) speed, and
  updates and renders its World.
- MapUnload releases the atlases and their images, releases the Map, clears its
  resource binding, and destroys resource data when no other consumer holds them.
- Unload performs idempotent final cleanup.

Controller owns a `MapResources` guard, borrowed by children through transition
`userData`, so a loading or post-function failure or early application exit also
releases the Map and any acquired atlases.
It is destroyed before Launcher's resource manager. A subsequent run recreates
and deserializes the World rather than retaining stale created data.

Play renders Layer 0, centred and fitted to the window, on a black background.
The Application's independent renderer lives in `render/`: its sector surfaces,
thresholds, nested aperture clipping, transit geometry, Furniture and Agent pose
artwork follow the editor's rendering, but it has no editor globals or ImGui
dependency. Paths, graph vertices, grid, Markers, selections, next-Layer wireframe
and diagnostic readouts are omitted. Physical controls and processing effects
remain part of the World artwork. Play advances the simulation using elapsed
frame time in seconds; suspended states do not advance it.

The renderer borrows Launcher's MPP services and uses the resource-managed
`SectorAtlas` and `ObjectAtlas` ImageSets from the same `Resources.yaml`. GPU
batches are retained by compatible pipeline and submitted in command order with
native aperture scissors. The renderer requires atlases already loaded by MapLoad;
it never creates or loads resources. Play releases its batches and borrowed atlas
acquisitions before MapUnload releases the guard's atlas acquisitions. Rendering has not been extracted into a module shared with the editor:
the two implementations are intentionally independent so they can diverge.

Assign the editor's built-in **Player controlled** behaviour to any Agent to
control the lowest-ID eligible Agent in Play. Hold Left/Right to walk Standing;
release to stop (both keys cancel). The camera follows that Agent and its Layer.
Press Up to cross an aligned open ordinary Door between Layers, or otherwise
open an eligible manual Door; press again once open to cross. Up also enters or
exits Stairwell landing doorways. Left/Right walks on visible side platforms;
hold Up/Down at departure to choose ascent/descent, with matching horizontal
input. Arrival walks onto the next platform rather than skipping its junction.
For Staircases and Escalators, hold Up near the lower endpoint or Down near the
upper endpoint while walking toward the flight (within 0.25 World units).
Left/Right follows the slope with reversal and automatic endpoint dismount.
Moving Escalators add belt motion to walking, including walking against the belt;
release rides the steps. Broken Escalators stop and behave as stationary stairs.
Fixed and fully extended extensible Ladders mount while walking toward their centre with Up from below or Down
from above, within 0.25 World units. Once mounted, Up/Down climbs, release stops,
and opposite input reverses while alone; endpoint arrivals automatically dismount.
This includes Room Ladders. When no higher-priority climb or doorway action applies,
a fresh Up press operates one physical Button within effective arm length and its
Interaction point reach. Buttons require Arms capability, Buttons Mobility and
Access permissions; they never auto-approach or repeat while Up is held.
The same lowest-priority fresh Up action toggles a standalone BoothWindow through
its owned back-side shutter control, with physical Arms reach and permission
checks. Front-side operation and BoothWindow traversal remain unavailable;
Dumbwaiter-owned shutters retain their normal landing-button interlocks.
Airlock outside Buttons use Up to call; Left/Right manually enters the open
entrance, walks inside and exits through the committed opposite Door after the
normal interlock cycle. Player Bulkhead collision uses the moving leaf's bounding
box, allowing passage before full opening as soon as the Standing body clears it;
autonomous Agents retain their fully-open gate. Release stops, retaining capacity and crossing safety;
there is no automatic alignment or ejection, even in shared autonomous batches.
Security Scanner and Decontamination Chamber sensors open their directional
entrances without Up. Left/Right enters, walks inside during processing and
explicitly exits. Scanners wait for the Player to walk across the central scan
position; Decontamination batches need no manual slot alignment. Capacity,
processing timers and committed exits remain authoritative.
Open enclosed Lift/Shuttle Doors also use Up for manual boarding and exit;
Platform lifts use ordinary Left/Right. While aboard an open stationary vehicle,
Shift+Up/Down (Lift/Platform lift) or Shift+Left/Right (Shuttle) browses Stop short
names, and Space requests a visit. Shift+cursor suppresses locomotion even when
no selector is available. Requests survive leaving; Player passengers never
align to slots or disembark automatically. Extensible
Ladders can be deployed via their Buttons before climbing. Walls, low spaces,
closed Bulkhead Doors and Floor gaps block movement. Automatic
crouching/crawling is not supported. Without an eligible
Agent, Player input is disabled and the fitted Layer-0 camera remains.
See [Player control](../../docs/player-control.md) for authoring and validation.

F11 toggles Play debug mode (off by default; key repeat does not toggle again).
While enabled, press a physical enclosed/Platform Lift Button to select its live
readout next to the car, using MPP's built-in font. The panel shows full-precision
fractional Level, service phase, current/target Stops, dwell elapsed/minimum/time
left, boarding duration/cutoff, simulation ticks, occupancy/reservations and
Door/admission state. The selected Lift stays monitored until another Lift Button
is pressed or debug is disabled. Debug is runtime-only and does not consume
simulation events or change device operation.

Press Escape to return to Controller, run MapUnload and Unload, and exit Launcher
normally.

`pf::application::Map` derives from `applib::Map` and retains a `WorldResource`.
The World resource owns and deserializes the `core::World`, including its declared
Furniture, Agent-tag, and Agent-behaviour dependencies. Map's `getWorld()` returns
a borrowed mutable or const pointer to that same instance: it performs no second
parse and holds no independent World ownership. `setWorldResource()` binds a
created resource, and `clearWorldResource()` removes the adapter's binding without
destroying the resource's World. Destroying the World resource's data invalidates
borrowed pointers; subsequent `getWorld()` calls return null until it is recreated.

`MapResourceFactory` preserves AppLib metadata and the reference's 512-unit
acceleration grid. Both hosts register the combined manifest's resource types.
`MapWorldDefinitionFactory` selects the `PromethiumFermide` definition factory and
binds its `Resource: World` dependency, already created by ResourceManager.

The repository's single source manifest, `resources/Resources.yaml`, declares
the World resource `World/WorldFile` for `test-worlds/room-height-test-1.world.yaml`
and the Map resource `World/World`
as its dependent Map alongside all existing editor resources. Both hosts register
the combined resource types and read the original `resources/Resources.yaml` and
resource tree in the repository, without build-directory asset copies. Explicit resource creation loads the
World during MapLoad; simulation time starts advancing only in Play.

Play derives from `applib::State` to reuse its scene, camera, and render-pipeline
lifecycle. `applib::StatePlay` is deliberately not used yet: its default setup
requires a model, map transition data, and a player entity.

Build and run on Linux from the repository root:

```sh
cmake --build build-linux --config Release --target Launcher pf_application_tests pf_application_map_tests pf_application_render_tests
ctest --test-dir build-linux -C Release -R "^pf_application(_map|_render)?$" --output-on-failure
cd build-linux/bin/x64/Release
./Launcher
```

Build and run on Windows:

```bat
cmake --build build-windows --config Release --target Launcher pf_application_tests pf_application_map_tests pf_application_render_tests
ctest --test-dir build-windows -C Release -R "^pf_application(_map|_render)?$" --output-on-failure
cd build-windows\bin\x64\Release
Launcher.exe
```

Building Launcher also builds the application and generates its default
`Game.yaml` next to Launcher. The configuration references the repository's
`resources/` directory by absolute path; Worlds, scripts, manifests and artwork
are read directly from their originals, so edits are visible on restart without
staging assets or rebuilding C++. The editor also uses the original resource tree.
The DLL filename in the configuration is generated for the host platform.
Run Launcher from that output directory so the relative DLL path resolves correctly.
These development builds require the source checkout to remain at its configured path.
