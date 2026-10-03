"""Short real process-tree/locking and CTest recovery regressions, Linux/Windows."""
import importlib.util
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time
import unittest
from unittest.mock import patch

SCRIPTS = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('validation_owner', SCRIPTS / 'validation_run.py')
v = importlib.util.module_from_spec(spec)
spec.loader.exec_module(v)

WORKLOAD = '''import argparse, os, pathlib, subprocess, sys, time
sys.path.insert(0, SCRIPTS)
import validation_run as v
p = argparse.ArgumentParser()
p.add_argument('--tree', required=True)
p.add_argument('--delay', type=float, default=30)
p.add_argument('--build-child', action='store_true')
v.arguments(p)
a = p.parse_args()
r = v.supervise(a, [a.tree])
if r is not None: sys.exit(r)
child = subprocess.Popen([sys.executable, '-c', 'import time; time.sleep(60)'], start_new_session=sys.platform.startswith('linux'))
pathlib.Path(a.tree, 'child.pid').write_text(str(child.pid))
pathlib.Path(a.tree, 'started').touch()
if a.build_child:
    v.run([sys.executable, '-c', 'import time; time.sleep(60)'], timeout=0.2)
time.sleep(a.delay)
'''


def alive(pid):
    if sys.platform.startswith('linux'):
        stat = Path(f'/proc/{pid}/stat')
        if stat.exists() and stat.read_text().split(') ')[1].startswith('Z'):
            return False
    if os.name == 'nt':
        import ctypes
        k = ctypes.WinDLL('kernel32')
        k.OpenProcess.restype = ctypes.c_void_p
        k.CloseHandle.argtypes = [ctypes.c_void_p]
        handle = k.OpenProcess(0x1000, False, pid)
        if not handle:
            return False
        code = ctypes.c_ulong()
        k.GetExitCodeProcess.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
        k.GetExitCodeProcess(handle, ctypes.byref(code))
        k.CloseHandle(handle)
        return code.value == 259
    try:
        os.kill(pid, 0)
        return True
    except ProcessLookupError:
        return False


