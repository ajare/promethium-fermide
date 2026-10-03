"""Shared validation supervisor. Only the supervisor owns locks and process trees.

Workers run in a Linux session or Windows kill-on-close Job Object. A Windows
worker is created suspended and assigned before it can spawn any descendants.
"""
import argparse
import ctypes
import json
import math
import os
import re
from pathlib import Path
import signal
import subprocess
import sys
import time
import uuid
import xml.etree.ElementTree as ET

WORKER = 'PF_VALIDATION_WORKER'


def positive(value):
    value = float(value)
    if not math.isfinite(value) or value <= 0:
        raise argparse.ArgumentTypeError('budget must be finite and positive')
    return value


def arguments(parser, default=7200):
    parser.add_argument('--run-timeout', type=positive, default=default,
                        help='whole invocation seconds (default: %(default)s)')
    parser.add_argument('--recover-stale', action='store_true',
                        help='acknowledge manual inspection of interrupted ownership; never overrides an active lock/group')
    parser.add_argument('--build-timeout', type=positive, default=1800,
                        help='each configure/build command seconds (default: %(default)s)')


def save(path, data):
    path = Path(path)
    temporary = path.with_suffix('.tmp')
    temporary.write_text(json.dumps(data, indent=2) + '\n', encoding='utf-8')
    temporary.replace(path)


class TreeLock:
    """OS lock, not PID liveness: PID reuse cannot steal an active run."""
    def __init__(self, tree):
        self.directory = Path(tree).resolve() / '.pf-validation'
        self.directory.mkdir(parents=True, exist_ok=True)
        self.stream = None

    def acquire(self, recover_stale=False):
        self.stream = (self.directory / 'lock').open('a+b')
        self.stream.seek(0)
        if os.name == 'nt':
            import msvcrt
            if not self.stream.read(1):
                self.stream.write(b'0')
                self.stream.flush()
            self.stream.seek(0)
            try:
                msvcrt.locking(self.stream.fileno(), msvcrt.LK_NBLCK, 1)
            except OSError:
                self.stream.close()
                self.stream = None
                raise RuntimeError(f'active validation owns {self.directory.parent}; inspect {self.directory}/owner.json; do not restart')
        else:
            import fcntl
            try:
                fcntl.flock(self.stream, fcntl.LOCK_EX | fcntl.LOCK_NB)
            except BlockingIOError:
                self.stream.close()
                self.stream = None
                raise RuntimeError(f'active validation owns {self.directory.parent}; inspect {self.directory}/owner.json; do not restart')
        owner = self.directory / 'owner.json'
        if owner.exists():
            previous = json.loads(owner.read_text())
            if previous.get('outcome') in ('running', 'cleanup-incomplete'):
                # After SIGKILL Linux descendants may retain the session. Never
                # kill a recorded PID/group: it could have been reused.
                pgid = previous.get('pgid')
                if os.name != 'nt' and pgid:
                    try:
                        os.killpg(pgid, 0)
                    except ProcessLookupError:
                        pass
                    else:
                        raise RuntimeError(f'prior owned process group {pgid} may remain active; inspect it manually before recovery; metadata: {owner}')
                if not recover_stale:
                    raise RuntimeError(f'stale interrupted owner: {owner}; OS lock is free, but detached descendants cannot be excluded after uncatchable termination. Inspect prior work, then use --recover-stale')
                print(f'Stale interrupted owner acknowledged (OS lock free, no prior group active): {owner}', flush=True)
        return self

    def close(self):
        if self.stream:
            self.stream.close()
            self.stream = None


