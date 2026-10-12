#!/usr/bin/env python3
"""End-to-end tests for the KCPC binary frontend server (beads-3o1).

Each test starts a real core, finds the binary port the way a frontend must
(`ports.binary` of the startup manifest, never a hardcoded 6545) and talks to
it with kcpc_client.py. The suite grows with every slice of
docs/protocol/kcpc-binary-v1.md. Slice 1: framing, HELLO, PING,
MACHINE_INFO, ERROR, the client limit, the manifest line, the
[ipc] binary_port_enabled key and shutdown with clients connected. Slice 2:
the machine reads REGS_GET, MEM_INFO and MEM_READ, checked against the text
IPC, on a running and on a paused machine.

Usage: test_kcpc.py [--ui-free | --gui] [-k substring] [path/to/koncepcja]
  (default)  the binary is started with -H
  --ui-free  the binary is a KONCPC_MODERN_UI=0 build: started without -H,
             it is headless by construction
  --gui      the GUI build under SDL's dummy drivers: a Z80 thread and a
             render thread, so the server is exercised against the threaded
             loop too
  -k TEXT    run only the tests whose name contains TEXT
The binary defaults to ./koncepcja, or $KONCPC_EXE when set.
Exit status 0 when every test passed.
"""

import http.client
import os
import re
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time
import traceback
from pathlib import Path
from typing import Callable, Dict, List, Optional, Tuple

sys.path.insert(0, str(Path(__file__).resolve().parent))
import kcpc_client as kc  # noqa: E402
from kcpc_client import KcpcClient, KcpcClosed, KcpcError  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent.parent.parent

MANIFEST_TIMEOUT_S = 30.0
IO_TIMEOUT_S = 10.0
EXIT_TIMEOUT_S = 10.0

MODE = 'headless'  # 'headless', 'ui-free' or 'gui'; set by main()
EXE = ROOT / 'koncepcja'


class Core:
    """One emulator process and what its startup manifest said."""

    def __init__(self, *extra_args: str):
        self.tmpdir = tempfile.mkdtemp(prefix='koncpc-kcpc-')
        # A throwaway copy of the shipped example config: never whoever's
        # koncepcja.cfg sits in the working directory.
        cfg = Path(self.tmpdir) / 'koncepcja.cfg'
        shutil.copyfile(ROOT / 'koncepcja.cfg.example', cfg)
        env = os.environ.copy()
        env['KONCPC_NO_DIALOGS'] = '1'
        env['KONCPC_TEST_WINDOW'] = '1'
        cmd = [str(EXE)]
        if MODE == 'headless':
            cmd.append('-H')
        elif MODE == 'gui':
            env['SDL_VIDEODRIVER'] = 'dummy'
            env['SDL_AUDIODRIVER'] = 'dummy'
        cmd += ['-c', str(cfg)] + list(extra_args)
        self.proc = subprocess.Popen(cmd, cwd=ROOT, env=env,
                                     stdout=subprocess.PIPE,
                                     stderr=subprocess.PIPE, text=True)
        self.stdout_lines: List[str] = []
        self.stderr_lines: List[str] = []
        for stream, sink in ((self.proc.stdout, self.stdout_lines),
                             (self.proc.stderr, self.stderr_lines)):
            threading.Thread(target=self._pump, args=(stream, sink),
                             daemon=True).start()
        self.manifest: Dict[str, str] = {}
        try:
            self.manifest = self._read_manifest()
        except Exception:
            self.stop()
            raise

    @staticmethod
    def _pump(stream, sink: List[str]) -> None:
        for line in stream:
            sink.append(line)

    def _read_manifest(self) -> Dict[str, str]:
        """The YAML block between '--- # koncepcja' and '...', as a flat dict
        keyed by dotted path ('ports.binary')."""
        deadline = time.monotonic() + MANIFEST_TIMEOUT_S
        in_block = False
        section = ''
        fields: Dict[str, str] = {}
        seen = 0
        while time.monotonic() < deadline:
            if seen >= len(self.stdout_lines):
                if self.proc.poll() is not None:
                    raise AssertionError(
                        f'core exited with {self.proc.returncode} before its '
                        f'manifest:\n{"".join(self.stderr_lines[-20:])}')
                time.sleep(0.02)
                continue
            line = self.stdout_lines[seen].rstrip('\n')
            seen += 1
            if not in_block:
                in_block = line.startswith('--- # koncepcja')
                continue
            if line == '...':
                return fields
            m = re.match(r'^(\s*)([A-Za-z0-9_]+):\s*(.*)$', line)
            if not m:
                continue
            indent, key, value = m.groups()
            value = value.split(' #')[0].strip()
            if not indent:
                section = key if value == '' else ''
                if value != '':
                    fields[key] = value.strip("'")
            elif section:
                fields[f'{section}.{key}'] = value.strip("'")
        raise AssertionError('no complete startup manifest within '
                             f'{MANIFEST_TIMEOUT_S:.0f}s')

    @property
    def binary_port(self) -> int:
        text = self.manifest.get('ports.binary', '<missing>')
        assert text.isdigit(), f'manifest ports.binary is {text!r}'
        return int(text)

    @property
    def pid(self) -> int:
        return int(self.manifest['pid'])

    def client(self) -> KcpcClient:
        return KcpcClient(self.binary_port, timeout=IO_TIMEOUT_S)

    def ipc(self, command: str) -> str:
        """One text IPC command on its own connection."""
        port = int(self.manifest['ports.ipc'])
        with socket.create_connection(('127.0.0.1', port),
                                      timeout=IO_TIMEOUT_S) as sock:
            sock.sendall((command + '\n').encode())
            buf = b''
            while b'\n' not in buf:
                chunk = sock.recv(65536)
                if not chunk:
                    break
                buf += chunk
        return buf.decode().strip()

    def ipc_quit(self) -> None:
        port = int(self.manifest['ports.ipc'])
        with socket.create_connection(('127.0.0.1', port),
                                      timeout=IO_TIMEOUT_S) as sock:
            sock.sendall(b'quit\n')
            # The core may exit before it answers; either is fine.
            try:
                sock.recv(64)
            except OSError:
                pass

    def wait_exit(self, timeout: float = EXIT_TIMEOUT_S) -> int:
        return self.proc.wait(timeout=timeout)

    def reads_served(self) -> Dict[str, int]:
        """Where the machine gate took this core's reads, from the line the
        server logs when it stops: {'boundary', 'paused', 'fallback',
        'busy'}. Call after the core has exited."""
        deadline = time.monotonic() + 2.0
        while True:
            for line in self.stderr_lines:
                m = re.search(r'KCPC: reads served: (\d+) at a frame '
                              r'boundary, (\d+) while paused, (\d+) by '
                              r'fallback, (\d+) busy', line)
                if m:
                    return dict(zip(('boundary', 'paused', 'fallback',
                                     'busy'), map(int, m.groups())))
            # The pump thread may still be draining the pipe.
            assert time.monotonic() < deadline, (
                'the core did not log where its reads were served:\n'
                + ''.join(self.stderr_lines[-10:]))
            time.sleep(0.02)

    def stop(self) -> None:
        if self.proc.poll() is None:
            self.proc.kill()
            self.proc.wait()
        shutil.rmtree(self.tmpdir, ignore_errors=True)

    def __enter__(self) -> 'Core':
        return self

    def __exit__(self, *exc) -> None:
        self.stop()


# -- raw-socket helpers for the malformed frames ---------------------------

