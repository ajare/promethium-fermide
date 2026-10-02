# Modular smoke builds on Windows

Ticket #307 covers **build** validation. Runtime, CTest concurrency, subprocess,
fixture and temporary-directory validation remain separate work under #279.

## Reproduce the matrix

Use Windows x64, MSVC, CMake, Python 3, and recursively initialized submodules.
Run from the repository root; select the installed Visual Studio generator:

```bat
set CL=/MP4
python scripts\validate_windows_smoke.py --build-root out\msvc-smoke --generator "Visual Studio 18 2026"
```

`CL=/MP4` enables MSVC translation-unit parallelism; CMake's `--parallel` alone
parallelizes targets, not the source files inside a Visual Studio target. Preserve
any existing `CL` options when setting it.

Use an unused build root for fresh-build evidence. The script configures separate
GUI-enabled and GUI-disabled trees with `BUILD_TESTING=ON`, then directly builds
**every configured smoke executable**, the retained standalone checks,
`pf-compile-contracts`, all five dedicated tools, the World generator, and finally
the dispatch-only compatibility executable, in **Debug and Release**. Startup
exists only with the GUI. `--gui on` or `--gui off` selects one tree.
To reuse a particular existing tree rather than configure a new one, use
`--build-tree <path> --gui on` (or `--gui off`).

The script passes `--config` on every build. Representative Agent, Render, and
Editor targets are built first, before the aggregate. CMake File API dependency
and source inventories establish their boundaries; generated compiler commands
are checked for consistent `/MDd` (Debug) and `/MD` (Release) across project and
source-built dependency targets. MPP has its own external build; its runtime
selection must also be inspected in its build logs/projects.

Logs and inventories are written to each tree's `validation-logs/`:

- `configure.log`: toolchain and configure diagnostics;
- `<configuration>-<target>.log`: direct target build evidence;
- `<configuration>-boundaries.json`: representative dependency/source closure;
- `products.json`: absolute product paths from CMake, checked for existence.

All project headless executables, including retained standalone checks, are in
`<tree>/bin/x64/Debug/` or `<tree>/bin/x64/Release/`. Do not discover products
relative to a process working directory. CTest uses target-resolved paths.

## Elevated analysis

The matrix accepts `--high-analysis`. A focused warning-clean harness/API-contract
build, independent of the production library's existing diagnostic baseline, is:

```bat
cmake -S . -B out\msvc-analysis -G "Visual Studio 18 2026" -A x64 -DPF_BUILD_GUI=OFF -DBUILD_TESTING=ON -DPF_HIGH_ANALYSIS=ON
cmake --build out\msvc-analysis --config Debug --target pf-smoke-harness-probe pf-compile-contracts --parallel 4
```

Normal verification targets receive `/W3 /permissive-`; elevated analysis uses
`/W4 /permissive-` without a competing explicit `/W3` (MSVC D9025). Dependencies
retain their own warning policies. Existing production warnings are not silenced
or treated as new modularization defects.

## Remediation

- Restore the parent Debug/Release configuration inventory after Willpower's
  subdirectory changes the shared cache for its standalone Shipping/MemCheck
  configurations. PF does not support or validate those extra configurations.
- Apply the shared configuration-aware output policy to standalone verification
  executables and smoke support archives as well as modules and tools.
- Avoid conflicting explicit warning levels when elevated analysis is enabled.
- Preserve the existing DLL CRT selection and module ownership; no linker runtime
  exclusions, merged modules, or smoke implementations in the compatibility
  executable are used.

Exact environment, commands, outcomes, and limitations are recorded on #307.
