# Controlled graphics Startup on Windows (#311)

Startup remains a subprocess-only module: it links the smoke harness, not editor,
SDL, MPP, or graphics implementations. Building it builds its required editor and
runtime assets. The editor must return **1** for the deliberately nonexistent SDL
video driver. Success, other ordinary statuses, aborts, exception termination,
missing products, and a 30-second timeout all fail the check.

The runner supplies a private Unicode environment block on Windows (and changes
only the forked child on Linux). `SDL_VIDEODRIVER` is replaced; `DISPLAY` and
`WAYLAND_DISPLAY` are removed. The parent environment is unchanged. The editor,
runner, and compatibility dispatcher share OS/CRT dialog suppression. Windows
caller-selected error-mode flags are preserved; Linux setup is a no-op.

## Reproduce

Use Windows x64, MSVC, CMake, Python 3 and initialized submodules. Select an unused
build directory for fresh multi-configuration evidence:

```bat
cmake -S . -B "out\startup validation\gui" -G "Visual Studio 18 2026" -A x64 -DPF_BUILD_GUI=ON -DBUILD_TESTING=ON
cmake --build "out\startup validation\gui" --config Debug --target pf-smoke-startup pf-startup-probe prometheum-fermide-headless pf-compatibility-probe pf-smoke-harness-probe --parallel 4
ctest --test-dir "out\startup validation\gui" -C Debug -R "^(smoke-startup.*|smoke-harness-contract|smoke-ownership-audit|headless-compatibility-contract)$" --no-tests=error --output-on-failure -j 4
```

Repeat the build and CTest commands with `Release`. Use `CL=/MP4` to parallelize
MSVC translation units, preserving existing CL flags. From Git Bash, also set
`MSYS2_ENV_CONV_EXCL=CL` to prevent MSYS from converting `/MP4` into a path.

`smoke-startup-contract` runs the actual editor from an unrelated temporary
working directory whose path contains spaces, and checks the direct CLI and
missing-product failure. `smoke-startup-failures` uses a synthetic child copied to
a path containing spaces. It tests controlled status 1, unexpected success,
status 23, CRT abort, a real unhandled Windows access violation, and timeout.
Its in-process probe runs the real check and then verifies the parent's video
and display environment and caller error-mode flag. No synthetic child links
or opens graphics. This test takes approximately 30 seconds.

Only `smoke-startup` and `smoke-startup-contract` retain `RUN_SERIAL`, for the
editor's shared startup log. The synthetic failure contract is parallel-safe;
there is no global parallelism restriction.

For actual compatibility dispatch, run the configuration-specific absolute path
of `prometheum-fermide-headless.exe --graphics-startup-smoke` from an unrelated
directory containing spaces. GUI-enabled builds must report Startup PASS and
return 0. Configure a second tree with `PF_BUILD_GUI=OFF`, build the dispatcher
in both configurations, and repeat: it must report missing `pf-smoke-startup.exe`
and return 127. Neither selection may open a graphics window. The separate
[compatibility contract](headless-compatibility.md) checks mapping and abnormal
child outcomes with synthetic products.

## Validation evidence

The fresh GUI-enabled `out/311 startup/gui` tree built Startup, its editor child,
the synthetic probe, and compatibility products in Debug and Release. All 30
selected smoke/module contracts and compatibility tests passed in each
configuration with CTest `-j 4`. GUI-disabled compatibility was also built and
checked in Debug and Release in `out/307-nogui`. Exact commands, logs, actual
dispatch outcomes, and commit identification are recorded on #311.