def read_until_closed(sock: socket.socket) -> Tuple[List[kc.Frame], bool]:
    """Every frame the server sends until it closes the connection.

    Returns (frames, closed). `closed` is False if the timeout came first,
    which is a failure for every caller: the server was meant to hang up.
    """
    sock.settimeout(IO_TIMEOUT_S)
    data = b''
    closed = False
    try:
        while True:
            chunk = sock.recv(65536)
            if not chunk:
                closed = True
                break
            data += chunk
    except socket.timeout:
        pass
    except ConnectionError:
        # A reset instead of a clean close: the frame may have been lost,
        # which is exactly what the server's lingering close prevents.
        closed = False
    frames = []
    while len(data) >= kc.HEADER_SIZE:
        magic, version, kind, opcode, frame_id, length = kc.HEADER.unpack(
            data[:kc.HEADER_SIZE])
        assert magic == kc.MAGIC, f'bad magic from server: {magic!r}'
        body = data[kc.HEADER_SIZE:kc.HEADER_SIZE + length]
        assert len(body) == length, 'server frame cut short'
        frames.append(kc.Frame(version, kind, opcode, frame_id, body))
        data = data[kc.HEADER_SIZE + length:]
    assert data == b'', f'{len(data)} stray bytes after the last frame'
    return frames, closed


def expect_error_and_close(sock: socket.socket, status: int,
                           what: str, leading: int = 0) -> None:
    """After `leading` ordinary frames: exactly one ERROR frame (a response,
    opcode 0, id 0) carrying `status`, then end of stream."""
    frames, closed = read_until_closed(sock)
    assert closed, f'{what}: the server did not close the connection'
    assert len(frames) == leading + 1, (
        f'{what}: expected {leading} frame(s) then one ERROR, got '
        f'{[(f.opcode, f.id) for f in frames]}')
    err = frames[-1]
    assert err.kind == kc.KIND_RESPONSE, f'{what}: kind {err.kind}'
    assert err.opcode == kc.OP_ERROR, f'{what}: opcode {err.opcode:#06x}'
    assert err.id == 0, f'{what}: ERROR id {err.id}'
    assert err.status == status, (
        f'{what}: status {kc.status_name(err.status)}, expected '
        f'{kc.status_name(status)} ({err.message!r})')
    assert err.message, f'{what}: ERROR without a message'


def connect(port: int) -> socket.socket:
    sock = socket.create_connection(('127.0.0.1', port), timeout=IO_TIMEOUT_S)
    sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    return sock


def hello_frame(request_id: int = 1, **kwargs) -> bytes:
    return kc.encode_frame(kc.KIND_REQUEST, kc.OP_HELLO, request_id,
                           kc.hello_payload(**kwargs))


# -- tests -----------------------------------------------------------------

def test_manifest_port_and_hello() -> None:
    """Acceptance 4: the manifest prints ports.binary and a client that uses
    only that value completes HELLO; the reply describes this very core."""
    with Core() as core:
        assert core.manifest.get('manifest_version') == '3', core.manifest
        port = core.binary_port
        assert port not in (int(core.manifest['ports.ipc']),
                            int(core.manifest['ports.telnet']))
        with core.client() as c:
            hello = c.hello('test_kcpc manifest')
            assert hello.pid == core.pid == core.proc.pid, (
                hello.pid, core.pid, core.proc.pid)
            assert hello.server_version == core.manifest['version'], (
                hello.server_version, core.manifest['version'])
            assert hello.client_id != 0
            assert hello.server_minor == 0
            assert hello.role == kc.ROLE_OBSERVER
            assert hello.max_frame == kc.MAX_SERVER_FRAME, hello.max_frame
            # The groups implemented so far: the machine reads, with the
            # ROM spaces.
            assert hello.caps == kc.CAP_MACHINE_READ | kc.CAP_MEM_ROM, (
                f'caps {hello.caps:#x}')

            m = hello.machine
            assert m.model == int(core.manifest['machine.model']), m
            assert m.ram_bytes == (
                int(core.manifest['machine.ram_size_kb']) * 1024), m
            # Not the manifest's effective_tier: that is printed before the
            # first frame has latched the tier (beads-3o1.1 report). The
            # text IPC reads the live one.
            tier = core.ipc('tier')
            assert f'effective={m.tier_name} ' in tier, (tier, m)
            assert m.headless == (core.manifest['mode'] == 'headless'), m
            assert m.has_asic == (m.model == 3), m
            assert 0 <= m.crtc_type <= 3, m
            # The text IPC reads the same chip type.
            crtc = core.ipc('config get crtc_type')
            assert crtc == f'OK {m.crtc_type}', (crtc, m.crtc_type)
            # The example config fits AMSDOS in slot 7. The on-board BASIC
            # is not an expansion ROM: slot 0 sets no bit.
            assert m.rom_slots == 0x80, f'rom_slots {m.rom_slots:#x}'
            c.ping()
        if MODE != 'gui':
            assert core.manifest['mode'] == 'headless', core.manifest['mode']


def test_two_clients_concurrently() -> None:
    """Acceptance 2: two connections complete HELLO and PING at the same
    time and every response carries the id of its own request."""
    with Core() as core:
        port = core.binary_port
        errors: List[str] = []
        ids: List[int] = []
        start = threading.Barrier(2)

        def worker(index: int) -> None:
            try:
                with KcpcClient(port, timeout=IO_TIMEOUT_S) as c:
                    # Disjoint id ranges: a response routed to the wrong
                    # connection cannot match by accident.
                    c.next_id = 0x10000 * (index + 1)
                    start.wait(timeout=IO_TIMEOUT_S)
                    hello = c.hello(f'test_kcpc worker {index}')
                    ids.append(hello.client_id)
                    for _ in range(200):
                        c.ping()  # request() checks opcode and id
                    assert c.machine_info().model == hello.machine.model
            except Exception as e:  # pylint: disable=broad-except
                errors.append(f'worker {index}: {e!r}')

        threads = [threading.Thread(target=worker, args=(i,))
                   for i in range(2)]
        for t in threads:
            t.start()
        for t in threads:
            t.join(timeout=60)
        assert not errors, errors
        assert len(ids) == 2 and ids[0] != ids[1], f'client ids {ids}'
        assert 0 not in ids


def test_pipelined_pings_in_order() -> None:
    """Acceptance 2: requests sent without waiting are answered in the order
    they arrived."""
    with Core() as core, core.client() as c:
        c.hello('test_kcpc pipeline')
        count = 1000
        first = 5000
        burst = b''.join(
            kc.encode_frame(kc.KIND_REQUEST, kc.OP_PING, first + i)
            for i in range(count))
        c.send_raw(burst)
        for i in range(count):
            frame = c.read_frame()
            assert frame.kind == kc.KIND_RESPONSE
            assert frame.opcode == kc.OP_PING
            assert frame.id == first + i, (
                f'reply {i} has id {frame.id}, expected {first + i}')
            assert frame.status == kc.OK
            assert len(frame.payload) == 4, 'PING replies with status alone'


def test_machine_info_stamp() -> None:
    """MACHINE_INFO repeats what HELLO said, and its stamp tells time: the
    frame count grows while the machine runs and stands still while it is
    paused, with run_state saying which."""
    with Core() as core, core.client() as c:
        hello = c.hello('test_kcpc stamp')
        info = c.machine_info()
        for field in ('model', 'crtc_type', 'flags', 'ram_bytes',
                      'rom_slots'):
            assert getattr(info, field) == getattr(hello.machine, field), (
                field, info, hello.machine)
        assert not info.stamp.paused, 'a freshly started core is running'

        # Running: emulated frames pass. 'wait vbl' counts them, so this
        # cannot pass on a frozen core, and it leaves the machine paused.
        before = c.machine_info().stamp
        resp = core.ipc('wait vbl 10 10000')
        assert resp.startswith('OK'), resp
        paused1 = c.machine_info().stamp
        assert paused1.paused, 'run_state must say paused after wait vbl'
        assert paused1.frame >= before.frame + 10, (before, paused1)

        # Paused: two reads describe the same instant, and reading did not
        # resume the machine.
        time.sleep(0.2)
        paused2 = c.machine_info().stamp
        assert paused2.paused
        assert paused2.frame == paused1.frame, (paused1, paused2)

        # Running again: reading did not leave it paused either.
        assert core.ipc('run').startswith('OK')
        deadline = time.monotonic() + IO_TIMEOUT_S
        while True:
            now = c.machine_info().stamp
            if not now.paused and now.frame > paused2.frame:
                break
            assert time.monotonic() < deadline, (
                f'frame count stuck at {now} after run')
            time.sleep(0.05)


