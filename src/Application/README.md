# Launcher application

`PromethiumFermide` is a minimal application DLL using AppLib. It exports the
Launcher DLL interface and registers `Controller` as its initial state. Controller
uses AppLib's suspend/resume flow to run:

`Load -> MapLoad -> Play -> MapUnload -> Unload -> exit`

There is no map-transition state. Load, MapLoad, MapUnload, and Unload are no-op
placeholders that return to Controller on their first update; they do not scan,
load, or unload resources or maps. AppLib's real loading states are intentionally
not used yet because they perform resource work and require map data.

Play renders an empty black MPP scene with no GUI, assets, map, entities, or audio.
Press Escape to return to Controller, run MapUnload and Unload, and exit Launcher
normally.

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
`Game.yaml` and empty `application-resources/Resources.yaml` next to Launcher.
The DLL filename in the configuration is generated for the host platform.
Run Launcher from that output directory so relative paths resolve correctly.
