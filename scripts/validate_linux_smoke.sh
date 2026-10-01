#!/usr/bin/env bash
# Complete, headless Linux release gate for the modular smoke architecture (#306).
set -euo pipefail

if [[ "$(uname -s)" != Linux ]]; then
    echo "ERROR: this validation gate is Linux-only; Windows validation is delegated to #279." >&2
    exit 2
fi

ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_ROOT="${1:-$ROOT/build-linux-validation}"
JOBS="${PF_VALIDATION_JOBS:-$(nproc)}"
EVIDENCE="$BUILD_ROOT/evidence"
mkdir -p "$EVIDENCE"
unset DISPLAY WAYLAND_DISPLAY

run_timed() {
    local label="$1"
    shift
    local start end
    start="$(date +%s%N)"
    "$@"
    end="$(date +%s%N)"
    printf '%s\t%d\n' "$label" "$(((end - start) / 1000000))" >> "$EVIDENCE/runtimes-ms.tsv"
}

configure_and_build() {
    local name="$1" config="$2" gui="$3" analysis="$4"
    local directory="$BUILD_ROOT/$name"
    cmake --fresh -S "$ROOT" -B "$directory" -DCMAKE_BUILD_TYPE="$config" \
        -DBUILD_TESTING=ON -DPF_BUILD_GUI="$gui" -DPF_HIGH_ANALYSIS="$analysis" \
        -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
    cmake --build "$directory" --parallel "$JOBS"
}

: > "$EVIDENCE/runtimes-ms.tsv"
printf 'command\tmilliseconds\n' >> "$EVIDENCE/runtimes-ms.tsv"

# The Debug/no-GUI quadrant also supplies the elevated-analysis build.
configure_and_build release-gui Release ON OFF
configure_and_build release-headless Release OFF OFF
configure_and_build debug-gui Debug ON OFF
configure_and_build debug-headless-analysis Debug OFF ON

release_gui="$BUILD_ROOT/release-gui"
release_headless="$BUILD_ROOT/release-headless"
debug_gui="$BUILD_ROOT/debug-gui"
debug_headless="$BUILD_ROOT/debug-headless-analysis"

ctest --test-dir "$release_gui" --output-on-failure
ctest --test-dir "$release_gui" --output-on-failure -j "$JOBS"

modules=(agent agent-tags behaviours editor metrics permissions persistence render routing simulation transports world)
for module in "${modules[@]}"; do
    target="pf-smoke-$module"
    cmake --build "$release_headless" --target "$target" --parallel "$JOBS"
    executable="$release_headless/bin/x64/Release/$target"
    selector="$($executable --list | head -n 1)"
    "$executable" --check "$selector"
    run_timed "$target" "$executable"
done
# Generated link commands must contain only the owning module's check objects.
python3 - "$release_headless" "${modules[@]}" <<'PY'
import pathlib
import sys

root = pathlib.Path(sys.argv[1]) / "src/headless/smoke/CMakeFiles"
modules = sys.argv[2:]
for module in modules:
    text = (root / f"pf-smoke-{module}.dir/link.txt").read_text(encoding="utf-8")
    unexpected = [other for other in modules if other != module and f"pf-smoke-{other}.dir/" in text]
    if unexpected:
        raise SystemExit(f"pf-smoke-{module} links check objects from: {', '.join(unexpected)}")
print("PASS module link ownership: no module links another module's check objects")
PY

cmake --build "$release_gui" --target pf-smoke-startup --parallel "$JOBS"
startup="$release_gui/bin/x64/Release/pf-smoke-startup"
"$startup" --check graphicsInitializationFailure
run_timed pf-smoke-startup "$startup"

# The five dedicated tools exercise real bounded workflows, not only --help.
tools=(pf-restoration-benchmark pf-generate-routing-world pf-metrics-server pf-lift-repro pf-pause-position-repro)
for target in "${tools[@]}"; do
    cmake --build "$release_headless" --target "$target" --parallel "$JOBS"
    "$release_headless/bin/x64/Release/$target" --help >/dev/null
done
temporary="$(mktemp -d -t pf-306-tools-XXXXXX)"
cleanup() {
    if [[ -n "${source:-}" && -f "$temporary/MarkerIdentity.cpp" ]]; then
        touch -r "$temporary/MarkerIdentity.cpp" "$source"
    fi
    rm -rf -- "$temporary"
}
trap cleanup EXIT
"$release_headless/bin/x64/Release/pf-generate-routing-world" "$temporary/routing.world.yaml"
"$release_headless/bin/x64/Release/pf-restoration-benchmark" "$temporary/routing.world.yaml" 1
"$release_headless/bin/x64/Release/pf-metrics-server" --port 0 --ticks 1
lift_fixture="$ROOT/resources/test-worlds/lift-test-1.world.yaml"
"$release_headless/bin/x64/Release/pf-lift-repro" crossing "$lift_fixture"
"$release_headless/bin/x64/Release/pf-lift-repro" boarding "$lift_fixture"
"$release_headless/bin/x64/Release/pf-pause-position-repro" minimal

standalone=(pf-simulation-step-timing-checks pf-occupant-packing-checks pf-world-render-slot-checks pf-sector-tileset-checks)
for target in "${standalone[@]}"; do
    cmake --build "$release_headless" --target "$target" --parallel "$JOBS"
done
"$release_headless/pf-simulation-step-timing-checks"
"$release_headless/pf-occupant-packing-checks"
"$release_headless/pf-world-render-slot-checks"
"$release_headless/pf-sector-tileset-checks" \
    "$ROOT/resources/textures/sectors.tileset.yaml"
cmake --build "$release_headless" --target pf-compile-contracts --parallel "$JOBS"

# Record a fresh direct build, a no-op build, and a representative touched-check rebuild.
rebuild="$BUILD_ROOT/rebuild-world"
cmake --fresh -S "$ROOT" -B "$rebuild" -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_TESTING=ON -DPF_BUILD_GUI=OFF -DPF_HIGH_ANALYSIS=OFF
run_timed clean-pf-smoke-world cmake --build "$rebuild" --target pf-smoke-world --parallel "$JOBS"
run_timed noop-pf-smoke-world cmake --build "$rebuild" --target pf-smoke-world --parallel "$JOBS"
source="$ROOT/src/headless/smoke/world/MarkerIdentity.cpp"
stamp="$temporary/MarkerIdentity.cpp"
cp --preserve=timestamps -- "$source" "$stamp"
touch "$source"
run_timed touched-pf-smoke-world cmake --build "$rebuild" --target pf-smoke-world --parallel "$JOBS"
touch -r "$stamp" "$source"
source=""

python3 "$ROOT/scripts/tests/smoke_ownership.py" "$ROOT" \
    "$ROOT/src/headless/smoke/CMakeLists.txt" "$ROOT/docs/smoke-migration-manifest.md"
git -C "$ROOT" diff --check
printf 'PASS Linux modular smoke validation; evidence: %s\n' "$EVIDENCE"