class Readers:
    """`count` clients that each read in a loop, as fast as the server
    answers, until stopped. `what` is 'info' (MACHINE_INFO) or 'memory' (all
    64 KiB of the CPU space and the registers, in turn: what a live memory
    view and a register window ask for). On a running machine every read is taken by
    the frame runner at a frame boundary; on a paused one, under a pause
    lease."""

    def __init__(self, core: Core, count: int, what: str = 'info'):
        self.core = core
        self.what = what
        self.stop = threading.Event()
        self.calls = 0
        self.running_reads = 0   # replies whose stamp said "running"
        self.slowest = 0.0       # seconds, the longest single read
        self.errors: List[str] = []
        self.threads = [threading.Thread(target=self._run)
                        for _ in range(count)]

    def _read(self, c: KcpcClient, turn: int) -> kc.Stamp:
        if self.what == 'info':
            return c.machine_info().stamp
        if turn % 2:
            return c.regs_get().stamp
        stamp, data = c.mem_read_stamped(kc.SPACE_CPU, 0, kc.CPU_SPACE_SIZE)
        assert len(data) == kc.CPU_SPACE_SIZE
        return stamp

    def _run(self) -> None:
        try:
            with self.core.client() as c:
                c.hello('test_kcpc reader')
                turn = 0
                while not self.stop.is_set():
                    begin = time.monotonic()
                    stamp = self._read(c, turn)
                    turn += 1
                    self.slowest = max(self.slowest,
                                       time.monotonic() - begin)
                    self.calls += 1
                    if not stamp.paused:
                        self.running_reads += 1
        except Exception as e:  # pylint: disable=broad-except
            self.errors.append(repr(e))

    def __enter__(self) -> 'Readers':
        for t in self.threads:
            t.start()
        return self

    def __exit__(self, *exc) -> None:
        self.stop.set()
        for t in self.threads:
            t.join(timeout=30)


def test_concurrent_reads_keep_the_run_state() -> None:
    """Several clients reading at once leave the machine as they found it:
    running if it ran, paused if it was paused. (Overlapping pause leases
    used to lose the resume: four readers stopped the machine for good.)"""
    with Core() as core, core.client() as c:
        c.hello('test_kcpc run state')
        before = c.machine_info().stamp
        with Readers(core, 4) as readers:
            time.sleep(1.5)
        assert not readers.errors, readers.errors
        assert readers.calls > 100, f'only {readers.calls} reads happened'
        after = c.machine_info().stamp
        assert not after.paused, 'the readers left the machine paused'
        # It also kept running underneath them, at something like 50 Hz.
        assert after.frame - before.frame >= 25, (before, after, readers.calls)
        time.sleep(0.3)
        later = c.machine_info().stamp
        assert later.frame > after.frame, 'the machine is not running'

        # And the other way round: a paused machine stays paused.
        assert core.ipc('wait vbl 2 10000').startswith('OK')
        paused = c.machine_info().stamp
        assert paused.paused
        with Readers(core, 4) as readers:
            time.sleep(0.5)
        assert not readers.errors, readers.errors
        still = c.machine_info().stamp
        assert still.paused and still.frame == paused.frame, (paused, still)
        print(f'    {readers.calls} reads while paused, frame unchanged')


def _stepped_state(core: Core, frames: int, readers: int,
                   what: str = 'info') -> Tuple[int, bool, str, str, Readers]:
    """Reset to a known paused state, run `frames` frames with the text
    IPC's `step frame` while `readers` clients read, and describe the
    machine afterwards: (frames run, paused, registers, memory, readers).

    The registers are every one of them, R included, and the memory is all
    physical RAM (the video RAM with it), so two runs that end in the same
    description computed the same thing."""
    with core.client() as c:
        c.hello('test_kcpc step')
        assert core.ipc('pause').startswith('OK')
        resp = core.ipc('reset --no-resume')
        assert resp.startswith('OK'), resp
        deadline = time.monotonic() + IO_TIMEOUT_S
        while not c.machine_info().stamp.paused:
            assert time.monotonic() < deadline, 'reset did not stay paused'
            time.sleep(0.02)
        before = c.machine_info().stamp
        with Readers(core, readers, what) as r:
            resp = core.ipc(f'step frame {frames}')
        assert resp.startswith('OK'), resp
        assert not r.errors, r.errors
        after = c.machine_info().stamp
        # Through the text IPC, not through the port under test.
        regs = core.ipc('regs') + ''.join(
            ' ' + core.ipc(f'reg get {name}')
            for name in ('I', 'R', 'IFF1', 'IFF2', 'AFX', 'BCX', 'DEX',
                         'HLX'))
        pages = c.mem_info().ram_bytes // 16384
        ram = ''.join(core.ipc(f'mem read 0 16384 --bank={page}')
                      for page in range(pages))
        return after.frame - before.frame, after.paused, regs, ram, r


def test_reads_do_not_change_what_the_machine_computes() -> None:
    """Epic invariant 2 and acceptance 5 of slice 2: a run with clients
    attached makes the same observations as a run without. From the same
    reset, 100 frames end in the same registers and the same RAM whether
    nobody reads, two clients read MACHINE_INFO all the way through, or a
    client reads all 64 KiB and the registers at every frame boundary; and
    the step itself runs exactly 100 frames and ends paused. (A read used to
    abandon the text IPC's frame step after a few frames and leave the
    machine running.)"""
    frames = 100
    with Core() as core:
        alone = _stepped_state(core, frames, 0)
        again = _stepped_state(core, frames, 0)
        assert alone[:4] == again[:4], (
            'the reference run is not reproducible; the comparison below '
            'would prove nothing')
        assert alone[0] == frames and alone[1], alone[:2]
        for what, readers in (('info', 2), ('memory', 1), ('memory', 3)):
            watched = _stepped_state(core, frames, readers, what)
            r = watched[4]
            label = f'{readers} x {what}'
            # A read that waited out the gate's fallback takes a quarter of
            # a second, so more than 50 in these two seconds were taken at
            # frame boundaries, by the runner.
            assert r.calls > 50, (
                f'{label}: only {r.calls} reads during the step')
            assert r.running_reads > 50, (
                f'{label}: only {r.running_reads} reads saw the machine '
                'running; the step was not read at its frame boundaries')
            assert watched[0] == frames, (
                f'{label}: the step ran {watched[0]} frames, not {frames}')
            assert watched[1], f'{label}: the step did not end paused'
            assert watched[2] == alone[2], (
                f'{label}: registers differ:\n  alone   {alone[2]}\n'
                f'  watched {watched[2]}')
            assert watched[3] == alone[3], (
                f'{label}: RAM differs after the same {frames} frames')
            print(f'    {label}: {r.calls} reads during {frames} frames '
                  f'({r.running_reads} at a frame boundary, slowest '
                  f'{r.slowest * 1000:.0f} ms); registers and RAM identical')


# -- beads-mk1w: a second actor that takes a lease by its own means ---------

