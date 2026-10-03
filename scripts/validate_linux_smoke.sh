#!/usr/bin/env bash
# Keep the established Linux entry point; Python owns budgets and child cleanup.
set -euo pipefail
ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
exec python3 "$ROOT/scripts/validate_linux_smoke.py" "$@"
