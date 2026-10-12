# Launcher application

`PromethiumFermide` is a minimal application DLL using AppLib. It exports the
Launcher DLL interface and registers just one state, `Play`. Play renders an
empty black MPP scene with no GUI, assets, map, entities, or audio. Press Escape
to unwind the state stack and exit Launcher normally.

Play derives from `applib::State` to reuse its scene, camera, and render-pipeline
lifecycle. `applib::StatePlay` is deliberately not used yet: its default setup
requires a model, map transition data, and a player entity.

Build and run on Windows from the repository root:

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
