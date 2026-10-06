#!/usr/bin/env python3
"""Audit and run the complete configured MSVC CTest inventory (#312).

First configure/build fresh trees with validate_windows_smoke.py. This validator
builds the default inventory too, never filters CTest, and always selects -C.
Evidence (including verbose per-check skip reasons) stays in ctest-validation/.
"""
import argparse
import ctypes
import json
import os
import re
from pathlib import Path
import subprocess
import sys
import time
import xml.etree.ElementTree as ET
import validation_run as validation

CORE = {'agent', 'agent-tags', 'behaviours', 'permissions', 'persistence',
        'routing', 'simulation', 'transports', 'world'}
REPRESENTATIVES = {
    'pf-smoke-agent': 'src/headless/smoke/agent/Identity.cpp',
    'pf-smoke-render': 'src/headless/smoke/render/Walls.cpp',
    'pf-smoke-editor': 'src/headless/smoke/editor/Isolation.cpp',
}


def run(command, log, env=None):
    print(subprocess.list2cmdline(command), flush=True)
    start = time.monotonic()
    with log.open('w', encoding='utf-8') as stream:
        if command[0] == 'ctest':
            command = command.copy()
            index = command.index('--output-junit')
            xml = Path(command[index + 1])
            del command[index:index + 2]
            code = validation.ctest(command, command[command.index('--test-dir') + 1],
                                    command[command.index('-C') + 1], xml=xml,
                                    stdout=stream, stderr=subprocess.STDOUT, env=env)
        else:
            code = validation.run(command, stdout=stream, stderr=subprocess.STDOUT,
                                  env=env).returncode
    if code:
        raise RuntimeError(f'command failed ({code}); see {log}')
    return round(time.monotonic() - start, 2)


def audit_inventory(inventory, gui, config):
    modules = CORE | {'render', 'editor', 'metrics'} | ({'startup'} if gui else set())
    tests = inventory['tests']
    names = [test['name'] for test in tests]
    if len(names) != len(set(names)):
        raise RuntimeError('duplicate CTest names')
    direct = {}
    for test in tests:
        command = test.get('command', [])
        if not command or not Path(command[0]).is_file():
            raise RuntimeError(f"missing required CTest product: {test['name']}: {command}")
        product = Path(command[0]).stem
        if product == 'promethium-fermide-headless':
            raise RuntimeError('compatibility aggregate duplicates direct coverage')
        if product.startswith('pf-smoke-') and product != 'pf-smoke-harness-probe':
            module = product.removeprefix('pf-smoke-')
            if module not in modules or module in direct or len(command) != 1:
                raise RuntimeError(f'unexpected direct module command: {command}')
            properties = {item['name']: item['value'] for item in test['properties']}
            expected = {'smoke', 'core'} if module in CORE else {'smoke', module}
            if module == 'metrics':
                expected = {'smoke', 'metrics', 'http'}
            if module == 'startup':
                expected = {'smoke', 'startup', 'graphics', 'subprocess', 'gui'}
            expected |= {'functional', 'validation-fast'}
            if test['name'] != f'smoke-{module}' or set(properties.get('LABELS', [])) != expected:
                raise RuntimeError(f'wrong module name/labels: {test}')
            if Path(command[0]).parent.name != config:
                raise RuntimeError(f'wrong product configuration: {command}')
            direct[module] = command[0]
    if set(direct) != modules:
        raise RuntimeError(f'missing direct modules: {modules - direct.keys()}')
    return direct


def file_api(build, config):
    reply = build / '.cmake/api/v1/reply'
    index = json.loads(max(reply.glob('index-*.json')).read_text())
    model = json.loads((reply / index['reply']['codemodel-v2']['jsonFile']).read_text())
    refs = next(c for c in model['configurations'] if c['name'] == config)['targets']
    return {ref['id']: json.loads((reply / ref['jsonFile']).read_text()) for ref in refs}