def windows_job(process):
    """Assign a suspended process to a non-inheritable kill-on-close job."""
    from ctypes import wintypes as w
    kernel = ctypes.WinDLL('kernel32', use_last_error=True)
    class Basic(ctypes.Structure):
        _fields_ = [('per_process', ctypes.c_int64), ('per_job', ctypes.c_int64),
                    ('flags', w.DWORD), ('min_ws', ctypes.c_size_t), ('max_ws', ctypes.c_size_t),
                    ('active', w.DWORD), ('affinity', ctypes.c_size_t),
                    ('priority', w.DWORD), ('scheduling', w.DWORD)]
    class IO(ctypes.Structure):
        _fields_ = [(name, ctypes.c_uint64) for name in ('read_ops', 'write_ops', 'other_ops', 'read_bytes', 'write_bytes', 'other_bytes')]
    class Extended(ctypes.Structure):
        _fields_ = [('basic', Basic), ('io', IO), ('process_memory', ctypes.c_size_t),
                    ('job_memory', ctypes.c_size_t), ('peak_process', ctypes.c_size_t), ('peak_job', ctypes.c_size_t)]
    kernel.CreateJobObjectW.restype = w.HANDLE
    kernel.CreateJobObjectW.argtypes = [ctypes.c_void_p, w.LPCWSTR]
    kernel.SetInformationJobObject.argtypes = [w.HANDLE, ctypes.c_int, ctypes.c_void_p, w.DWORD]
    kernel.AssignProcessToJobObject.argtypes = [w.HANDLE, w.HANDLE]
    kernel.CloseHandle.argtypes = [w.HANDLE]
    job = kernel.CreateJobObjectW(None, None)
    info = Extended()
    info.basic.flags = 0x2000  # JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
    if not job or not kernel.SetInformationJobObject(job, 9, ctypes.byref(info), ctypes.sizeof(info)) or not kernel.AssignProcessToJobObject(job, int(process._handle)):
        if job:
            kernel.CloseHandle(job)
        process.kill()
        process.wait()
        raise ctypes.WinError(ctypes.get_last_error())
    # Popen closes the primary thread handle. NtResumeProcess resumes all threads
    # without a discovery race or opening unrelated process handles.
    resume = ctypes.WinDLL('ntdll').NtResumeProcess
    resume.argtypes = [w.HANDLE]
    resume.restype = ctypes.c_long
    if resume(int(process._handle)) != 0:
        kernel.CloseHandle(job)
        process.wait()
        raise RuntimeError('could not resume validation worker')
    return lambda: kernel.CloseHandle(job)