class Lifecycle(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='validation ownership ')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.script = self.root / 'worker.py'
        self.script.write_text('SCRIPTS = ' + repr(str(SCRIPTS)) + '\n' + WORKLOAD)

    def launch(self, tree, *args):
        env = os.environ.copy()
        env.pop(v.WORKER, None)
        p = subprocess.Popen([sys.executable, str(self.script), '--tree', str(tree), *args],
                             stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, env=env,
                             creationflags=subprocess.CREATE_NEW_PROCESS_GROUP if os.name == 'nt' else 0)
        self.addCleanup(lambda: p.poll() is None and p.kill())
        return p

    def wait_started(self, tree):
        deadline = time.monotonic() + 10
        while not (tree / 'started').exists():
            if time.monotonic() > deadline:
                self.fail('worker did not start')
            time.sleep(0.02)
        return int((tree / 'child.pid').read_text())

    def assert_dead(self, pid):
        deadline = time.monotonic() + 5
        while alive(pid) and time.monotonic() < deadline:
            time.sleep(0.02)
        self.assertFalse(alive(pid))

    def test_timeout_cleans_children_and_preserves_unrelated(self):
        unrelated = subprocess.Popen([sys.executable, '-c', 'import time; time.sleep(60)'])
        self.addCleanup(lambda: (unrelated.terminate(), unrelated.wait()))
        tree = self.root / 'tree'
        process = self.launch(tree, '--run-timeout', '1')
        child = self.wait_started(tree)
        output = process.communicate(timeout=12)[0]
        self.assertEqual(process.returncode, 124, output)
        self.assert_dead(child)
        self.assertIsNone(unrelated.poll())
        owner = json.loads((tree / '.pf-validation/owner.json').read_text())
        self.assertEqual(owner['outcome'], 'timeout')
        self.assertTrue((Path(owner['evidence']) / 'output.log').exists())

    def test_cancellation_cleanup(self):
        tree = self.root / 'tree'
        process = self.launch(tree)
        child = self.wait_started(tree)
        process.send_signal(signal.SIGTERM)
        output = process.communicate(timeout=12)[0]
        if os.name == 'nt':
            # TerminateProcess is uncatchable; closing the supervisor's Job
            # handle in the OS still kills workers and descendants.
            self.assertNotEqual(process.returncode, 0, output)
        else:
            self.assertEqual(process.returncode, 130, output)
        self.assert_dead(child)

    @unittest.skipUnless(os.name == 'nt', 'Windows console cancellation')
    def test_windows_managed_console_cancellation(self):
        import ctypes
        if not ctypes.windll.kernel32.GetConsoleWindow():
            self.skipTest('no Windows console for CTRL_BREAK_EVENT')
        tree = self.root / 'tree'
        process = self.launch(tree)
        child = self.wait_started(tree)
        process.send_signal(subprocess.CTRL_BREAK_EVENT)
        output = process.communicate(timeout=12)[0]
        self.assertEqual(process.returncode, 130, output)
        self.assert_dead(child)

    def test_nested_timeout_cleans_entire_tree(self):
        tree = self.root / 'tree'
        process = self.launch(tree, '--build-child')
        child = self.wait_started(tree)
        self.assertEqual(process.wait(timeout=12), 124)
        self.assert_dead(child)
        process.communicate()

    def test_duplicate_rejected_and_separate_tree_allowed(self):
        tree = self.root / 'tree'
        first = self.launch(tree, '--run-timeout', '3')
        self.wait_started(tree)
        second = self.launch(tree)
        output = second.communicate(timeout=10)[0]
        self.assertEqual(second.returncode, 1)
        self.assertIn('active validation owns', output)
        other = self.launch(self.root / 'other', '--delay', '0.1')
        self.assertEqual(other.wait(timeout=10), 0)
        other.communicate()
        first.communicate(timeout=12)

    def test_stale_owner_recovery(self):
        tree = self.root / 'tree'
        directory = tree / '.pf-validation'
        directory.mkdir(parents=True)
        v.save(directory / 'owner.json', {'outcome': 'running', 'pid': 99999999, 'pgid': 99999999})
        refused = self.launch(tree, '--delay', '0.1')
        self.assertIn('--recover-stale', refused.communicate(timeout=10)[0])
        self.assertEqual(refused.returncode, 1)
        process = self.launch(tree, '--delay', '0.1', '--recover-stale')
        output = process.communicate(timeout=10)[0]
        self.assertEqual(process.returncode, 0, output)
        self.assertIn('Stale interrupted owner', output)

    @unittest.skipUnless(sys.platform.startswith('linux'), 'Linux SIGKILL recovery')
    def test_uncatchable_parent_death_blocks_live_group(self):
        tree = self.root / 'tree'
        process = self.launch(tree)
        child = self.wait_started(tree)
        owner = json.loads((tree / '.pf-validation/owner.json').read_text())
        process.kill()
        process.communicate(timeout=5)
        try:
            second = self.launch(tree)
            output = second.communicate(timeout=10)[0]
            self.assertEqual(second.returncode, 1, output)
            self.assertIn('may remain active', output)
            self.assertTrue(alive(child))
        finally:
            os.killpg(owner['pgid'], signal.SIGKILL)
            os.kill(child, signal.SIGKILL)


class Budgets(unittest.TestCase):
    def test_build_and_nested_limits_are_distinct(self):
        env = os.environ.copy()
        env.pop(v.WORKER, None)
        env['PF_VALIDATION_BUILD_TIMEOUT'] = '0.3'
        with patch.dict(os.environ, env, clear=True), patch.object(v.subprocess, 'run', return_value=subprocess.CompletedProcess([], 0)) as child:
            v.run(['cmake', '--build', 'tree'])
            self.assertEqual(child.call_args.kwargs['timeout'], 0.3)
            v.run(['ctest'])
            self.assertEqual(child.call_args.kwargs['timeout'], 7200)
            v.run(['contract'], timeout=0.1)
            self.assertEqual(child.call_args.kwargs['timeout'], 0.1)
        with tempfile.TemporaryDirectory() as directory, patch.dict(os.environ, {v.WORKER: directory}), patch.object(v.subprocess, 'run', side_effect=subprocess.CalledProcessError(3, ['cmake'])):
            with self.assertRaises(subprocess.CalledProcessError):
                v.run(['cmake'], check=True)
            phase = next(Path(directory).glob('phase-*.json'))
            self.assertEqual(json.loads(phase.read_text())['outcome'], 'build-failure')
            self.assertEqual(json.loads(phase.read_text())['exit_status'], 3)
        for value in ('nan', 'inf', '0', '-1'):
            with self.assertRaises(v.argparse.ArgumentTypeError):
                v.positive(value)


