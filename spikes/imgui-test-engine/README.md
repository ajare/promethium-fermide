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

## Item-resolution hazards found while migrating the station checks

Migrating the remaining station editor checks surfaced three engine behaviours
that each silently produced a wrong click rather than an error. The shared
`headless::TestEngineDriver::click()` handles all three; do not hand-roll
clicks in migrated checks.

1. **Stale rect after a layout change.** Adding or removing a row shifts
   everything below it. A rect read before the shift is wrong afterwards, so
   the press lands on whatever now occupies the old position. `ItemClick()`
   re-resolves through `MouseMove()` internally and inherits this.
   *Symptom:* the click silently hits a neighbour; the assertion downstream
   fails with no hint that the wrong widget was pressed.

2. **Cold pool entry poisons the operation.** `MouseMove(ref)` on an entry
   the item hooks have not repopulated since going idle fails to resolve it,
   and *every later query in the same operation then comes back empty*.
   Warming with a throwaway `ItemInfo(ref, NoError)` before the move fixes it.
   *Symptom:* `ItemInfo` returns `ID == 0` for a widget that is plainly
   drawn in the captured text.

3. **Teleporting does not scroll.** `MouseTeleportToPos()` goes straight to
   the coordinate. If the target is scrolled out of the window the press
   lands outside the window and hits nothing. `MouseMove(ref)` is the
   engine's item-aware move and scrolls the target into view first.
   *Symptom:* clicks on controls below the fold do nothing.

The driver resolves, presses, and then **verifies `ImGui::GetHoveredID()`
matches the intended item**, retrying with a fresh resolve if it does not.
That turns all three hazards into a self-correcting loop instead of a
mystery failure, and avoids tuning per-click frame counts.

## Results after migrating all station editor checks

| Suite | Before (scan) | After (engine) |
|---|---|---|
| `station-editor-*` (8 tests) | 213.48s | 0.66s |
| `smoke-editor` + contracts + station | 1094.17s | 12.58s |
