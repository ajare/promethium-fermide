#!/usr/bin/env python3
"""Incremental Linux smoke validation (functional fast / expensive final lanes)."""
import argparse
import os
from pathlib import Path
import sys
import validation_run as validation

MODULES = 'agent agent-tags behaviours editor metrics permissions persistence render routing simulation startup transports world'.split()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--config', choices=('Debug', 'Release'))
    parser.add_argument('--build-dir', type=Path)
    parser.add_argument('--lane', default='fast', metavar='fast|final')
    parser.add_argument('--list', action='store_true')
    parser.add_argument('--build-only', action='store_true', help='build selected targets without tests; final all builds the default inventory')
    parser.add_argument('--rerun-failed', action='store_true', help='focused feedback only; no build or final verification')
    parser.add_argument('modules', nargs='*', metavar='TEST')
    validation.arguments(parser)
    args = parser.parse_args()
    if args.list:
        parser.print_help()
        print('Available smoke-test sets: ' + ' '.join(MODULES) + ' all')
        return 0
    if args.build_only and args.rerun_failed:
        parser.error('--build-only cannot be combined with --rerun-failed')
    if args.lane not in ('fast', 'final'):
        parser.error('--lane must be fast or final')
    if not sys.platform.startswith('linux') or not args.config:
        parser.error('requires Linux and --config Debug|Release')
    if not args.modules or ('all' in args.modules and args.modules != ['all']):
        parser.error('specify test sets, or all alone')
    modules = MODULES if args.modules == ['all'] else list(dict.fromkeys(args.modules))
    if set(modules) - set(MODULES):
        parser.error('unknown smoke-test set')
    root = Path(__file__).resolve().parents[1]
    build = args.build_dir or root / 'build-linux-validation' / args.config.lower()
    if not build.is_absolute():
        build = root / build
    status = validation.supervise(args, [build])
    if status is not None:
        return status
    env = os.environ.copy()
    env.pop('DISPLAY', None)
    env.pop('WAYLAND_DISPLAY', None)
    os.environ.pop('DISPLAY', None)
    os.environ.pop('WAYLAND_DISPLAY', None)
    jobs = os.environ.get('PF_VALIDATION_JOBS', str(os.cpu_count() or 1))
    if not args.rerun_failed:
        validation.run(['cmake', '-S', str(root), '-B', str(build),
                        '-DCMAKE_BUILD_TYPE=' + args.config, '-DBUILD_TESTING=ON', '-DPF_BUILD_GUI=ON'], env=env, check=True)
        targets = [] if args.lane == 'final' and args.modules == ['all'] else [
            '--target', 'pf-smoke-harness-probe', *('pf-smoke-' + m for m in modules)]
        validation.run(['cmake', '--build', str(build), '--config', args.config,
                        *targets, '--parallel', jobs], env=env, check=True)
    if args.build_only:
        print('Build complete; tests were not requested (NOT final verification).', flush=True)
        return 0
    cmd = ['ctest', '--test-dir', str(build), '-C', args.config, '--parallel', jobs, '--output-on-failure']
    if not args.rerun_failed:
        cmd += ['-R', '^smoke-(' + '|'.join(modules) + ')($|(-cli|-concurrency)?-contract$)']
        if args.lane == 'fast':
            cmd += ['-L', '^validation-fast$']
    return validation.ctest(cmd, build, args.config, args.rerun_failed)


if __name__ == '__main__':
    raise SystemExit(main())