def supervise(args, trees):
    """Re-exec current entry point; return worker status, or None inside worker."""
    if os.environ.get(WORKER):
        return None
    locks = []
    process = None
    close_job = None
    outcome = 'failure'
    code = 1
    metadata = {'pid': os.getpid(), 'command': sys.argv, 'outcome': 'running'}
    old_handlers = {}
    def cancel(signum, frame):
        raise KeyboardInterrupt
    try:
        if sys.platform.startswith('linux'):
            # Reap orphaned grandchildren after killing our session (not other
            # people's children); avoids stale zombie groups after interruption.
            libc = ctypes.CDLL(None, use_errno=True)
            if libc.prctl(36, 1, 0, 0, 0) != 0:  # PR_SET_CHILD_SUBREAPER
                raise OSError(ctypes.get_errno(), 'cannot enable validation subreaper')
        for tree in sorted({str(Path(t).resolve()) for t in trees}):
            lock = TreeLock(tree)
            locks.append(lock)
            lock.acquire(args.recover_stale)
        evidence = locks[0].directory / ('run-' + uuid.uuid4().hex)
        evidence.mkdir()
        metadata['evidence'] = str(evidence)
        for lock in locks:
            save(lock.directory / 'owner.json', metadata)
        print(f'Validation log: {evidence / "output.log"}; budget {args.run_timeout}s. Outer tool budget must exceed this by 60s.', flush=True)
        env = dict(os.environ, **{WORKER: str(evidence), 'PF_VALIDATION_BUILD_TIMEOUT': str(args.build_timeout)})
        signals = [signal.SIGINT, signal.SIGTERM]
        if os.name == 'nt':
            signals.append(signal.SIGBREAK)
        for sig in signals:
            old_handlers[sig] = signal.signal(sig, cancel)
        with (evidence / 'output.log').open('w') as log:
            process = subprocess.Popen([sys.executable, *sys.argv], env=env, stdout=log,
                                       stderr=subprocess.STDOUT, start_new_session=os.name != 'nt',
                                       creationflags=0x4 if os.name == 'nt' else 0)
            if os.name == 'nt':
                close_job = windows_job(process)
            else:
                metadata['pgid'] = process.pid
            for lock in locks:
                save(lock.directory / 'owner.json', metadata)
            try:
                code = process.wait(timeout=args.run_timeout)
                outcome = 'success' if code == 0 else ('timeout' if code == 124 else 'failure')
                # Workers that run several steps must return nonzero on any
                # failed step; retained phase records identify which kind failed.
            except subprocess.TimeoutExpired:
                outcome, code = 'timeout', 124
    except KeyboardInterrupt:
        outcome, code = 'cancelled', 130
    except (RuntimeError, OSError, ValueError) as error:
        print(f'ERROR: {error}', file=sys.stderr)
    finally:
        for sig in old_handlers:
            signal.signal(sig, signal.SIG_IGN)
        # Also reap descendants left behind by a normally exiting worker.
        if close_job:
            close_job()
        elif process and os.name != 'nt':
            # Popen will not signal an already-reaped worker. Kill descendants
            # by adopted parent identity below, never a potentially reused PGID.
            process.kill()
        if process:
            if os.name == 'nt' and not close_job and process.poll() is None:
                process.kill()
            process.wait()
        if sys.platform.startswith('linux'):
            deadline = time.monotonic() + 5
            while time.monotonic() < deadline:
                # A descendant may create a new session. Subreaper adoption
                # still identifies it as ours; pidfds prevent PID-reuse kills.
                children = Path(f'/proc/self/task/{os.getpid()}/children').read_text().split()
                for child in children:
                    try:
                        fd = os.pidfd_open(int(child))
                    except ProcessLookupError:
                        continue
                    try:
                        status = Path(f'/proc/{child}/status').read_text()
                        if f'PPid:\t{os.getpid()}\n' in status:
                            signal.pidfd_send_signal(fd, signal.SIGKILL)
                    except (FileNotFoundError, ProcessLookupError):
                        pass
                    finally:
                        os.close(fd)
                try:
                    pid, _ = os.waitpid(-1, os.WNOHANG)
                except ChildProcessError:
                    break
                if pid == 0:
                    time.sleep(0.01)
            if Path(f'/proc/self/task/{os.getpid()}/children').read_text().strip():
                outcome, code = 'cleanup-incomplete', 1
                print('ERROR: owned descendants did not finish cleanup; inspect before recovery', file=sys.stderr)
        for sig, handler in old_handlers.items():
            signal.signal(sig, handler)
        if 'evidence' in metadata:
            metadata.update(outcome=outcome, exit_status=code)
            save(Path(metadata['evidence']) / 'result.json', metadata)
            for lock in locks:
                save(lock.directory / 'owner.json', metadata)
            print(f'Validation {outcome} (exit {code}); evidence: {metadata["evidence"]}', flush=True)
        for lock in reversed(locks):
            lock.close()
    return code


def run(command, **kwargs):
    """Worker command budgets; supervisor cleans the entire tree on timeout."""
    build = command[0] == 'cmake'
    kwargs.setdefault('timeout', float(os.environ.get('PF_VALIDATION_BUILD_TIMEOUT', 1800)) if build else 7200)
    evidence = os.environ.get(WORKER)
    if evidence:
        # Distinct files support the runtime validator's concurrent children.
        phase = Path(evidence) / ('phase-' + uuid.uuid4().hex + '.json')
        save(phase, {'command': command, 'outcome': 'running', 'timeout': kwargs['timeout']})
    try:
        result = subprocess.run(command, **kwargs)
    except subprocess.TimeoutExpired as error:
        if evidence:
            parts = [error.stdout or '', error.stderr or '']
            partial = ''.join(p.decode('utf-8', errors='replace') if isinstance(p, bytes) else p for p in parts)
            phase.with_suffix('.log').write_text(partial, encoding='utf-8')
            save(phase, {'command': command, 'outcome': 'build-timeout' if build else 'subprocess-timeout'})
            # Do not wait for ThreadPoolExecutor shutdown or leak descendants.
            os._exit(124)
        raise
    except subprocess.CalledProcessError as error:
        if evidence:
            save(phase, {'command': command, 'outcome': 'build-failure' if build else 'command-failure',
                         'exit_status': error.returncode})
        raise
    if evidence:
        save(phase, {'command': command, 'outcome': 'success' if result.returncode == 0 else ('build-failure' if build else 'command-failure'), 'exit_status': result.returncode})
    return result


