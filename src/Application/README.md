# Launcher application

`PromethiumFermide` is a minimal application DLL using AppLib. It exports the
Launcher DLL interface and registers `Controller` as its initial state. Controller
uses AppLib's suspend/resume flow to run:

`Load -> MapLoad -> Play -> MapUnload -> Unload -> exit`

There is no map-transition state. The lifecycle steps run synchronously on their
first update and return to Controller:

- Load scans the application resource manifest.
- MapLoad acquires, creates, and loads `World/World` and its World resource dependency.
- Play keeps the Map alive, starts its simulation at normal (1×) speed, and
  updates and renders its World.
- MapUnload releases the Map, clears its resource binding, and destroys the Map
  and World resource data when no other consumer holds them.
- Unload performs idempotent final cleanup.

Controller owns a `MapResources` guard, borrowed by children through transition
`userData`, so a loading failure or early application exit also releases the Map.
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
native aperture scissors. Play releases its batches and atlas acquisitions before
MapUnload. Rendering has not been extracted into a module shared with the editor:
the two implementations are intentionally independent so they can diverge.

Assign the editor's built-in **Player controlled** behaviour to any Agent to
control the lowest-ID eligible Agent in Play. Hold Left/Right to walk Standing;
release to stop (both keys cancel). The camera follows that Agent and its Layer.
Walls, low spaces, closed Bulkhead Doors and Floor gaps block movement; no device
operation or automatic crouching/crawling is performed. Without an eligible
Agent, Player input is disabled and the fitted Layer-0 camera remains.
See [Player control](../../docs/player-control.md) for authoring and validation.

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
the combined resource types and use the same staged `Resources.yaml` and root
resource tree beside their executables. Explicit resource creation loads the
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

Building Launcher also builds the application and deploys its default
`Game.yaml` and the complete root `resources/` tree (including `Resources.yaml`)
next to Launcher, using the same asset staging target as the editor.
The DLL filename in the configuration is generated for the host platform.
Run Launcher from that output directory so relative paths resolve correctly.
