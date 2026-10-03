"""Bounded public CLI and artifact isolation contracts (legacy scripts own inventories)."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import os
from pathlib import Path
import subprocess
import tempfile

# Selected for file/registry/package creation, not merely cheapest runtime.
# Simulation, Permissions, World and Render are in-memory: their shared
# Context artifact paths are additionally exercised through the harness probe.
REPRESENTATIVES = {
    'simulation': ['pauseOnStaircasePreservesPosition'],
    'permissions': ['authorizationAndPersistence'],
    'routing': ['savedIntentWaitsForRegistryResolution'],
    'transports': ['luaRandomnessIsIndependent'],
    'behaviours': ['savedWorldCreatesAndReopensAdjacentPackage'],
    'agent-tags': ['saveReopenAndUnknownTagValidationUseStableIds'],
    'editor': ['tags/savedWorldCreatesAndReopensAdjacentRegistry'],
    'persistence': ['yaml-file', 'transactional-concurrent'],
    'render': ['theRenderSnapshotIsDeterministic'],
    'world': ['marker-identity'],
    'agent': ['displayColourIsRandomAtCreationBackfilledAndPersisted'],
}


def invoke(binary, args, cwd, env, timeout, code=0):
    executable = binary if isinstance(binary, list) else [str(binary)]
    result = subprocess.run([*executable, *args], cwd=cwd, env=env,
                            capture_output=True, text=True, timeout=timeout)
    assert result.returncode == code, (args, result.returncode, result.stdout, result.stderr)
    if code == 2:
        assert not result.stdout and result.stderr.startswith('ERROR '), result
    else:
        assert not result.stderr, result.stderr
    return result.stdout


def selected(binary, module, check, cwd, env, timeout):
    out = invoke(binary, ['--check', check], cwd, env, timeout)
    assert out == f'PASS {module} {check}\nSUMMARY {module} pass=1 fail=0 skip=0\n', out


def contract(args):
    with tempfile.TemporaryDirectory(prefix='pf lane with spaces ') as directory:
        root = Path(directory)
        cwd, parent = root / 'cwd', root / 'temp parent'
        cwd.mkdir()
        parent.mkdir()
        env = os.environ.copy()
        env.update(TMPDIR=str(parent), TMP=str(parent), TEMP=str(parent))
        env.pop('DISPLAY', None)
        env.pop('WAYLAND_DISPLAY', None)
        checks = REPRESENTATIVES[args.module]
        assert all(check in args.names for check in checks), (args.module, checks)
        if args.lane == 'cli':
            listing = invoke(args.binary, ['--list'], cwd, env, args.timeout)
            assert listing == ''.join(name + '\n' for name in args.names), listing
            # One bounded selection verifies reporting without a full-module run.
            selected(args.binary, args.module, checks[0], cwd, env, args.timeout)
            for misuse in (['--bogus'], ['--check'], ['--check', 'absent'],
                           ['--check', ''], ['--list', 'extra'],
                           ['--check', checks[0], 'extra']):
                result = subprocess.run([str(args.binary), *misuse], cwd=cwd, env=env,
                                        capture_output=True, text=True, timeout=args.timeout)
                assert result.returncode == 2 and not result.stdout, result
                assert result.stderr.startswith(f'ERROR {args.module}:'), result.stderr
        else:
            # All children share cwd AND an otherwise empty OS temporary parent.
            # Probe paths writes artifacts, nests Contexts and reports cleaned roots.
            def worker(_):
                for check in checks:
                    selected(args.binary, args.module, check, cwd, env, args.timeout)
                out = invoke(args.probe, ['--check', 'paths'], cwd, env, args.timeout)
                lines = out.splitlines()
                assert len(lines) == 3 and lines[0].startswith('ROOT '), out
                assert lines[1:] == ['PASS harness paths', 'SUMMARY harness pass=1 fail=0 skip=0'], out
                return Path(lines[0][5:])
            with ThreadPoolExecutor(max_workers=4) as pool:
                roots = list(pool.map(worker, range(4)))
            assert len(set(roots)) == 4, f'Concurrent temporary roots collide: {roots}'
            assert all(path.parent == parent and not path.exists() for path in roots), roots
        assert not list(cwd.iterdir()), f'Working-directory artifacts: {list(cwd.iterdir())}'
        assert not list(parent.iterdir()), f'Temporary artifacts: {list(parent.iterdir())}'
    print(f'PASS {args.module} {args.lane} contract')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, required=True)
    parser.add_argument('--probe', type=Path, required=True)
    parser.add_argument('--module', choices=REPRESENTATIVES, required=True)
    parser.add_argument('--lane', choices=('cli', 'concurrency'), required=True)
    parser.add_argument('--timeout', type=int, default=240)
    parser.add_argument('names', nargs='+')
    contract(parser.parse_args())


if __name__ == '__main__':
    main()
