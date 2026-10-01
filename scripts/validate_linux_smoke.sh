#!/usr/bin/env bash
# Incrementally build and run selected Linux smoke-test modules.
set -euo pipefail

ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
JOBS="${PF_VALIDATION_JOBS:-$(nproc)}"
SMOKE_TESTS=(
    agent
    agent-tags
    behaviours
    editor
    metrics
    permissions
    persistence
    render
    routing
    simulation
    startup
    transports
    world
)

print_usage() {
    cat <<EOF
Usage: $(basename "$0") --config Debug|Release [--build-dir path] TEST [TEST ...]
       $(basename "$0") --list

Incrementally configures, builds, and runs the selected Linux smoke-test sets.
Use "all" to select every test set.

Arguments:
  --config Debug|Release  Required build configuration.
  --build-dir path        Build directory. Defaults to
                          build-linux-validation/<debug|release>.
  --list                  List these arguments and the available test sets.
  --help                  Show this help.

Environment:
  PF_VALIDATION_JOBS      Parallel build and test jobs. Defaults to nproc.

Available smoke-test sets:
EOF
    printf '  %s\n' "${SMOKE_TESTS[@]}"
    printf '  all\n'
}

usage_error() {
    printf 'ERROR: %s\n\n' "$1" >&2
    print_usage >&2
    exit 2
}

if [[ "$(uname -s)" != Linux ]]; then
    echo "ERROR: this validation command is Linux-only." >&2
    exit 2
fi

CONFIG=""
BUILD_DIR=""
LIST_ONLY=0
SELECTED=()

while (( $# > 0 )); do
    case "$1" in
        --config)
            (( $# >= 2 )) || usage_error "--config requires Debug or Release."
            CONFIG="$2"
            shift 2
            ;;
        --build-dir)
            (( $# >= 2 )) || usage_error "--build-dir requires a path."
            BUILD_DIR="$2"
            shift 2
            ;;
        --list|--help|-h)
            LIST_ONLY=1
            shift
            ;;
        --*)
            usage_error "unknown argument: $1"
            ;;
        *)
            SELECTED+=("$1")
            shift
            ;;
    esac
done

if (( LIST_ONLY )); then
    (( ${#SELECTED[@]} == 0 )) || usage_error "--list does not accept test sets."
    [[ -z "$CONFIG" ]] || usage_error "--list does not accept --config."
    [[ -z "$BUILD_DIR" ]] || usage_error "--list does not accept --build-dir."
    print_usage
    exit 0
fi

[[ "$CONFIG" == Debug || "$CONFIG" == Release ]] || \
    usage_error "--config must be specified as Debug or Release."
(( ${#SELECTED[@]} > 0 )) || usage_error "specify at least one smoke-test set."

contains_test() {
    local candidate="$1"
    local test
    for test in "${SMOKE_TESTS[@]}"; do
        [[ "$candidate" == "$test" ]] && return 0
    done
    return 1
}

if [[ " ${SELECTED[*]} " == *" all "* ]]; then
    (( ${#SELECTED[@]} == 1 )) || usage_error '"all" cannot be combined with other test sets.'
    SELECTED=("${SMOKE_TESTS[@]}")
fi

UNIQUE=()
for test in "${SELECTED[@]}"; do
    contains_test "$test" || usage_error "unknown smoke-test set: $test"
    if [[ " ${UNIQUE[*]} " != *" $test "* ]]; then
        UNIQUE+=("$test")
    fi
done
SELECTED=("${UNIQUE[@]}")

if [[ -z "$BUILD_DIR" ]]; then
    BUILD_DIR="$ROOT/build-linux-validation/${CONFIG,,}"
elif [[ "$BUILD_DIR" != /* ]]; then
    BUILD_DIR="$ROOT/$BUILD_DIR"
fi

unset DISPLAY WAYLAND_DISPLAY

# Reconfiguration is incremental: an existing CMake cache and all compatible
# objects are retained. GUI support is enabled so every listed set, including
# Startup's editor subprocess test, is available from the same build tree.
cmake -S "$ROOT" -B "$BUILD_DIR" \
    -DCMAKE_BUILD_TYPE="$CONFIG" \
    -DBUILD_TESTING=ON \
    -DPF_BUILD_GUI=ON

TARGETS=()
REGEX_PARTS=()
for test in "${SELECTED[@]}"; do
    TARGETS+=("pf-smoke-$test")
    REGEX_PARTS+=("$test")
done

cmake --build "$BUILD_DIR" --config "$CONFIG" \
    --target "${TARGETS[@]}" --parallel "$JOBS"

regex="$(IFS='|'; printf '%s' "${REGEX_PARTS[*]}")"
ctest --test-dir "$BUILD_DIR" --build-config "$CONFIG" \
    -R "^smoke-($regex)$" --parallel "$JOBS" --output-on-failure