def _free_port() -> int:
    """A loopback port nobody holds right now, for a server this test
    starts itself. (Never a probe of somebody else's.)"""
    with socket.socket() as sock:
        sock.bind(('127.0.0.1', 0))
        return sock.getsockname()[1]


class Resetter:
    """Asks the M4 board's HTTP server for a CPC reset, over and over. The
    server only raises a flag; the core's main loop then calls
    emulator_reset(), which takes a pause lease of its own. It is the one
    lease holder a test can aim at a text IPC command that is still running:
    the text IPC serves one connection at a time."""

    def __init__(self, port: int):
        self.port = port
        self.stop = threading.Event()
        self.posts = 0
        self.errors: List[str] = []
        self.thread = threading.Thread(target=self._run)

    def _run(self) -> None:
        try:
            while not self.stop.is_set():
                conn = http.client.HTTPConnection('127.0.0.1', self.port,
                                                  timeout=IO_TIMEOUT_S)
                try:
                    conn.request('POST', '/reset')
                    conn.getresponse().read()
                finally:
                    conn.close()
                self.posts += 1
        except Exception as exc:  # noqa: BLE001 - reported by the test
            self.errors.append(f'{type(exc).__name__}: {exc}')

    def __enter__(self) -> 'Resetter':
        self.thread.start()
        return self

    def __exit__(self, *exc) -> None:
        self.stop.set()
        self.thread.join()


def test_a_reset_by_another_actor_does_not_end_a_frame_step() -> None:
    """beads-mk1w: a frame step ends in a real stop, and a reset that arrives
    from somewhere else while it runs must neither cut it short nor run the
    machine on after it. emulator_reset() used to let go of its lease and
    resume by hand: when the frame its lease waited out was the step's last,
    it resumed over the step's stop (the step answered OK and the machine
    ran on), and between its release and its resume the machine looked
    stopped to the waiting step, which then gave up early."""
    frames = 150
    http_port = _free_port()
    with Core('-O', f'peripheral.m4_http_port={http_port}') as core, \
            core.client() as c:
        c.hello('test_kcpc reset during step')
        assert core.ipc('m4 http start').startswith('OK')
        deadline = time.monotonic() + IO_TIMEOUT_S
        while f'running=1 port={http_port} ' not in core.ipc('m4 http status'):
            assert time.monotonic() < deadline, core.ipc('m4 http status')
            time.sleep(0.02)
        posts = 0
        for attempt in range(3):
            assert core.ipc('pause').startswith('OK')
            deadline = time.monotonic() + IO_TIMEOUT_S
            while not c.machine_info().stamp.paused:
                assert time.monotonic() < deadline, 'the machine did not pause'
                time.sleep(0.02)
            before = c.machine_info().stamp
            with Resetter(http_port) as resetter:
                resp = core.ipc(f'step frame {frames}')
            assert resp.startswith('OK'), resp
            assert not resetter.errors, resetter.errors
            after = c.machine_info().stamp
            assert after.frame - before.frame == frames, (
                f'attempt {attempt}: the step ran '
                f'{after.frame - before.frame} frames, not {frames}')
            assert after.paused, f'attempt {attempt}: not paused after the step'
            # Any reset still on its way lands now. The stop must outlast it.
            time.sleep(0.3)
            later = c.machine_info().stamp
            assert later.paused and later.frame == after.frame, (
                f'attempt {attempt}: the machine ran on after the step: '
                f'{after} then {later}')
            posts += resetter.posts
        assert posts > 3 * frames // 4, (
            f'only {posts} reset requests in {3 * frames} frames: the step was '
            'hardly disturbed and this proved little')
        print(f'    {posts} reset requests during 3 x {frames} stepped frames; every '
              'step ran its frames and stayed stopped')


# -- slice 2: the machine reads ---------------------------------------------

def ipc_bytes(core: Core, command: str) -> bytes:
    """The bytes of a text IPC `mem read`: 'OK ' and two hex digits each."""
    resp = core.ipc(command)
    assert resp.startswith('OK '), f'{command}: {resp[:80]}'
    return bytes.fromhex(resp[3:])


def pause_core(core: Core, c: KcpcClient) -> kc.Stamp:
    """Pause through the text IPC and wait until the port agrees."""
    assert core.ipc('pause').startswith('OK')
    deadline = time.monotonic() + IO_TIMEOUT_S
    while True:
        stamp = c.machine_info().stamp
        if stamp.paused:
            return stamp
        assert time.monotonic() < deadline, 'the machine did not pause'
        time.sleep(0.02)


def expect_status(c: KcpcClient, status: int, what: str, space: int,
                  addr: int, length: int, bank: int = 0) -> None:
    try:
        c.mem_read(space, addr, length, bank)
    except KcpcError as e:
        assert e.status == status, (
            f'{what}: {kc.status_name(e.status)}, expected '
            f'{kc.status_name(status)} ({e.message!r})')
        assert e.message, f'{what}: no message'
        return
    raise AssertionError(f'{what}: the read was accepted')


def test_mem_read_matches_the_text_ipc() -> None:
    """Acceptance 1: MEM_READ of 65536 bytes of the CPU space is one response
    frame whose bytes equal the text IPC's `mem read 0 65536`; the same for
    CPU_RAM against --view=ram and PHYS_RAM against every --bank=N page."""
    with Core() as core, core.client() as c:
        c.hello('test_kcpc mem')
        # Let the firmware draw something first, then stop the clock so the
        # two ports read the same instant.
        assert core.ipc('wait vbl 60 20000').startswith('OK')
        paused = pause_core(core, c)

        request_id = c.send_request(
            kc.OP_MEM_READ,
            kc.mem_read_payload(kc.SPACE_CPU, 0, kc.CPU_SPACE_SIZE))
        frame = c.read_frame()   # the very next frame, and the only one
        assert frame.kind == kc.KIND_RESPONSE and frame.id == request_id
        assert frame.opcode == kc.OP_MEM_READ and frame.status == kc.OK
        assert len(frame.payload) == (
            kc.MEM_READ_REPLY_OVERHEAD + kc.CPU_SPACE_SIZE), len(frame.payload)
        cpu = frame.payload[kc.MEM_READ_REPLY_OVERHEAD:]
        assert cpu == ipc_bytes(core, 'mem read 0 65536'), (
            'CPU space differs from the text IPC')

        stamp, ram = c.mem_read_stamped(kc.SPACE_CPU_RAM, 0,
                                        kc.CPU_SPACE_SIZE)
        assert stamp.paused and stamp.frame == paused.frame, (stamp, paused)
        assert ram == ipc_bytes(core, 'mem read 0 65536 --view=ram'), (
            'CPU_RAM space differs from --view=ram')
        assert cpu != ram, (
            'a ROM is paged in at the BASIC prompt: the two views of the '
            'same addresses must differ, or the test compared nothing')

        info = c.mem_info()
        assert info.ram_bytes == c.machine_info().ram_bytes == (
            int(core.manifest['machine.ram_size_kb']) * 1024), info
        assert info.stamp.paused and info.stamp.frame == paused.frame
        assert info.rom_slots == 0x80, f'rom_slots {info.rom_slots:#x}'
        phys = c.mem_read(kc.SPACE_PHYS_RAM, 0, info.ram_bytes)
        assert len(phys) == info.ram_bytes
        pages = info.ram_bytes // 16384
        assert pages >= 8, 'the example config is a 128K machine'
        for page in range(pages):
            assert phys[page * 16384:(page + 1) * 16384] == ipc_bytes(
                core, f'mem read 0 16384 --bank={page}'), (
                    f'PHYS_RAM page {page} differs from --bank={page}')
        assert any(phys[:0x10000]), 'the base RAM is all zeros'

        # Parts of a space, across the 16K slot borders.
        for space, view in ((kc.SPACE_CPU, ''), (kc.SPACE_CPU_RAM,
                                                 ' --view=ram')):
            for addr, length in ((0x3FF0, 0x20), (0xBFFF, 2), (0xFFFF, 1),
                                 (0x1234, 0x9000)):
                assert c.mem_read(space, addr, length) == ipc_bytes(
                    core, f'mem read {addr} {length}{view}'), (
                        space, hex(addr), length)
        assert c.mem_read(kc.SPACE_PHYS_RAM, 0xFFF0, 0x20) == (
            phys[0xFFF0:0x10010]), 'across the base/expansion border'
        assert c.mem_read(kc.SPACE_CPU, 0x10000, 0) == b''