def audit_dependencies(targets):
    by_name = {t['name']: t for t in targets.values()}
    evidence = {}
    for name in REPRESENTATIVES:
        seen, pending = set(), [by_name[name]['id']]
        while pending:
            identity = pending.pop()
            if identity in seen:
                continue
            seen.add(identity)
            pending.extend(d['id'] for d in targets[identity].get('dependencies', []))
        closure = [targets[i] for i in seen]
        sources = {}
        for target in closure:
            for source in target.get('sources', []):
                if 'compileGroupIndex' not in source:
                    continue
                path = source['path']
                if path in sources:
                    raise RuntimeError(f'duplicate compiled implementation: {path}')
                sources[path] = target['name']
        dependencies = {t['name'] for t in closure if t['type'] != 'UTILITY'} - {name}
        if name == 'pf-smoke-agent':
            allowed = {'pf-smoke-support', 'promethium-fermide-core', 'pf-lua', 'yaml-cpp'}
            if not dependencies <= allowed:
                raise RuntimeError(f'core dependency boundary violated: {dependencies - allowed}')
        else:
            required = {'pf-render', 'pf-imgui-cpu', 'pf-headless-render-support'}
            if name == 'pf-smoke-editor':
                required.add('pf-agent-editing')
            if not required <= dependencies:
                raise RuntimeError(f'missing compiled production support: {required - dependencies}')
            own = [p for p, owner in sources.items() if owner == name]
            if any(not p.startswith('src/headless/') for p in own):
                raise RuntimeError(f'{name} compiles production implementations: {own}')
        links = by_name[name].get('link', {}).get('commandFragments', [])
        if name == 'pf-smoke-agent':
            inputs = ' '.join(item['fragment'] for item in links).lower()
            forbidden = ('imgui', 'pf-render', 'pf-agent-editing', 'opengl',
                         'glew', 'sdl', 'httplib', 'ws2_32', 'mpp', 'willpower')
            if any(library in inputs for library in forbidden):
                raise RuntimeError(f'core link inputs violate dependency boundary: {inputs}')
        evidence[name] = {'dependencies': sorted(dependencies), 'sources': sources,
                          'link': links}
    return evidence


def audit_results(suite, inventory, checks):
    cases = suite.findall('testcase')
    if sorted(t.attrib['name'] for t in cases) != sorted(t['name'] for t in inventory['tests']):
        raise RuntimeError('CTest did not execute the full configured inventory')
    skips, reasons, check_skips = [], {}, []
    for case in cases:
        name = case.attrib['name']
        output = case.findtext('system-out', '')
        if case.find('failure') is not None:
            raise RuntimeError(f'failed CTest: {name}')
        if case.find('skipped') is not None:
            reason = 'Could not load EGL library'
            if name != 'willpower_resource_manager_gui_smoke' or reason not in output:
                raise RuntimeError(f'unexpected CTest skip: {name}: {output}')
            skips.append(name)
            reasons[name] = 'Optional headless offscreen graphics unavailable: ' + reason
        module = name.removeprefix('smoke-')
        if module not in checks:
            continue
        records = re.findall(r'^(PASS|SKIP|FAIL) ' + re.escape(module) + r' ([^:\n]+)(?:: (.*))?$',
                             output, re.MULTILINE)
        if [record[1] for record in records] != checks[module]:
            raise RuntimeError(f'check execution disagrees with registry: {module}')
        for status, check, reason in records:
            if status == 'FAIL':
                raise RuntimeError(f'failed module check: {module}/{check}: {reason}')
            if status == 'SKIP':
                allowed = {'transactional-symlink-target', 'transactional-symlink-temp',
                           'transactional-permissions'}
                if module != 'persistence' or check not in allowed or reason != 'POSIX symlink/permission semantics unavailable':
                    raise RuntimeError(f'unexpected check skip: {module}/{check}: {reason}')
                check_skips.append({'module': module, 'check': check, 'reason': reason})
    return {'skipped_tests': skips, 'skip_reasons': reasons, 'skipped_checks': check_skips}


def object_snapshot(build, config):
    return {str(p.relative_to(build)): p.stat().st_mtime_ns
            for p in (build / 'src/headless/smoke').rglob('*.obj')
            if config in p.parts}


