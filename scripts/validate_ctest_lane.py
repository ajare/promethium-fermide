#!/usr/bin/env python3
"""Run an explicit smoke validation lane in an already built Linux/MSVC tree.

fast: project functional scenarios once, bounded CLI and artifact isolation.
final: unfiltered CTest, including expensive exhaustive eight-process contracts
and dependency tests. Default full CTest is deliberately not weakened.
"""
import argparse
import os
from pathlib import Path
import subprocess


def command(args):
    result = ['ctest', '--test-dir', str(args.build_tree), '-C', args.config,
              '--parallel', str(args.parallel), '--output-on-failure']
    if args.lane == 'fast':
        result += ['-L', '^validation-fast$']
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-tree', type=Path, required=True)
    parser.add_argument('--config', choices=('Debug', 'Release'), required=True)
    parser.add_argument('--lane', choices=('fast', 'final'), required=True)
    parser.add_argument('--parallel', type=int, default=8)
    parser.add_argument('--list', action='store_true', help='List the selected tests without running')
    args = parser.parse_args()
    if args.parallel < 1:
        parser.error('--parallel must be positive')
    cmd = command(args)
    if args.list:
        cmd += ['--show-only']
    print(subprocess.list2cmdline(cmd), flush=True)
    # CTest registrations own platform/config-specific bounded timeouts; a
    # command-line --timeout must not override the larger MSVC Debug budgets.
    env = os.environ.copy()
    env.pop('DISPLAY', None)
    env.pop('WAYLAND_DISPLAY', None)
    if os.name == 'nt':
        import ctypes
        ctypes.windll.kernel32.SetErrorMode(0x8003)
        env.update(SDL_ASSERT='abort', SDL_VIDEODRIVER='offscreen')
    return subprocess.run(cmd, env=env, timeout=7200).returncode


if __name__ == '__main__':
    raise SystemExit(main())