def test_rom_spaces_and_limits() -> None:
    """Acceptance 2: ROM_LOWER and ROM_UPPER return the ROM bytes while the
    ROM is paged out; addr+len past the end of a space is OUT_OF_RANGE; a
    read over the frame limit is TOO_LARGE."""
    with Core() as core, core.client() as c:
        c.hello('test_kcpc rom')
        assert core.ipc('pause').startswith('OK')
        assert core.ipc('reset --no-resume').startswith('OK')
        pause_core(core, c)

        # Out of reset both ROMs are paged in and slot 0 is selected, so the
        # text IPC's CPU view shows them: that is the reference.
        info = c.mem_info()
        assert info.lower_rom_on and info.upper_rom_on, info
        assert info.upper_rom == 0, info
        lower = ipc_bytes(core, 'mem read 0 16384')
        basic = ipc_bytes(core, f'mem read {0xC000} 16384')
        assert c.mem_read(kc.SPACE_ROM_LOWER, 0, 0x4000) == lower
        assert c.mem_read(kc.SPACE_ROM_UPPER, 0, 0x4000, bank=0) == basic
        amsdos = c.mem_read(kc.SPACE_ROM_UPPER, 0, 0x4000, bank=7)
        assert amsdos != basic, 'slot 7 holds AMSDOS, not BASIC'
        # A slot with nothing fitted answers with the on-board BASIC, as it
        # does on the bus.
        assert c.mem_read(kc.SPACE_ROM_UPPER, 0, 0x4000, bank=3) == basic

        # Page both ROMs out with three instructions in RAM:
        #   DI : LD BC,&7F8C : OUT (C),C
        assert core.ipc('mem write 0x8000 F3018C7FED4918FE').startswith('OK')
        assert core.ipc('reg set PC 0x8000').startswith('OK')
        assert core.ipc('step 3').startswith('OK')
        info = c.mem_info()
        assert not info.lower_rom_on and not info.upper_rom_on, (
            f'rom_config {info.rom_config:#x}: the ROMs are still paged in')
        cpu = c.mem_read(kc.SPACE_CPU, 0, kc.CPU_SPACE_SIZE)
        assert cpu == c.mem_read(kc.SPACE_CPU_RAM, 0, kc.CPU_SPACE_SIZE), (
            'with both ROMs out the CPU sees RAM everywhere')
        assert cpu[:0x4000] != lower and cpu[0xC000:] != basic
        assert cpu == ipc_bytes(core, 'mem read 0 65536')
        # ...and the ROM spaces are what they were.
        assert c.mem_read(kc.SPACE_ROM_LOWER, 0, 0x4000) == lower
        assert c.mem_read(kc.SPACE_ROM_UPPER, 0, 0x4000, bank=0) == basic
        assert c.mem_read(kc.SPACE_ROM_UPPER, 0, 0x4000, bank=7) == amsdos
        assert c.mem_read(kc.SPACE_ROM_LOWER, 0x3FF0, 0x10) == lower[0x3FF0:]

        # Select AMSDOS and page the upper ROM back in: the CPU space shows
        # slot 7, and MEM_INFO says which slot.
        #   LD BC,&DF07 : OUT (C),C : LD BC,&7F84 : OUT (C),C
        assert core.ipc(
            'mem write 0x8000 0107DFED4901847FED4918FE').startswith('OK')
        assert core.ipc('reg set PC 0x8000').startswith('OK')
        assert core.ipc('step 4').startswith('OK')
        info = c.mem_info()
        assert info.upper_rom == 7 and info.upper_rom_on, info
        assert not info.lower_rom_on, info
        assert c.mem_read(kc.SPACE_CPU, 0xC000, 0x4000) == amsdos

        # Past the end of a space: nothing wraps.
        ram_bytes = info.ram_bytes
        for what, space, addr, length, bank in (
                ('CPU, one byte over', kc.SPACE_CPU, 0xFFFF, 2, 0),
                ('CPU, 64K + 1', kc.SPACE_CPU, 0, 0x10001, 0),
                ('CPU, empty read past the end', kc.SPACE_CPU, 0x10001, 0, 0),
                ('CPU_RAM, addr wraps 32 bits', kc.SPACE_CPU_RAM,
                 0xFFFFFFFF, 2, 0),
                ('ROM_LOWER, one byte over', kc.SPACE_ROM_LOWER, 0x3FFF, 2,
                 0),
                ('ROM_UPPER, 16K + 1', kc.SPACE_ROM_UPPER, 0, 0x4001, 7),
                ('ROM_UPPER, slot 32', kc.SPACE_ROM_UPPER, 0, 1, 32),
                ('PHYS_RAM, one byte over', kc.SPACE_PHYS_RAM,
                 ram_bytes - 1, 2, 0),
                ('PHYS_RAM, starts past the end', kc.SPACE_PHYS_RAM,
                 ram_bytes + 1, 0, 0)):
            expect_status(c, kc.OUT_OF_RANGE, what, space, addr, length, bank)
        assert c.mem_read(kc.SPACE_PHYS_RAM, ram_bytes - 1, 1) != b''
        assert c.mem_read(kc.SPACE_PHYS_RAM, ram_bytes, 0) == b''

        # A space the server does not have, and a bank where none applies.
        for what, space, bank in (
                ('ASIC_PAGE before CAP_CHIPS', kc.SPACE_ASIC_PAGE, 0),
                ('space 99', 99, 0),
                ('a bank on the CPU space', kc.SPACE_CPU, 1),
                ('a bank on PHYS_RAM', kc.SPACE_PHYS_RAM, 2)):
            expect_status(c, kc.BAD_PAYLOAD, what, space, 0, 16, bank)
        try:
            c.request(kc.OP_MEM_READ, b'\x00' * 11)
            raise AssertionError('a short MEM_READ request was accepted')
        except KcpcError as e:
            assert e.status == kc.BAD_PAYLOAD, e

        # Over the frame limit, which is 16 MiB on this connection...
        fits = kc.MAX_SERVER_FRAME - kc.MEM_READ_REPLY_OVERHEAD
        for length in (fits + 1, 0xFFFFFFFF):
            expect_status(c, kc.TOO_LARGE, f'{length} bytes',
                          kc.SPACE_PHYS_RAM, 0, length)
        c.ping()

    # ...and whatever a client asked for in HELLO on its own. With the
    # smallest allowed limit, all of a 128K machine's RAM does not fit one
    # reply although every byte of the range exists.
    with Core() as core, core.client() as small:
        hello = small.hello('test_kcpc small frames',
                            max_frame=kc.MIN_CLIENT_MAX_FRAME)
        assert hello.max_frame == kc.MIN_CLIENT_MAX_FRAME
        ram_bytes = small.mem_info().ram_bytes
        assert ram_bytes >= kc.MIN_CLIENT_MAX_FRAME
        fits = kc.MIN_CLIENT_MAX_FRAME - kc.MEM_READ_REPLY_OVERHEAD
        expect_status(small, kc.TOO_LARGE, 'all RAM in a 128 KiB frame',
                      kc.SPACE_PHYS_RAM, 0, ram_bytes)
        expect_status(small, kc.TOO_LARGE, 'one byte over the frame',
                      kc.SPACE_PHYS_RAM, 0, fits + 1)
        assert len(small.mem_read(kc.SPACE_PHYS_RAM, 0, fits)) == fits
        # The promise of section 11: 64 KiB always fits.
        assert len(small.mem_read(kc.SPACE_CPU, 0, 0x10000)) == 0x10000