def incremental(build, config, logs, source, parallel):
    evidence = {}
    for owner, relative in REPRESENTATIVES.items():
        path = source / relative
        stat = path.stat()
        before = object_snapshot(build, config)
        product = build / 'bin/x64' / config / (owner + '.exe')
        product_time = product.stat().st_mtime_ns
        try:
            path.touch()
            elapsed = run(['cmake', '--build', str(build), '--config', config,
                           '--parallel', str(parallel)], logs / f'{config}-touch-{owner}.log')
        finally:
            os.utime(path, ns=(stat.st_atime_ns, stat.st_mtime_ns))
        after = object_snapshot(build, config)
        changed = [p for p, stamp in after.items() if before.get(p) != stamp]
        expected = Path(relative).stem + '.obj'
        if not changed or any(owner + '.dir' not in Path(p).parts or
                              Path(p).name != expected for p in changed):
            raise RuntimeError(f'{owner}: unexpected rebuilt module objects: {changed}')
        if product.stat().st_mtime_ns == product_time:
            raise RuntimeError(f'{owner}: owning executable did not relink')
        evidence[owner] = {'source': relative, 'changed_objects': changed,
                           'relinked': str(product), 'seconds': elapsed}
    return evidence


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-tree', type=Path, required=True)
    parser.add_argument('--gui', choices=('on', 'off'), required=True)
    parser.add_argument('--parallel', type=int, default=8)
    validation.arguments(parser, default=14400)
    args = parser.parse_args()
    if sys.platform != 'win32' or args.parallel < 2:
        parser.error('requires Windows/MSVC and parallelism >= 2')
    # Inherited by native children, including vendored headless test binaries.
    ctypes.windll.kernel32.SetErrorMode(0x8003)
    build = args.build_tree.resolve()
    source = Path(__file__).resolve().parents[1]
    status = validation.supervise(args, [build])
    if status is not None:
        return status
    logs = build / 'ctest-validation'
    logs.mkdir(exist_ok=True)
    env = os.environ.copy()
    env.update(SDL_ASSERT='abort', SDL_VIDEODRIVER='offscreen')
    env.pop('DISPLAY', None)
    env.pop('WAYLAND_DISPLAY', None)
    results = {}
    for config in ('Debug', 'Release'):
        run(['cmake', '--build', str(build), '--config', config, '--parallel', '4'],
            logs / f'{config}-default-build.log')
        inventory = json.loads(validation.output(
            ['ctest', '--test-dir', str(build), '-C', config, '--show-only=json-v1'], text=True, timeout=30, env=env))
        (logs / f'{config}-inventory.json').write_text(json.dumps(inventory, indent=2) + '\n')
        direct = audit_inventory(inventory, args.gui == 'on', config)
        checks = {}
        for module, exe in direct.items():
            checks[module] = validation.output([exe, '--list'], text=True, timeout=30).splitlines()
        dependencies = audit_dependencies(file_api(build, config))
        (logs / f'{config}-dependencies.json').write_text(json.dumps(dependencies, indent=2) + '\n')
        results[config] = {'tests': len(inventory['tests']), 'checks': checks, 'runs': {}}
        for kind, jobs in (('sequential', 1), ('parallel', args.parallel)):
            xml = logs / f'{config}-{kind}.xml'
            elapsed = run(['ctest', '--test-dir', str(build), '-C', config,
                           '--output-on-failure', '--verbose', '-j', str(jobs),
                           '--test-output-size-passed', '1048576',
                           '--test-output-size-failed', '1048576', '--output-junit', str(xml)],
                          logs / f'{config}-{kind}.log', env)
            suite = ET.parse(xml).getroot()
            results[config]['runs'][kind] = {'seconds': elapsed,
                                            **audit_results(suite, inventory, checks)}
        results[config]['incremental'] = incremental(build, config, logs, source, 4)
        (logs / 'results.json').write_text(json.dumps(results, indent=2) + '\n')
    return 0


if __name__ == '__main__':
    try:
        sys.exit(main())
    except (RuntimeError, subprocess.SubprocessError) as error:
        print(error, file=sys.stderr)
        sys.exit(1)
