# Spike: Dear ImGui Test Engine migration

Spike of `ocornut/imgui_test_engine` v1.89.5 (vendored at `ext/imgui_test_engine/`)
as a replacement for this project's spatial-scan headless UI click helpers.

These are measurement/feasibility harnesses, not part of the smoke suite.

| File | Purpose |
|---|---|
| `spike_te.cpp` | Baseline: scan vs engine item resolution + determinism |
| `spike_port.cpp` | Port of `station/deliveryWorkflow` |
| `spike_port2.cpp` | Port of `escalators/brokenControlsAndHistory` |
| `spike_port3.cpp` | Disabled-control semantics (synthetic) |
| `spike_port3b.cpp` | Disabled-control semantics against the real permission panel |

Build any of them with, from the repo root:

```
LUASRC=<lua-5.4 source>/src
g++ -std=c++20 -w -DIMGUI_TEST_ENGINE_ENABLE_STD_FUNCTION=1 \
    -Iinclude -Iinclude/imgui -Iext/imgui_test_engine -Iext \
    -Isrc/headless/smoke -Isrc/headless/smoke/support -Isrc/headless/support \
    -Ibuild-linux/_deps/yaml-cpp-src/include -I$LUASRC \
    <file>.cpp src/headless/RenderGuiStubs.cpp -o /tmp/<file> \
    -L build-linux -L build-linux/bin/x64/Release -L build-linux/_deps/yaml-cpp-build \
    -l:libpf-agent-editing.a -l:libpf-render.a -l:libpromethium-fermide-core.a \
    -l:libpf-imgui-cpu.a -l:libpf-agent-resources.a -l:libpf-metrics.a \
    -l:libpf-smoke-support.a -l:liblua-5.4.9.a -l:libyaml-cpp.a -lpthread -lm
```

## Headline results

| Check | Scan frames | Engine frames |
|---|---|---|
| `station/deliveryWorkflow` | 198,862 | 106 |
| `escalators/brokenControlsAndHistory` | 386 | 29 |

## Two rules the spikes established

1. **Never use `ItemClick` to assert disabled-control behaviour.** The engine silently
   skips disabled targets and reports Success — the assertion becomes vacuous. Use
   `ItemInfo` to assert `InFlags & ImGuiItemFlags_Disabled`, then
   `MouseTeleportToPos(rect)` + `MouseClick()` for a real press.
2. **Capture text with `ImGui::LogToBuffer()` + `GImGui->LogBuffer`**, not
   `LogToClipboard` + `io.SetClipboardTextFn`. The engine replaces the clipboard
   hook while a test runs.