def test_regs_get_matches_the_text_ipc() -> None:
    """Acceptance 3: REGS_GET equals the text IPC's registers at the same
    paused instant, and two reads while paused carry the same stamp."""
    names = {'af': 'AF', 'bc': 'BC', 'de': 'DE', 'hl': 'HL', 'ix': 'IX',
             'iy': 'IY', 'sp': 'SP', 'pc': 'PC', 'af_alt': 'AFX',
             'bc_alt': 'BCX', 'de_alt': 'DEX', 'hl_alt': 'HLX', 'i': 'I',
             'r': 'R', 'iff1': 'IFF1', 'iff2': 'IFF2', 'im': 'IM',
             'halt': 'HALT'}

    def text_regs(core: Core) -> dict:
        out = {}
        for field, name in names.items():
            resp = core.ipc(f'reg get {name}')
            assert resp.startswith('OK '), (name, resp)
            out[field] = int(resp[3:], 16)
        return out

    with Core() as core, core.client() as c:
        c.hello('test_kcpc regs')
        assert core.ipc('wait vbl 60 20000').startswith('OK')
        paused = pause_core(core, c)

        first = c.regs_get()
        assert first.values() == text_regs(core), (
            first.values(), text_regs(core))
        assert first.pc != 0 and first.sp != 0, first
        # The one-line dump agrees too.
        dump = core.ipc('regs')
        for token, value in (('PC', first.pc), ('SP', first.sp),
                             ('IX', first.ix), ('IY', first.iy)):
            assert f'{token}={value:04X}' in dump, (token, dump)
        assert f'A={first.af >> 8:02X} F={first.af & 0xFF:02X}' in dump, dump

        time.sleep(0.2)
        second = c.regs_get()
        assert first.stamp.paused and second.stamp.paused
        assert first.stamp.frame == second.stamp.frame == paused.frame, (
            first.stamp, second.stamp, paused)
        assert second.values() == first.values(), 'a paused machine moved'

        # The block follows the machine, not a copy taken at the pause:
        # every register set through the text IPC reads back.
        wanted = {'af': 0x1234, 'bc': 0x2345, 'de': 0x3456, 'hl': 0x4567,
                  'ix': 0x5678, 'iy': 0x6789, 'sp': 0x789A, 'af_alt': 0x89AB,
                  'bc_alt': 0x9ABC, 'de_alt': 0xABCD, 'hl_alt': 0xBCDE,
                  'i': 0x5A}
        for field, value in wanted.items():
            resp = core.ipc(f'reg set {names[field]} {value:#x}')
            assert resp.startswith('OK'), (field, resp)
        after = c.regs_get()
        for field, value in wanted.items():
            assert getattr(after, field) == value, (
                f'{field}: {getattr(after, field):#x}, set to {value:#x}')
        assert after.values() == text_regs(core)
        assert after.stamp.frame == paused.frame and after.stamp.paused

        # Running: the stamp says so and the frame count moves.
        assert core.ipc('reset').startswith('OK')
        deadline = time.monotonic() + IO_TIMEOUT_S
        while True:
            live = c.regs_get()
            if not live.stamp.paused and live.stamp.frame > paused.frame + 5:
                break
            assert time.monotonic() < deadline, f'not running: {live.stamp}'
            time.sleep(0.02)
        assert 0 <= live.im <= 2 and live.iff1 in (0, 1), live


def test_reads_work_running_and_across_a_pause() -> None:
    """Acceptance 4: reads work on a running and on a paused machine, and a
    read that is waiting for a frame boundary when the machine is paused
    under it is still answered, once. Nothing a reader does changes the run
    state."""
    with Core() as core, core.client() as c:
        c.hello('test_kcpc gate')
        # Running: every read is taken at a frame boundary.
        stamps = []
        begin = time.monotonic()
        for _ in range(100):
            stamp, data = c.mem_read_stamped(kc.SPACE_CPU, 0,
                                             kc.CPU_SPACE_SIZE)
            assert len(data) == kc.CPU_SPACE_SIZE
            stamps.append(stamp)
        took = time.monotonic() - begin
        assert not any(s.paused for s in stamps), 'a read paused the machine'
        frames = [s.frame for s in stamps]
        assert frames == sorted(frames) and frames[-1] > frames[0], frames
        # One frame boundary each: 100 reads span about 100 frames (2 s at
        # 50 Hz) on a host that keeps up, where 100 reads through the
        # fallback would take 25 s, more than 1200 frames. The bound leaves
        # room for a slow client on a loaded CI machine.
        assert frames[-1] - frames[0] <= 600, (frames[0], frames[-1])
        print(f'    100 x 64 KiB while running: {took * 1000:.0f} ms, '
              f'{frames[-1] - frames[0]} frames')

        # Readers hammering while the machine is paused and resumed under
        # them: each request gets its one answer (the client matches ids),
        # whichever side of a pause it landed on.
        with Readers(core, 3, 'memory') as readers:
            for _ in range(25):
                assert core.ipc('pause').startswith('OK')
                time.sleep(0.03)
                assert core.ipc('run').startswith('OK')
                time.sleep(0.05)
            calls_running = readers.calls
            assert core.ipc('pause').startswith('OK')
            time.sleep(0.5)
            calls_paused = readers.calls - calls_running
        assert not readers.errors, readers.errors
        assert calls_running > 100, f'only {calls_running} reads'
        assert calls_paused > 100, (
            f'only {calls_paused} reads in half a second of pause')
        assert readers.running_reads > 50, readers.running_reads
        print(f'    {calls_running} reads across 25 pause/run cycles, '
              f'{calls_paused} more while paused; slowest '
              f'{readers.slowest * 1000:.0f} ms')

        # Still paused, and nothing moved while the readers read.
        one = c.regs_get()
        time.sleep(0.2)
        two = c.regs_get()
        assert one.stamp.paused and two.stamp == one.stamp, (one.stamp,
                                                             two.stamp)
        assert two.values() == one.values()
        # And it runs again when told to.
        assert core.ipc('run').startswith('OK')
        deadline = time.monotonic() + IO_TIMEOUT_S
        while True:
            now = c.mem_info().stamp
            if not now.paused and now.frame > one.stamp.frame:
                break
            assert time.monotonic() < deadline, f'stuck at {now}'
            time.sleep(0.05)


def test_quit_while_clients_read() -> None:
    """Shutdown with reads in flight: the core still quits promptly, and no
    reader is left waiting for a frame boundary that will not come."""
    with Core() as core:
        readers = Readers(core, 4, 'memory')
        with readers:
            time.sleep(0.5)
            begin = time.monotonic()
            core.ipc_quit()
            try:
                code = core.wait_exit()
            except subprocess.TimeoutExpired:
                raise AssertionError(
                    f'quit did not end the core within {EXIT_TIMEOUT_S:.0f}s '
                    'with clients reading') from None
            took = time.monotonic() - begin
        assert code == 0, f'quit exited with {code}'
        assert readers.calls > 20, readers.calls
        # Every reader was cut off by the shutdown, not by a timeout of its
        # own socket.
        assert len(readers.errors) == 4, readers.errors
        assert not any('timed out' in e for e in readers.errors), (
            readers.errors)
        # The core says where the gate took its reads. On a running machine
        # that is the frame boundary: the runner copied, nothing was paused.
        served = core.reads_served()
        assert served['busy'] == 0, served
        assert served['boundary'] > 20, served
        assert served['boundary'] + served['paused'] + served[
            'fallback'] >= readers.calls, (served, readers.calls)
        print(f'    quit -> exit 0 in {took * 1000:.0f} ms with 4 clients '
              f'reading ({readers.calls} reads; served {served})')