def output(command, **kwargs):
    return run(command, stdout=subprocess.PIPE, check=True, **kwargs).stdout


def ctest(command, build, config, rerun=False, xml=None, **kwargs):
    """Only a complete JUnit inventory can authorize focused recovery."""
    cache = Path(build) / 'CMakeCache.txt'
    if cache.exists():
        match = re.search(r'^CMAKE_BUILD_TYPE:STRING=(.+)$', cache.read_text(), re.MULTILINE)
        if match and match.group(1) != config:
            raise RuntimeError(f'configured build type {match.group(1)} does not match {config}')
    directory = Path(build).resolve() / '.pf-validation'
    directory.mkdir(parents=True, exist_ok=True)
    inventory_file = directory / 'failed-tests.json'
    if rerun:
        if not inventory_file.exists():
            raise RuntimeError('no completed failure inventory; run the original suite first')
        prior = json.loads(inventory_file.read_text())
        if prior.get('config') != config or prior.get('build') != str(Path(build).resolve()) or prior.get('outcome') != 'test-failure' or not prior.get('failed'):
            raise RuntimeError('failure inventory is empty, incomplete, or for a different configuration')
        prior_xml = Path(prior.get('xml', ''))
        if not prior_xml.is_file():
            raise RuntimeError('missing JUnit evidence for failure inventory')
        prior_cases = ET.parse(prior_xml).getroot().findall('testcase')
        if sorted(c.attrib['name'] for c in prior_cases if c.find('failure') is not None) != sorted(prior['failed']):
            raise RuntimeError('failure inventory disagrees with retained JUnit evidence')
        # Explicit names avoid CTest LastTestsFailed.log from another config/run.
        command += ['-R', '^(' + '|'.join(re.escape(n) for n in prior['failed']) + ')$']
    inventory = json.loads(output(command + ['--show-only=json-v1'], text=True, timeout=30,
                                  env=kwargs.get('env')))['tests']
    names = [t['name'] for t in inventory]
    if not names or (rerun and set(names) != set(prior['failed'])):
        raise RuntimeError('empty or changed CTest recovery selection')
    save(inventory_file, {'outcome': 'incomplete', 'config': config, 'build': str(Path(build).resolve())})
    xml = xml or directory / ('ctest-' + uuid.uuid4().hex + '.xml')
    result = run(command + ['--output-junit', str(xml)], **kwargs)
    if not xml.exists():
        raise RuntimeError('CTest did not produce completion evidence')
    cases = ET.parse(xml).getroot().findall('testcase')
    if sorted(c.attrib['name'] for c in cases) != sorted(names):
        raise RuntimeError('CTest inventory incomplete')
    if any((c.attrib.get('status') == 'notrun' and c.find('skipped') is None)
           or 'not run' in (c.findtext('failure', '')).lower() for c in cases):
        raise RuntimeError('CTest contains unexecuted tests; not a completed failure inventory')
    failed = [c.attrib['name'] for c in cases if c.find('failure') is not None]
    if result.returncode and not failed:
        raise RuntimeError('CTest failed without a completed failure inventory')
    save(inventory_file, {'outcome': 'test-failure' if failed else ('focused-success' if rerun else 'success'),
                          'config': config, 'build': str(Path(build).resolve()), 'failed': failed, 'xml': str(xml)})
    if failed and result.returncode == 0:
        raise RuntimeError('CTest failure evidence disagrees with exit status')
    if rerun and result.returncode == 0:
        print('Focused recovery passed; NOT final verification. Run the full required final lane on final sources.', flush=True)
    return result.returncode
