# Launcher application

`PromethiumFermide` is a minimal application DLL using AppLib. It exports the
Launcher DLL interface and registers `Controller` as its initial state. Controller
uses AppLib's suspend/resume flow to run:

`Load -> MapLoad -> Play -> MapUnload -> Unload -> exit`

There is no map-transition state. The lifecycle steps run synchronously on their
first update and return to Controller:

- Load scans the application resource manifest.
- MapLoad acquires, creates, and loads `World/World` and its TextFile dependency.
- Play keeps the Map alive without updating or rendering its World.
- MapUnload releases the Map, clears its World, and destroys the Map and source
  data when no other consumer holds them.
- Unload performs idempotent final cleanup.

Controller owns a `MapResources` guard, borrowed by children through transition
`userData`, so a loading failure or early application exit also releases the Map.
It is destroyed before Launcher's resource manager. A subsequent run recreates
and deserializes the World rather than retaining stale created data.

Play renders an empty black MPP scene with no GUI, assets, map, entities, or audio.
Press Escape to return to Controller, run MapUnload and Unload, and exit Launcher
normally.

`pf::application::Map` derives from `applib::Map` and exclusively owns an optional
`core::World`. `getWorld()` returns a borrowed mutable or const pointer;
`setWorld(std::unique_ptr<core::World>)` replaces the owned instance, and
`clearWorld()` releases it. Replacement, clearing, and Map destruction destroy
the previous World. `MapResourceFactory` creates these resources with their
AppLib metadata and the reference's 512-unit acceleration grid. Neither the
Map constructor nor its resource factory loads a World. On application entry,
the resource manager registers `MapResourceFactory` and `MapWorldDefinitionFactory`.
The latter selects the `PromethiumFermide` definition factory and deserializes its
`Resource: Yaml` TextFile dependency into a candidate World before replacing the
owned instance.

`application-resources/Resources.yaml` declares `World/WorldFile` for
`test-worlds/room-height-test-1.world.yaml` and `World/World` as its dependent Map.
The fixture is staged beside this manifest. Explicit resource creation loads the
World during MapLoad; Play deliberately leaves it idle.

Play derives from `applib::State` to reuse its scene, camera, and render-pipeline
lifecycle. `applib::StatePlay` is deliberately not used yet: its default setup
requires a model, map transition data, and a player entity.

Build and run on Linux from the repository root:

```sh
cmake --build build-linux --config Release --target Launcher pf_application_tests
ctest --test-dir build-linux -C Release -R "^pf_application$" --output-on-failure
cd build-linux/bin/x64/Release
./Launcher
```

Build and run on Windows:

```bat
cmake --build build-windows --config Release --target Launcher pf_application_tests
ctest --test-dir build-windows -C Release -R "^pf_application$" --output-on-failure
cd build-windows\bin\x64\Release
Launcher.exe
```

Building Launcher also builds the application and deploys its default
`Game.yaml`, `application-resources/Resources.yaml`, and its test World next to Launcher.
The DLL filename in the configuration is generated for the host platform.
Run Launcher from that output directory so relative paths resolve correctly.