def test_bad_frames_close_the_connection() -> None:
    """Acceptance 3: bad magic, a wrong major, an unknown kind, an oversized
    length, request id 0, a first frame that is not HELLO and a second HELLO
    each get one ERROR frame with the documented status, then the socket is
    closed."""
    with Core() as core:
        port = core.binary_port
        hello = hello_frame()
        cases: List[Tuple[str, bytes, int, int]] = [
            # (what, bytes to send, expected status, replies before ERROR)
            ('bad magic (a text IPC client on the wrong port)',
             b'ping\nping\nping\n\n', kc.BAD_FRAME, 0),
            ('bad magic in a full frame',
             kc.encode_frame(kc.KIND_REQUEST, kc.OP_HELLO, 1,
                             kc.hello_payload(), magic=b'KCPX'),
             kc.BAD_FRAME, 0),
            ('wrong major version',
             kc.encode_frame(kc.KIND_REQUEST, kc.OP_HELLO, 1,
                             kc.hello_payload(), version=1),
             kc.UNSUPPORTED_VERSION, 0),
            ('unknown kind',
             kc.encode_frame(9, kc.OP_HELLO, 1, kc.hello_payload()),
             kc.BAD_FRAME, 0),
            ('a response sent by a client',
             kc.encode_frame(kc.KIND_RESPONSE, kc.OP_HELLO, 1,
                             kc.hello_payload()),
             kc.BAD_FRAME, 0),
            ('length over 1 MiB',
             kc.encode_frame(kc.KIND_REQUEST, kc.OP_HELLO, 1, b'',
                             length=(1 << 20) + 1),
             kc.BAD_FRAME, 0),
            ('length 4 GiB',
             kc.encode_frame(kc.KIND_REQUEST, kc.OP_HELLO, 1, b'',
                             length=0xFFFFFFFF),
             kc.BAD_FRAME, 0),
            ('request id 0', hello_frame(0), kc.BAD_FRAME, 0),
            ('first frame is PING, not HELLO',
             kc.encode_frame(kc.KIND_REQUEST, kc.OP_PING, 1),
             kc.HELLO_REQUIRED, 0),
            ('a second HELLO', hello + hello_frame(2),
             kc.HELLO_REQUIRED, 1),
            ('bad frame after a good session',
             hello + kc.encode_frame(kc.KIND_REQUEST, kc.OP_PING, 2)
             + b'garbage-garbage-', kc.BAD_FRAME, 2),
            # The ERROR must not be destroyed by a reset caused by input the
            # server never read.
            ('not HELLO, with 4000 requests pipelined behind it',
             b''.join(kc.encode_frame(kc.KIND_REQUEST, kc.OP_PING, 1 + i)
                      for i in range(4000)),
             kc.HELLO_REQUIRED, 0),
        ]
        for what, payload, status, leading in cases:
            with connect(port) as sock:
                sock.sendall(payload)
                expect_error_and_close(sock, status, what, leading)

        # The server is none the worse for it.
        with core.client() as c:
            c.hello('test_kcpc after bad frames')
            c.ping()


def test_recoverable_errors_keep_the_connection() -> None:
    """UNKNOWN_OPCODE and BAD_PAYLOAD answer the request and leave the
    connection open; a refused HELLO may be sent again."""
    with Core() as core, core.client() as c:
        # HELLO cut short, then with a role that does not exist, then with a
        # max_frame too small for a 64 KiB memory read.
        for what, payload in (
                ('short', b'\x00\x00\x00'),
                ('bad role', kc.hello_payload(role=9)),
                ('tiny max_frame', kc.hello_payload(max_frame=1024))):
            request_id = c.send_request(kc.OP_HELLO, payload)
            frame = c.read_response(request_id)
            assert frame.opcode == kc.OP_HELLO
            assert frame.status == kc.BAD_PAYLOAD, (
                what, kc.status_name(frame.status))
            assert frame.message, what

        hello = c.hello('test_kcpc recoverable', role=kc.ROLE_CONTROLLER,
                        max_frame=1 << 20)
        # Roles arrive with CONTROL; until then everyone observes, and the
        # reply says so rather than pretending.
        assert hello.role == kc.ROLE_OBSERVER
        assert hello.max_frame == 1 << 20, hello.max_frame

        # An opcode of a capability this server does not have yet, and one
        # that exists nowhere.
        for opcode in (kc.OP_REGS_SET, 0x7FFF, kc.OP_ERROR):
            try:
                c.request(opcode, b'\x00' * 12)
                raise AssertionError(f'opcode {opcode:#06x} was accepted')
            except KcpcError as e:
                assert e.status == kc.UNKNOWN_OPCODE, (hex(opcode), e)
        c.ping()

        # A payload on PING, and bytes after MACHINE_INFO's (empty) request,
        # are ignored: a later minor version may add fields.
        c.request(kc.OP_PING, b'from-the-future')
        c.request(kc.OP_MACHINE_INFO, b'\x01\x02')
        # A request of exactly the 1 MiB limit is accepted.
        c.request(kc.OP_PING, b'\x00' * kc.MAX_REQUEST_PAYLOAD)
        c.ping()


def test_ninth_client_is_refused() -> None:
    """Acceptance 3: with 8 clients connected a ninth gets one ERROR frame
    with TOO_MANY_CLIENTS and is closed, the eight are unaffected, and the
    place frees up when one of them leaves."""
    with Core() as core:
        port = core.binary_port
        clients = [KcpcClient(port, timeout=IO_TIMEOUT_S)
                   for _ in range(kc.MAX_CLIENTS)]
        try:
            ids = {c.hello(f'test_kcpc client {i}').client_id
                   for i, c in enumerate(clients)}
            assert len(ids) == kc.MAX_CLIENTS, f'client ids repeat: {ids}'

            # Refused on connect: it does not have to say HELLO first.
            with connect(port) as ninth:
                expect_error_and_close(ninth, kc.TOO_MANY_CLIENTS,
                                       'ninth client')
            # And the same when it does (the refusal must survive the unread
            # HELLO).
            with connect(port) as ninth:
                ninth.sendall(hello_frame())
                expect_error_and_close(ninth, kc.TOO_MANY_CLIENTS,
                                       'ninth client that sent HELLO')

            for c in clients:
                c.ping()

            clients.pop().close()
            deadline = time.monotonic() + IO_TIMEOUT_S
            while True:
                replacement = KcpcClient(port, timeout=IO_TIMEOUT_S)
                try:
                    replacement.hello('test_kcpc replacement')
                    clients.append(replacement)
                    break
                except KcpcClosed as e:
                    # The server has not noticed the close yet.
                    replacement.close()
                    assert e.error and e.error.status == kc.TOO_MANY_CLIENTS
                    assert time.monotonic() < deadline, (
                        'the place of a client that left never freed up')
                    time.sleep(0.05)
            for c in clients:
                c.ping()
        finally:
            for c in clients:
                c.close()