class Recovery(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.build = Path(self.temp.name)
        (self.build / 'CMakeCache.txt').write_text('CMAKE_BUILD_TYPE:STRING=Debug\n')
        script = self.build / 'case.py'
        script.write_text('import pathlib, sys\nsys.exit(0 if pathlib.Path(__file__).with_name("repaired").exists() else 1)\n')
        exe = Path(sys.executable).as_posix()
        (self.build / 'CTestTestfile.cmake').write_text(
            f'add_test(broken "{exe}" "{script.as_posix()}")\n'
            f'add_test(good "{exe}" "-c" "pass")\n'
            f'add_test(optional "{exe}" "-c" "import sys; sys.exit(77)")\n'
            'set_tests_properties(optional PROPERTIES SKIP_RETURN_CODE 77)\n')
        self.cmd = ['ctest', '--test-dir', str(self.build), '-C', 'Debug', '--output-on-failure']

    def test_completed_failure_focused_repair_and_final(self):
        self.assertNotEqual(v.ctest(self.cmd.copy(), self.build, 'Debug'), 0)
        inventory = self.build / '.pf-validation/failed-tests.json'
        self.assertEqual(json.loads(inventory.read_text())['failed'], ['broken'])
        (self.build / 'repaired').touch()
        self.assertEqual(v.ctest(self.cmd.copy(), self.build, 'Debug', True), 0)
        self.assertEqual(json.loads(inventory.read_text())['outcome'], 'focused-success')
        with self.assertRaises(RuntimeError):
            v.ctest(self.cmd.copy(), self.build, 'Debug', True)
        self.assertEqual(v.ctest(self.cmd.copy(), self.build, 'Debug'), 0)
        self.assertEqual(json.loads(inventory.read_text())['outcome'], 'success')

    def test_missing_incomplete_empty_wrong_config_inventory(self):
        with self.assertRaises(RuntimeError):
            v.ctest(self.cmd.copy(), self.build, 'Debug', True)
        directory = self.build / '.pf-validation'
        for state, config, failed in [('incomplete', 'Debug', ['broken']),
                                      ('test-failure', 'Release', ['broken']),
                                      ('test-failure', 'Debug', []),
                                      ('test-failure', 'Debug', ['missing'])]:
            v.save(directory / 'failed-tests.json', dict(outcome=state, config=config,
                   failed=failed, build=str(self.build.resolve())))
            with self.assertRaises(RuntimeError):
                v.ctest(self.cmd.copy(), self.build, 'Debug', True)
        with self.assertRaisesRegex(RuntimeError, 'build type'):
            v.ctest(self.cmd.copy(), self.build, 'Release')

    def test_incomplete_suite_cannot_authorize_recovery(self):
        def incomplete(command, **kwargs):
            xml = Path(command[command.index('--output-junit') + 1])
            xml.write_text('<testsuite><testcase name="broken"><failure/></testcase></testsuite>')
            return subprocess.CompletedProcess(command, 8)
        with patch.object(v, 'output', return_value=json.dumps({'tests': [{'name': 'broken'}, {'name': 'good'}]})), patch.object(v, 'run', side_effect=incomplete):
            with self.assertRaisesRegex(RuntimeError, 'incomplete'):
                v.ctest(self.cmd.copy(), self.build, 'Debug')
        inventory = self.build / '.pf-validation/failed-tests.json'
        self.assertEqual(json.loads(inventory.read_text())['outcome'], 'incomplete')
        with self.assertRaises(RuntimeError):
            v.ctest(self.cmd.copy(), self.build, 'Debug', True)

    def test_empty_selection_is_not_success(self):
        with self.assertRaisesRegex(RuntimeError, 'empty'):
            v.ctest(self.cmd + ['-R', '^absent$'], self.build, 'Debug')


if __name__ == '__main__':
    unittest.main()