def test_second_instance_gets_its_own_port() -> None:
    """Acceptance 4: a second core cannot have the first one's port; it
    probes forward and each manifest names the port of its own core."""
    with Core() as first, Core() as second:
        p1, p2 = first.binary_port, second.binary_port
        assert p1 != p2, f'both cores report binary port {p1}'
        for port in (p1, p2):
            assert kc.DEFAULT_PORT <= port < (
                kc.DEFAULT_PORT + kc.PORT_PROBE_COUNT), port
        with first.client() as a, second.client() as b:
            assert a.hello('test_kcpc first').pid == first.proc.pid
            assert b.hello('test_kcpc second').pid == second.proc.pid
        # All six ports of the two cores are different.
        ports = [core.manifest[f'ports.{name}'] for core in (first, second)
                 for name in ('ipc', 'telnet', 'binary')]
        assert len(set(ports)) == 6, ports


def listening_tcp_ports(pid: int) -> Optional[List[int]]:
    """The TCP ports `pid` listens on, or None where that cannot be asked
    (no lsof). Asked of the process itself: connecting to ports to see who
    answers would poke the text IPC and telnet console of any other emulator
    running on this machine, and the telnet console types what it receives
    into its CPC."""
    lsof = shutil.which('lsof')
    if lsof is None:
        return None
    out = subprocess.run(
        [lsof, '-a', '-p', str(pid), '-iTCP', '-sTCP:LISTEN', '-P', '-n',
         '-Fn'],
        capture_output=True, text=True, check=False).stdout
    return sorted({int(line.rsplit(':', 1)[1]) for line in out.splitlines()
                   if line.startswith('n') and ':' in line})


def test_port_can_be_disabled() -> None:
    """[ipc] binary_port_enabled=0: the manifest says `binary: null`, no
    KCPC server is started, and the text IPC is unaffected. The default is
    on (the user's decision of 2026-10-11)."""
    with Core('-O', 'ipc.binary_port_enabled=0') as off:
        assert off.manifest.get('ports.binary') == 'null', off.manifest
        assert off.ipc('ping') == 'OK pong'
        assert not any('KCPC: listening' in line
                       for line in off.stderr_lines), 'the server started'
        expected = sorted(int(off.manifest[f'ports.{name}'])
                          for name in ('ipc', 'telnet'))
        listening = listening_tcp_ports(off.proc.pid)
        if listening is None:
            print('    (no lsof: manifest and log only)')
        else:
            assert listening == expected, (
                f'listening on {listening}, expected only {expected}')
    with Core() as on:  # the default
        port = on.binary_port
        assert any(f'KCPC: listening on port {port}' in line
                   for line in on.stderr_lines)
        listening = listening_tcp_ports(on.proc.pid)
        if listening is not None:
            assert port in listening, (port, listening)
    with Core('-O', 'ipc.binary_port_enabled=1') as on:
        assert on.manifest.get('ports.binary', '').isdigit(), on.manifest


def _connect_assorted_clients(core: Core) -> Tuple[KcpcClient,
                                                   List[socket.socket]]:
    """Clients in every state a shutdown can find them in."""
    port = core.binary_port
    said_hello = core.client()
    said_hello.hello('test_kcpc shutdown')
    said_hello.ping()
    silent = connect(port)                      # connected, never spoke
    mid_header = connect(port)
    mid_header.sendall(kc.MAGIC + b'\x00')      # 5 of 16 header bytes
    mid_payload = connect(port)                 # header promises 4096 bytes
    mid_payload.sendall(kc.encode_frame(kc.KIND_REQUEST, kc.OP_HELLO, 1,
                                        b'', length=4096))
    return said_hello, [silent, mid_header, mid_payload]


def test_ipc_quit_with_clients_connected() -> None:
    """Acceptance 5: IPC `quit` ends the core promptly although clients are
    connected in every state, and a client that completed HELLO is told
    SHUTTING_DOWN before its connection closes."""
    with Core() as core:
        said_hello, others = _connect_assorted_clients(core)
        try:
            begin = time.monotonic()
            core.ipc_quit()
            try:
                code = core.wait_exit()
            except subprocess.TimeoutExpired:
                raise AssertionError(
                    f'quit did not end the core within {EXIT_TIMEOUT_S:.0f}s '
                    'with KCPC clients connected') from None
            took = time.monotonic() - begin
            assert code == 0, f'quit exited with {code}'
            print(f'    quit -> exit 0 in {took * 1000:.0f} ms')
            expect_error_and_close(said_hello.sock, kc.SHUTTING_DOWN,
                                   'client at quit')
        finally:
            said_hello.close()
            for sock in others:
                sock.close()


def test_sigterm_with_clients_connected() -> None:
    """Acceptance 5: SIGTERM ends the core with 128+15, promptly, with
    clients connected."""
    if os.name != 'posix':
        print('    (not POSIX: skipped)')
        return
    with Core() as core:
        said_hello, others = _connect_assorted_clients(core)
        try:
            begin = time.monotonic()
            core.proc.send_signal(signal.SIGTERM)
            try:
                code = core.wait_exit()
            except subprocess.TimeoutExpired:
                raise AssertionError(
                    'SIGTERM did not end the core within '
                    f'{EXIT_TIMEOUT_S:.0f}s with KCPC clients connected'
                ) from None
            took = time.monotonic() - begin
            assert code == 128 + signal.SIGTERM, f'SIGTERM exited with {code}'
            print(f'    SIGTERM -> exit {code} in {took * 1000:.0f} ms')
            expect_error_and_close(said_hello.sock, kc.SHUTTING_DOWN,
                                   'client at SIGTERM')
        finally:
            said_hello.close()
            for sock in others:
                sock.close()


TESTS: List[Callable[[], None]] = [
    test_manifest_port_and_hello,
    test_two_clients_concurrently,
    test_pipelined_pings_in_order,
    test_machine_info_stamp,
    test_concurrent_reads_keep_the_run_state,
    test_reads_do_not_change_what_the_machine_computes,
    test_a_reset_by_another_actor_does_not_end_a_frame_step,
    test_mem_read_matches_the_text_ipc,
    test_rom_spaces_and_limits,
    test_regs_get_matches_the_text_ipc,
    test_reads_work_running_and_across_a_pause,
    test_quit_while_clients_read,
    test_bad_frames_close_the_connection,
    test_recoverable_errors_keep_the_connection,
    test_ninth_client_is_refused,
    test_second_instance_gets_its_own_port,
    test_port_can_be_disabled,
    test_ipc_quit_with_clients_connected,
    test_sigterm_with_clients_connected,
]


def main() -> int:
    global MODE, EXE  # pylint: disable=global-statement
    argv = sys.argv[1:]
    only: Optional[str] = None
    if '-k' in argv:
        index = argv.index('-k')
        only = argv[index + 1]
        del argv[index:index + 2]
    if '--ui-free' in argv:
        MODE = 'ui-free'
    elif '--gui' in argv:
        MODE = 'gui'
    argv = [a for a in argv if a not in ('--ui-free', '--gui')]
    if argv:
        EXE = Path(argv[0]).resolve()
    elif os.environ.get('KONCPC_EXE'):
        EXE = Path(os.environ['KONCPC_EXE']).resolve()
    if not EXE.exists():
        print(f'FAIL: {EXE} does not exist; build it first')
        return 1

    print(f'KCPC tests against {EXE} ({MODE})')
    passed = 0
    failed: List[str] = []
    for test in TESTS:
        if only and only not in test.__name__:
            continue
        begin = time.monotonic()
        try:
            test()
        except Exception:  # pylint: disable=broad-except
            failed.append(test.__name__)
            print(f'  FAIL {test.__name__}')
            traceback.print_exc()
            continue
        passed += 1
        print(f'  ok   {test.__name__} '
              f'({time.monotonic() - begin:.1f}s)')
    print(f'Results: {passed} passed, {len(failed)} failed')
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
