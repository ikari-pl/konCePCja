#!/usr/bin/env python3
"""End-to-end tests for the KCPC binary frontend server (beads-3o1).

Each test starts a real core, finds the binary port the way a frontend must
(`ports.binary` of the startup manifest, never a hardcoded 6545) and talks to
it with kcpc_client.py. The suite grows with every slice of
docs/protocol/kcpc-binary-v1.md. Slice 1: framing, HELLO, PING,
MACHINE_INFO, ERROR, the client limit, the manifest line, the
[ipc] binary_port_enabled key and shutdown with clients connected. Slice 2:
the machine reads REGS_GET, MEM_INFO and MEM_READ, checked against the text
IPC, on a running and on a paused machine. Slice 3: the control role and the
machine writes. Slice 4: run control (PAUSE, RUN, RESET, STEP, RUN_STATE)
and the pushed events, which the tests wait for in a blocking read, never
in a polling loop. Slice 5: breakpoints and the steps that walk. Slice 6:
input (KEY, KEYS_RELEASE, TEXT, MOUSE, INPUT_STATE), checked by what the
firmware prints and by what a Z80 program reads from the PPI. Slice 7: what
the DevTools show (CHIP_GET, the ASIC's register page, SYM_*, FB_INFO) and
the events for a change of medium, of the symbols and of the machine.
Slice 9: media (MEDIA_INSERT, MEDIA_EJECT, MEDIA_INFO, TAPE_CONTROL),
snapshots (SNAPSHOT_SAVE, SNAPSHOT_LOAD) and configuration (CONFIG_GET,
CONFIG_SET, CONFIG_APPLY), checked by running a program off a disc, by the
bytes of the files on disk and against the text IPC.

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
import zipfile
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

    def __init__(self, *extra_args: str,
                 extra_env: Optional[Dict[str, str]] = None):
        self.tmpdir = tempfile.mkdtemp(prefix='koncpc-kcpc-')
        # A throwaway copy of the shipped example config: never whoever's
        # koncepcja.cfg sits in the working directory.
        cfg = Path(self.tmpdir) / 'koncepcja.cfg'
        shutil.copyfile(ROOT / 'koncepcja.cfg.example', cfg)
        env = os.environ.copy()
        env['KONCPC_NO_DIALOGS'] = '1'
        env['KONCPC_TEST_WINDOW'] = '1'
        env.update(extra_env or {})
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
        try:
            with socket.create_connection(('127.0.0.1', port),
                                          timeout=IO_TIMEOUT_S) as sock:
                sock.sendall((command + '\n').encode())
                buf = b''
                while b'\n' not in buf:
                    chunk = sock.recv(65536)
                    if not chunk:
                        break
                    buf += chunk
        except OSError as e:
            # A connection the core dropped: say whether the core is still
            # there, and what it last logged (beads-drzi).
            raise AssertionError(
                f'text IPC {command!r}: {e!r}; {self.post_mortem()}') from e
        return buf.decode().strip()

    def post_mortem(self) -> str:
        """Whether the core is alive, its exit code if not, and the end of
        its stderr."""
        try:
            code = self.proc.wait(timeout=2.0)
            state = f'the core exited with {code} (0x{code & 0xFFFFFFFF:08X})'
        except subprocess.TimeoutExpired:
            state = 'the core is still running'
        return (f'{state}; its last stderr:\n'
                + ''.join(self.stderr_lines[-40:]))

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
                           what: str, leading: int = 0,
                           shutdown: bool = False) -> None:
    """After `leading` ordinary frames: exactly one ERROR frame (a response,
    opcode 0, id 0) carrying `status`, then end of stream. With `shutdown`
    the frame before the ERROR must be EVT_SHUTDOWN, and events before that
    are not counted."""
    frames, closed = read_until_closed(sock)
    assert closed, f'{what}: the server did not close the connection'
    if shutdown:
        assert len(frames) >= 2, f'{what}: no EVT_SHUTDOWN before the ERROR'
        bye = frames[-2]
        assert (bye.kind, bye.opcode) == (kc.KIND_EVENT, kc.EVT_SHUTDOWN), (
            f'{what}: the frame before the ERROR is kind {bye.kind} opcode '
            f'{bye.opcode:#06x}, not EVT_SHUTDOWN')
        assert bye.payload == b'', f'{what}: EVT_SHUTDOWN has a payload'
        assert all(f.kind == kc.KIND_EVENT for f in frames[:-1]), (
            f'{what}: {[(f.kind, f.opcode) for f in frames]}')
        frames = frames[-1:]
        leading = 0
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
            assert hello.role == kc.ROLE_OBSERVER, 'it did not ask for control'
            assert hello.max_frame == kc.MAX_SERVER_FRAME, hello.max_frame
            # The groups implemented so far: the machine reads, with the
            # ROM spaces, the writes with the control role, run control,
            # events, breakpoints, input, chip state, symbols, the
            # description of the frame, media, snapshots and configuration.
            assert hello.caps == (kc.CAP_MACHINE_READ | kc.CAP_MEM_ROM
                                  | kc.CAP_MACHINE_WRITE
                                  | kc.CAP_RUN_CONTROL | kc.CAP_EVENTS
                                  | kc.CAP_BREAKPOINTS | kc.CAP_INPUT
                                  | kc.CAP_CHIPS | kc.CAP_SYMBOLS
                                  | kc.CAP_FB_INFO | kc.CAP_MEDIA
                                  | kc.CAP_SNAPSHOT | kc.CAP_CONFIG), (
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
    answers, until stopped. `what` is 'info' (MACHINE_INFO), 'memory' (all
    64 KiB of the CPU space and the registers, in turn: what a live memory
    view and a register window ask for) or 'chips' (all five chips and the
    ASIC's register page, in turn: what the chip windows ask for). On a
    running machine every read is taken by
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
        if self.what == 'chips':
            which = turn % 6
            if which == 5:
                return c.mem_read_stamped(kc.SPACE_ASIC_PAGE, 0,
                                          kc.ASIC_PAGE_SIZE)[0]
            return c.chip_get(kc.CHIP_CRTC + which).stamp
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
                ('space 6', 6, 0),
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


# -- slice 3: the control role and the machine writes (beads-3o1.3) --------

# What the text IPC calls each register of the block.
REG_TEXT_NAMES = {'af': 'AF', 'bc': 'BC', 'de': 'DE', 'hl': 'HL', 'ix': 'IX',
                  'iy': 'IY', 'sp': 'SP', 'pc': 'PC', 'af_alt': 'AFX',
                  'bc_alt': 'BCX', 'de_alt': 'DEX', 'hl_alt': 'HLX',
                  'i': 'I', 'r': 'R', 'iff1': 'IFF1', 'iff2': 'IFF2',
                  'im': 'IM', 'halt': 'HALT'}

SPIN_ADDR = 0x8000   # RAM in every banking state, free at the BASIC prompt
INTERRUPT_VECTOR = 0x0038


def text_reg(core: Core, field: str) -> int:
    resp = core.ipc(f'reg get {REG_TEXT_NAMES[field]}')
    assert resp.startswith('OK '), (field, resp)
    return int(resp[3:], 16)


def controller(core: Core, name: str) -> KcpcClient:
    """A client that asked for control in HELLO and got it."""
    c = core.client()
    hello = c.hello(name, role=kc.ROLE_CONTROLLER)
    assert hello.role == kc.ROLE_CONTROLLER, 'control was not free'
    assert hello.caps & kc.CAP_MACHINE_WRITE, f'caps {hello.caps:#x}'
    return c


def refused(status: int, what: str, call: Callable, *args, **kwargs) -> None:
    try:
        call(*args, **kwargs)
    except KcpcError as e:
        assert e.status == status, (
            f'{what}: {kc.status_name(e.status)}, expected '
            f'{kc.status_name(status)} ({e.message!r})')
        assert e.message, f'{what}: no message'
        return
    raise AssertionError(f'{what}: the request was accepted')


def stop_at_interrupt(core: Core, c: KcpcClient) -> kc.Regs:
    """Stop the machine on a breakpoint at &0038, the interrupt entry. PC is
    then in the lower ROM (so that ROM is paged in) and the machine is parked
    on the fetch of an instruction it has not run."""
    assert core.ipc(f'bp add {INTERRUPT_VECTOR:#x}').startswith('OK')
    assert core.ipc('run').startswith('OK')
    resp = core.ipc('wait bp 10000')
    assert resp.startswith('OK'), f'no breakpoint stop: {resp}'
    assert core.ipc(f'bp del {INTERRUPT_VECTOR:#x}').startswith('OK')
    regs = c.regs_get()
    assert regs.stamp.paused, regs.stamp
    return regs


def wait_running(c: KcpcClient, beyond: int = 0) -> kc.Stamp:
    deadline = time.monotonic() + IO_TIMEOUT_S
    while True:
        stamp = c.machine_info().stamp
        if not stamp.paused and stamp.frame > beyond:
            return stamp
        assert time.monotonic() < deadline, f'not running: {stamp}'
        time.sleep(0.02)


def spin(core: Core, c: KcpcClient) -> None:
    """Leave the machine running `JR $` at SPIN_ADDR with interrupts off: a
    running machine whose registers hold still, so a write to one of them
    can be read back, and cannot crash the firmware under it."""
    pause_core(core, c)
    c.mem_write(kc.SPACE_CPU_RAM, SPIN_ADDR, b'\x18\xfe')
    c.regs_set(pc=SPIN_ADDR, iff1=0, iff2=0)
    assert core.ipc('run').startswith('OK')
    wait_running(c)


def test_control_roles_and_forced_takeover() -> None:
    """Acceptance 1 and the takeover decision: one controller; an observer's
    writes answer NOT_CONTROLLER and change nothing; control is free once
    the controller disconnects; CONTROL with force always wins and demotes
    the previous controller without disconnecting it."""
    with Core() as core, core.client() as a, core.client() as b:
        hello_a = a.hello('test_kcpc first', role=kc.ROLE_CONTROLLER)
        assert hello_a.role == kc.ROLE_CONTROLLER
        hello_b = b.hello('test_kcpc second', role=kc.ROLE_CONTROLLER)
        assert hello_b.role == kc.ROLE_OBSERVER, (
            'HELLO took control away from the controller')
        ram_bytes = hello_a.machine.ram_bytes

        assert core.ipc('wait vbl 60 20000').startswith('OK')
        pause_core(core, a)
        a.regs_set()  # nothing selected; the instruction in flight ends
        regs = a.regs_get().values()
        ram = a.mem_read(kc.SPACE_PHYS_RAM, 0, ram_bytes)

        def unchanged(who: KcpcClient, what: str) -> None:
            assert who.regs_get().values() == regs, f'{what}: registers'
            assert who.mem_read(kc.SPACE_PHYS_RAM, 0, ram_bytes) == ram, (
                f'{what}: memory')

        refused(kc.NOT_CONTROLLER, 'observer REGS_SET', b.regs_set,
                hl=regs['hl'] ^ 0xFFFF, pc=0x1234)
        refused(kc.NOT_CONTROLLER, 'observer MEM_WRITE', b.mem_write,
                kc.SPACE_CPU_RAM, SPIN_ADDR, b'observer')
        refused(kc.NOT_CONTROLLER, 'observer MEM_WRITE to PHYS_RAM',
                b.mem_write, kc.SPACE_PHYS_RAM, 0x4000, b'observer')
        # The role is judged first: not even a malformed write is examined.
        refused(kc.NOT_CONTROLLER, 'observer, empty REGS_SET', b.request,
                kc.OP_REGS_SET)
        unchanged(b, 'an observer wrote')

        # Asking politely is not an error; the reply says who has control.
        asked = b.acquire_control()
        assert not asked.is_controller, asked
        assert asked.controller == hello_a.client_id, asked
        assert b.release_control().controller == hello_a.client_id, (
            'an observer released the controller\'s control')
        assert a.acquire_control().is_controller, 'the controller lost it'

        # The controller disconnects: control is free.
        a.close()
        deadline = time.monotonic() + IO_TIMEOUT_S
        while True:
            asked = b.acquire_control()
            if asked.is_controller:
                break
            assert time.monotonic() < deadline, (
                f'control was not freed by the disconnect: {asked}')
            time.sleep(0.02)
        assert asked.controller == hello_b.client_id, asked
        b.mem_write(kc.SPACE_CPU_RAM, SPIN_ADDR, b'second')
        assert b.mem_read(kc.SPACE_CPU_RAM, SPIN_ADDR, 6) == b'second'

        # Takeover: the newest forced request wins.
        with core.client() as c:
            hello_c = c.hello('test_kcpc third', role=kc.ROLE_CONTROLLER)
            assert hello_c.role == kc.ROLE_OBSERVER
            assert not c.acquire_control().is_controller
            taken = c.acquire_control(force=True)
            assert taken.is_controller, taken
            assert taken.controller == hello_c.client_id, taken

            regs = c.regs_get().values()
            ram = c.mem_read(kc.SPACE_PHYS_RAM, 0, ram_bytes)
            refused(kc.NOT_CONTROLLER, 'demoted MEM_WRITE', b.mem_write,
                    kc.SPACE_CPU_RAM, SPIN_ADDR, b'demoted')
            refused(kc.NOT_CONTROLLER, 'demoted REGS_SET', b.regs_set,
                    de=regs['de'] ^ 0xFFFF)
            unchanged(c, 'a demoted controller wrote')
            # Demoted, not disconnected: it still reads.
            b.ping()
            unchanged(b, 'reading as an observer')
            assert b.acquire_control().controller == hello_c.client_id

            c.mem_write(kc.SPACE_CPU_RAM, SPIN_ADDR, b'third!')
            assert b.mem_read(kc.SPACE_CPU_RAM, SPIN_ADDR, 6) == b'third!'
            # No client is special: the demoted one may force it back.
            back = b.acquire_control(force=True)
            assert back.is_controller, back
            assert back.controller == hello_b.client_id, back
            refused(kc.NOT_CONTROLLER, 'the third, demoted in turn',
                    c.mem_write, kc.SPACE_CPU_RAM, SPIN_ADDR, b'no')
            b.mem_write(kc.SPACE_CPU_RAM, SPIN_ADDR, b'second')
            assert c.mem_read(kc.SPACE_CPU_RAM, SPIN_ADDR, 6) == b'second'
        # Still paused: none of it ran the machine.
        assert b.machine_info().stamp.paused


def test_silent_controller_is_dropped() -> None:
    """Acceptance 2: a controller that sends no frame for the idle timeout
    is disconnected and control becomes free; PING keeps it; an observer is
    never timed out."""
    timeout_s = 1.5
    env = {'KONCPC_KCPC_CONTROLLER_TIMEOUT_MS': str(int(timeout_s * 1000))}
    with Core(extra_env=env) as core, core.client() as a, \
            core.client() as b:
        hello_a = a.hello('test_kcpc pinger', role=kc.ROLE_CONTROLLER)
        assert hello_a.role == kc.ROLE_CONTROLLER
        hello_b = b.hello('test_kcpc quiet observer')

        # PING keeps control for twice the timeout.
        begin = time.monotonic()
        pings = 0
        while time.monotonic() - begin < 2 * timeout_s:
            a.ping()
            pings += 1
            time.sleep(0.1)
        held = a.acquire_control()
        last_frame = time.monotonic()
        assert held.is_controller, (
            f'{pings} pings in {2 * timeout_s:.0f}s did not keep control')

        # Silence. The server says why, then closes.
        frame = a.read_frame()
        waited = time.monotonic() - last_frame
        assert frame.opcode == kc.OP_ERROR and frame.id == 0, frame
        assert frame.status == kc.IDLE_TIMEOUT, kc.status_name(frame.status)
        assert frame.message
        assert waited >= timeout_s - 0.05, (
            f'dropped after {waited:.2f}s of silence, timeout {timeout_s}s')
        try:
            a.read_frame()
            raise AssertionError('the connection stayed open')
        except (KcpcClosed, OSError):
            pass

        # The observer said nothing for all that time and is still there;
        # control is free and it takes it.
        took = b.acquire_control()
        assert took.is_controller, took
        assert took.controller == hello_b.client_id, took
        b.mem_write(kc.SPACE_CPU_RAM, SPIN_ADDR, b'free')
        print(f'    {pings} pings kept control for {2 * timeout_s:.0f}s; '
              f'dropped {waited:.2f}s after the last frame')


def test_regs_set_changes_exactly_one_register() -> None:
    """Acceptance 3: REGS_SET with a one-bit mask changes that register and
    no other, as REGS_GET and the text IPC both see it."""
    with Core() as core, controller(core, 'test_kcpc regs_set') as c:
        assert core.ipc('wait vbl 60 20000').startswith('OK')
        paused = pause_core(core, c)
        # Nothing selected: the block comes back, and the instruction that
        # was in flight at the pause has ended, so nothing else moves below.
        base = c.regs_set()
        assert base.stamp == paused, (base.stamp, paused)
        expect = base.values()
        assert c.regs_get().values() == expect

        for name in kc.REGS_SETTABLE:
            old = expect[name]
            if name == 'im':
                new = (old + 1) % 3
            elif name in ('iff1', 'iff2'):
                new = old ^ 1
            elif name in kc.REG8_NAMES:
                new = old ^ 0x5A
            else:
                new = old ^ 0xA55A
            after = c.regs_set(**{name: new})
            expect[name] = new
            assert after.values() == expect, (
                f'{name}: the reply shows '
                f'{ {k: v for k, v in after.values().items() if v != expect[k]} }')
            assert c.regs_get().values() == expect, f'{name}: REGS_GET'
            assert text_reg(core, name) == new, (
                f'{name}: the text IPC reads {text_reg(core, name):#x}')
            assert after.stamp == paused, (name, after.stamp)
        for name in kc.REGS_SETTABLE:
            assert text_reg(core, name) == expect[name], name
        dump = core.ipc('regs')
        assert f'PC={expect["pc"]:04X}' in dump, dump

        # Several at once, and a field the mask leaves out is not touched
        # whatever the block holds for it.
        both = c.regs_set(bc=0x1111, ix=0x2222)
        expect.update(bc=0x1111, ix=0x2222)
        assert both.values() == expect
        c.request(kc.OP_REGS_SET, kc.regs_set_payload(
            kc.REG_BITS['de'], {'de': 0x3333, 'hl': 0x4444, 'im': 7}))
        expect['de'] = 0x3333
        assert c.regs_get().values() == expect

        # What cannot be set is refused whole.
        for what, payload in (
                ('HALT', kc.regs_set_payload(
                    kc.REG_BITS['halt'] | kc.REG_BITS['hl'],
                    {'halt': 1, 'hl': 0x9999})),
                ('a 19th field', kc.regs_set_payload(1 << 18)),
                ('IM 3', kc.regs_set_payload(kc.REG_BITS['im'], {'im': 3})),
                ('IFF1 2', kc.regs_set_payload(kc.REG_BITS['iff1'],
                                              {'iff1': 2})),
                ('cut short', kc.regs_set_payload(1)[:-1])):
            refused(kc.BAD_PAYLOAD, what, c.request, kc.OP_REGS_SET, payload)
        assert c.regs_get().values() == expect, 'a refused write changed it'
        assert c.machine_info().stamp == paused, 'the machine moved'


def test_pc_edit_at_a_breakpoint_matches_the_text_ipc() -> None:
    """Acceptance 3, second half. At a breakpoint the machine has fetched
    the instruction it stopped on and not run it. The PC both ports show is
    the breakpoint's address; writing that same PC is no edit, and a new PC
    drops the fetched instruction: `reg set PC`'s rules."""
    code = bytes([0x21, 0x34, 0x12, 0x18, 0xFE])   # LD HL,&1234 : JR $
    with Core() as core, controller(core, 'test_kcpc breakpoint') as c:
        assert core.ipc('wait vbl 60 20000').startswith('OK')
        at = stop_at_interrupt(core, c)
        assert at.pc == INTERRUPT_VECTOR, (
            f'REGS_GET shows PC {at.pc:#06x} at a breakpoint on '
            f'{INTERRUPT_VECTOR:#06x}')
        assert text_reg(core, 'pc') == INTERRUPT_VECTOR
        assert at.values() == {n: text_reg(core, n) for n in REG_TEXT_NAMES}
        c.mem_write(kc.SPACE_CPU_RAM, SPIN_ADDR, code)

        # Writing back what is shown, PC included, is no edit: the fetched
        # instruction runs, once. The firmware's is a JP.
        jp = c.mem_read(kc.SPACE_CPU, INTERRUPT_VECTOR, 3)
        assert jp[0] == 0xC3, f'&0038 is not a JP in this ROM: {jp.hex()}'
        same = c.regs_set(**{n: getattr(at, n) for n in kc.REGS_SETTABLE})
        assert same.values() == at.values(), 'writing it back changed it'
        assert same.stamp == at.stamp
        assert core.ipc('step 1').startswith('OK')
        stepped = c.regs_get()
        assert stepped.pc == jp[1] | jp[2] << 8, (
            f'the instruction at the breakpoint went to {stepped.pc:#06x}')
        assert stepped.sp == at.sp

        # A new PC: the instruction fetched at the breakpoint is dropped and
        # the next one comes from there.
        at = stop_at_interrupt(core, c)
        assert at.pc == INTERRUPT_VECTOR
        moved = c.regs_set(pc=SPIN_ADDR, hl=0)
        assert moved.pc == SPIN_ADDR and moved.hl == 0, moved
        assert moved.sp == at.sp and moved.stamp == at.stamp, moved
        assert text_reg(core, 'pc') == SPIN_ADDR
        assert core.ipc('step 1').startswith('OK')
        binary = c.regs_get()
        assert (binary.pc, binary.hl) == (SPIN_ADDR + 3, 0x1234), binary
        assert binary.sp == at.sp, 'the dropped instruction ran'

        # The same through the text IPC ends in the same place. Interrupts
        # back on, so the loop is interrupted and the breakpoint reached.
        c.regs_set(iff1=1, iff2=1)
        at = stop_at_interrupt(core, c)
        assert at.pc == INTERRUPT_VECTOR
        c.regs_set(hl=0)
        assert core.ipc(f'reg set PC {SPIN_ADDR:#x}').startswith('OK')
        assert c.regs_get().pc == SPIN_ADDR
        assert core.ipc('step 1').startswith('OK')
        text = c.regs_get()
        assert (text.pc, text.hl) == (binary.pc, binary.hl), (text, binary)
        assert text.sp == at.sp
        assert text.stamp.paused


def test_mem_write_lands_in_the_ram_it_names() -> None:
    """Acceptance 4: a CPU_RAM write under the lower ROM reads back through
    CPU_RAM and not through CPU; a PHYS_RAM write lands in the right 16K
    page of the expansion RAM and nowhere else; what is not RAM, or does not
    fit, is refused whole."""
    with Core() as core, controller(core, 'test_kcpc mem_write') as c:
        assert core.ipc('wait vbl 60 20000').startswith('OK')
        at = stop_at_interrupt(core, c)   # in the lower ROM: it is paged in
        info = c.mem_info()
        assert info.lower_rom_on, info
        assert info.ram_config & 0x07 == 0, info
        ram_bytes = info.ram_bytes
        assert ram_bytes >= 0x20000, 'the example config is a 128K machine'
        expect = bytearray(c.mem_read(kc.SPACE_PHYS_RAM, 0, ram_bytes))

        def phys_matches(what: str) -> None:
            now = c.mem_read(kc.SPACE_PHYS_RAM, 0, ram_bytes)
            if now != expect:
                first = next(i for i in range(ram_bytes)
                             if now[i] != expect[i])
                raise AssertionError(
                    f'{what}: physical RAM differs from &{first:05X} on '
                    f'({now[first]:#04x}, expected {expect[first]:#04x})')

        # Under the lower ROM.
        under = 0x0100
        rom = c.mem_read(kc.SPACE_ROM_LOWER, under, 16)
        data = bytes(range(0xA0, 0xB0))
        assert data != rom and bytes(expect[under:under + 16]) != data
        stamp = c.mem_write(kc.SPACE_CPU_RAM, under, data)
        assert stamp == at.stamp, (stamp, at.stamp)
        expect[under:under + 16] = data
        assert c.mem_read(kc.SPACE_CPU_RAM, under, 16) == data
        assert c.mem_read(kc.SPACE_CPU, under, 16) == rom, (
            'the CPU view shows the write, not the ROM over it')
        assert ipc_bytes(core, f'mem read {under:#x} 16 --view=ram') == data
        assert ipc_bytes(core, f'mem read {under:#x} 16') == rom
        phys_matches('CPU_RAM write under the ROM')

        # Expansion RAM: page 5 is the second 16K above the base 64K.
        page, offset = 5, 0x0123
        patch = b'KCPC\x00\xff\x55\xaa'
        c.mem_write(kc.SPACE_PHYS_RAM, page * 0x4000 + offset, patch)
        expect[page * 0x4000 + offset:page * 0x4000 + offset + 8] = patch
        assert ipc_bytes(
            core, f'mem read {offset:#x} 8 --bank={page}') == patch
        phys_matches('PHYS_RAM write to page 5')
        assert patch not in c.mem_read(kc.SPACE_CPU_RAM, 0,
                                       kc.CPU_SPACE_SIZE), (
            'expansion RAM that is not banked in showed up in the CPU view')

        # Refused, and nothing written.
        for space in (kc.SPACE_CPU, kc.SPACE_ROM_LOWER, kc.SPACE_ROM_UPPER,
                      kc.SPACE_ASIC_PAGE, 9, 255):
            refused(kc.BAD_PAYLOAD, f'space {space}', c.mem_write, space,
                    0x8000, b'no')
        refused(kc.BAD_PAYLOAD, 'a bank for CPU_RAM', c.mem_write,
                kc.SPACE_CPU_RAM, 0x8000, b'no', 1)
        refused(kc.BAD_PAYLOAD, 'cut short', c.request, kc.OP_MEM_WRITE,
                kc.mem_write_payload(kc.SPACE_CPU_RAM, 0, b'')[:-1])
        for what, space, addr, size in (
                ('CPU_RAM across the end', kc.SPACE_CPU_RAM, 0xFFFF, 2),
                ('CPU_RAM past the end', kc.SPACE_CPU_RAM, 0x10000, 1),
                ('PHYS_RAM across the end', kc.SPACE_PHYS_RAM,
                 ram_bytes - 1, 2),
                ('PHYS_RAM past the end', kc.SPACE_PHYS_RAM, ram_bytes, 1),
                ('PHYS_RAM at 4 GiB', kc.SPACE_PHYS_RAM, 0xFFFFFFFF, 2)):
            refused(kc.OUT_OF_RANGE, what, c.mem_write, space, addr,
                    b'\xEE' * size)
        phys_matches('a refused write')

        # The edges that do fit.
        c.mem_write(kc.SPACE_CPU_RAM, 0x10000, b'')
        c.mem_write(kc.SPACE_PHYS_RAM, ram_bytes, b'')
        c.mem_write(kc.SPACE_CPU_RAM, 0xFFFF, b'\x5A')
        c.mem_write(kc.SPACE_PHYS_RAM, ram_bytes - 1, b'\xA5')
        expect[0xFFFF] = 0x5A
        expect[ram_bytes - 1] = 0xA5
        phys_matches('writes at the last byte of each space')

        # All 64 KiB of the CPU_RAM view in one request, then all of
        # physical RAM in one request.
        blob = bytes(((i * 7) ^ (i >> 8)) & 0xFF
                     for i in range(kc.CPU_SPACE_SIZE))
        c.mem_write(kc.SPACE_CPU_RAM, 0, blob)
        assert c.mem_read(kc.SPACE_CPU_RAM, 0, kc.CPU_SPACE_SIZE) == blob
        expect[0:kc.CPU_SPACE_SIZE] = blob
        phys_matches('a 64 KiB write')
        whole = bytes((i * 13 + (i >> 9)) & 0xFF for i in range(ram_bytes))
        c.mem_write(kc.SPACE_PHYS_RAM, 0, whole)
        assert c.mem_read(kc.SPACE_PHYS_RAM, 0, ram_bytes) == whole

        assert c.machine_info().stamp == at.stamp, 'the machine moved'


def test_writes_leave_the_run_state_alone() -> None:
    """Acceptance 5: a write while running leaves the machine running, a
    write while paused leaves it paused, and reads are still taken at a
    frame boundary, with nothing paused for them."""
    with Core() as core:
        with controller(core, 'test_kcpc run state') as c:
            assert core.ipc('wait vbl 60 20000').startswith('OK')
            spin(core, c)
            before = c.machine_info().stamp
            running_reads = 1
            for i in range(40):
                after = c.regs_set(de=0x1000 + i, iy=0xFF00 - i)
                assert not after.stamp.paused, f'write {i} says paused'
                assert (after.de, after.iy) == (0x1000 + i, 0xFF00 - i)
                stamp = c.mem_write(kc.SPACE_CPU_RAM, 0x9000,
                                    bytes([i]) * 64)
                assert not stamp.paused, f'write {i} says paused'
                got = c.regs_get()
                assert not got.stamp.paused, f'paused after write {i}'
                assert (got.de, got.iy) == (0x1000 + i, 0xFF00 - i), got
                assert SPIN_ADDR <= got.pc <= SPIN_ADDR + 2, hex(got.pc)
                assert c.mem_read(kc.SPACE_CPU_RAM, 0x9000,
                                  64) == bytes([i]) * 64
                running_reads += 2
            end = wait_running(c, before.frame + 5)
            running_reads += 1

            # A PC written while it runs lands between two instructions and
            # the machine carries on from there.
            c.mem_write(kc.SPACE_CPU_RAM, SPIN_ADDR + 0x100,
                        bytes([0x3E, 0x55, 0x18, 0xFE]))  # LD A,&55 : JR $
            moved = c.regs_set(pc=SPIN_ADDR + 0x100)
            assert not moved.stamp.paused and moved.pc == SPIN_ADDR + 0x100
            deadline = time.monotonic() + IO_TIMEOUT_S
            while True:
                got = c.regs_get()
                running_reads += 1
                if got.af >> 8 == 0x55:
                    break
                assert time.monotonic() < deadline, f'never got there: {got}'
            assert SPIN_ADDR + 0x102 <= got.pc <= SPIN_ADDR + 0x104, got
            assert not got.stamp.paused

            # Paused: every write says so and the frame count stands still.
            paused = pause_core(core, c)
            for i in range(20):
                after = c.regs_set(bc=i)
                assert after.stamp == paused and after.bc == i, after
                assert c.mem_write(kc.SPACE_PHYS_RAM, 0x9000,
                                   bytes([i]) * 64) == paused
            time.sleep(0.2)
            assert c.machine_info().stamp == paused, 'a write ran the machine'
            # And no lease was left behind: it runs when told to.
            assert core.ipc('run').startswith('OK')
            wait_running(c, paused.frame + 2)

        core.ipc_quit()
        assert core.wait_exit() == 0
        served = core.reads_served()
        # Every read of the running phase was taken by the runner at a frame
        # boundary; none fell back to a pause lease.
        assert served['fallback'] == 0 and served['busy'] == 0, served
        assert served['boundary'] >= running_reads, (served, running_reads)
        print(f'    80 writes while running over {end.frame - before.frame} '
              f'frames, 40 while paused; reads served {served}')


def test_readers_and_a_writer_at_once() -> None:
    """Observers reading 64 KiB and the registers as fast as they can while
    the controller writes: every reply arrives, every read of the running
    machine says running, and the machine is still running afterwards."""
    with Core() as core, controller(core, 'test_kcpc writer') as c:
        assert core.ipc('wait vbl 60 20000').startswith('OK')
        spin(core, c)
        before = c.machine_info().stamp
        writes = 0
        with Readers(core, 3, 'memory') as readers:
            begin = time.monotonic()
            while time.monotonic() - begin < 1.5:
                value = 0x4000 + writes
                assert c.regs_set(hl=value).hl == value
                c.mem_write(kc.SPACE_CPU_RAM, 0x9000,
                            value.to_bytes(2, 'little') * 512)
                writes += 1
        assert not readers.errors, readers.errors
        # Three readers at a boundary each, 50 boundaries a second: a writer
        # that left the runner no frames got 15 reads through here, each one
        # by the gate's 250 ms fallback.
        assert readers.calls > 45, f'only {readers.calls} reads'
        assert readers.running_reads == readers.calls, (
            f'{readers.calls - readers.running_reads} of {readers.calls} '
            'reads were stamped paused: a write\'s lease leaked into a read')
        # The writer gives way to the readers: beside waiting reads a write
        # lets a frame boundary pass first, so it gets a few dozen in, not
        # thousands.
        assert writes >= 2, f'only {writes} rounds of writes in 1.5 s'
        last = 0x4000 + writes - 1
        assert c.regs_get().hl == last
        assert c.mem_read(kc.SPACE_CPU_RAM, 0x9000,
                          1024) == last.to_bytes(2, 'little') * 512
        wait_running(c, before.frame + 5)
        print(f'    {writes * 2} writes beside {readers.calls} reads from 3 '
              f'observers; slowest read {readers.slowest * 1000:.0f} ms')


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


# -- slice 4: run control and pushed events ---------------------------------

EVENT_TIMEOUT_S = 10.0


def observer(core: Core, name: str, classes: int = 0,
             frame_interval: int = 0) -> KcpcClient:
    c = core.client()
    hello = c.hello(name)
    assert hello.caps & kc.CAP_EVENTS, f'caps {hello.caps:#x}'
    if classes:
        assert c.subscribe(classes, frame_interval) == (classes,
                                                       frame_interval)
    return c


def run_event(c: KcpcClient) -> kc.Event:
    """The next EVT_STOPPED, EVT_RESUMED or EVT_RESET: a blocking read."""
    while True:
        event = c.next_event(EVENT_TIMEOUT_S)
        if event.opcode in (kc.EVT_STOPPED, kc.EVT_RESUMED, kc.EVT_RESET):
            return event


def expect_stopped(c: KcpcClient, reason: int, client: int, request: int,
                   who: str) -> kc.Event:
    event = run_event(c)
    assert event.opcode == kc.EVT_STOPPED, f'{who}: {event}'
    assert (event.reason, event.cause_client, event.cause_request) == (
        reason, client, request), f'{who}: {event}'
    return event


def expect_resumed(c: KcpcClient, client: int, request: int,
                   who: str) -> kc.Event:
    event = run_event(c)
    assert event.opcode == kc.EVT_RESUMED, f'{who}: {event}'
    assert (event.cause_client, event.cause_request) == (client, request), (
        f'{who}: {event}')
    return event


def test_pause_and_run_are_pushed_to_every_subscriber() -> None:
    """Acceptance 1: the controller pauses; it and an observer subscribed to
    RUN both receive EVT_STOPPED, reason PAUSED, naming the controller and
    its request. RUN gives both EVT_RESUMED. No polling: every wait is a
    blocking read of the socket. The requester sees its reply first, the
    event after it, and the event describes the machine a read then finds."""
    with Core() as core, controller(core, 'pusher') as a, \
            observer(core, 'watcher', kc.CLASS_RUN) as b:
        hello = a.hello_reply
        assert hello.caps & kc.CAP_RUN_CONTROL, f'caps {hello.caps:#x}'
        a.subscribe(kc.CLASS_RUN)
        a_id = hello.client_id
        state = a.run_state()
        assert not state.stamp.paused and not state.walk_active, state
        assert state.controller == a_id, state

        stamp = a.pause()
        pause_id = a.last_request_id
        assert not [e for e in a.events if e.opcode == kc.EVT_STOPPED], (
            'EVT_STOPPED arrived before the reply to the PAUSE that caused '
            'it')
        assert stamp.paused, f'the PAUSE reply says running: {stamp}'
        for who, c in (('controller', a), ('observer', b)):
            event = expect_stopped(c, kc.STOP_PAUSED, a_id, pause_id, who)
            assert event.frame == stamp.frame, (who, event, stamp)
        regs = b.regs_get()
        assert regs.stamp.paused and regs.stamp.frame == event.frame, (
            regs.stamp, event)
        assert regs.pc == event.pc, (
            f'EVT_STOPPED says pc {event.pc:#06x}, REGS_GET {regs.pc:#06x}')
        assert a.run_state().stamp.paused

        # A second PAUSE changes nothing and announces nothing: the next
        # event either client sees is the RUN's.
        a.pause()
        a.run()
        run_id = a.last_request_id
        for who, c in (('controller', a), ('observer', b)):
            expect_resumed(c, a_id, run_id, who)
        assert not a.run_state().stamp.paused

        # An observer may ask for the state and may not change it.
        refused(kc.NOT_CONTROLLER, 'observer PAUSE', b.pause)
        refused(kc.NOT_CONTROLLER, 'observer RUN', b.run)
        refused(kc.NOT_CONTROLLER, 'observer STEP', b.step)
        refused(kc.NOT_CONTROLLER, 'observer RESET', b.reset)
        assert b.run_state().controller == a_id


def test_stops_from_the_text_ipc_are_pushed() -> None:
    """Acceptance 2: a pause on the text IPC port reaches both binary
    clients with cause_client 0, and so do its run, its single step and its
    frame step."""
    with Core() as core, controller(core, 'ctl') as a, \
            observer(core, 'obs', kc.CLASS_RUN) as b:
        a.subscribe(kc.CLASS_RUN)
        assert core.ipc('pause').startswith('OK')
        for who, c in (('controller', a), ('observer', b)):
            event = expect_stopped(c, kc.STOP_PAUSED, 0, 0, who)
        assert b.regs_get().stamp.frame == event.frame
        # A single step of a paused machine changes no run state, and is
        # announced all the same: the registers moved.
        assert core.ipc('step').startswith('OK')
        for who, c in (('controller', a), ('observer', b)):
            expect_stopped(c, kc.STOP_STEP_DONE, 0, 0, who)
        assert core.ipc('step frame 3').startswith('OK')
        for who, c in (('controller', a), ('observer', b)):
            expect_resumed(c, 0, 0, who)
            done = expect_stopped(c, kc.STOP_STEP_DONE, 0, 0, who)
            assert done.frame == event.frame + 3, (event, done)
        assert core.ipc('run').startswith('OK')
        for who, c in (('controller', a), ('observer', b)):
            expect_resumed(c, 0, 0, who)


def test_reads_and_writes_are_not_run_state_events() -> None:
    """Acceptance 3: a client reading and a controller writing at every
    frame pause the machine again and again under their leases, and none of
    it is announced. The first run-state event anybody sees afterwards is
    the PAUSE that ends the test."""
    with Core() as core, controller(core, 'writer') as a, \
            observer(core, 'reader') as r, \
            observer(core, 'watcher', kc.CLASS_RUN) as b:
        a.subscribe(kc.CLASS_RUN)
        spin(core, a)           # text IPC pause and run: two events each
        for c in (a, b):
            assert run_event(c).opcode == kc.EVT_STOPPED
            assert run_event(c).opcode == kc.EVT_RESUMED
        writes = 0
        first = a.machine_info().stamp
        for turn in range(60):
            data = r.mem_read(kc.SPACE_CPU, 0, kc.CPU_SPACE_SIZE)
            assert len(data) == kc.CPU_SPACE_SIZE
            stamp = a.mem_write(kc.SPACE_CPU_RAM, 0x9000, bytes([turn]))
            assert not stamp.paused, f'write {turn}: {stamp}'
            a.regs_set(hl=turn)
            writes += 2
        last = a.machine_info().stamp
        assert not last.paused and last.frame > first.frame, (first, last)
        a.pause()
        for who, c in (('controller', a), ('observer', b)):
            expect_stopped(c, kc.STOP_PAUSED, a.hello_reply.client_id,
                           a.last_request_id, who)
        print(f'    {writes} writes and 60 reads over '
              f'{last.frame - first.frame} frames: no run-state event')


def test_step_in_ends_with_an_event() -> None:
    """Acceptance 4, first half: STEP in N answers, then EVT_STOPPED with
    reason STEP_DONE and the step's request id arrives, and PC has advanced
    by N instructions. The stepping client is told although it subscribed
    to nothing; an observer that subscribed to RUN is told too."""
    count = 5
    with Core() as core, controller(core, 'stepper') as a, \
            observer(core, 'watcher', kc.CLASS_RUN) as b:
        a_id = a.hello_reply.client_id
        pause_core(core, a)
        expect_stopped(b, kc.STOP_PAUSED, 0, 0, 'observer')
        # Eight one-byte instructions (INC A) and a JR $ behind them.
        a.mem_write(kc.SPACE_CPU_RAM, SPIN_ADDR, b'\x3c' * 8 + b'\x18\xfe')
        before = a.regs_set(pc=SPIN_ADDR, af=0, iff1=0, iff2=0)
        step_id = a.step(count)
        assert not [e for e in a.events if e.opcode == kc.EVT_STOPPED], (
            'the step\'s EVT_STOPPED arrived before its reply')
        for who, c in (('stepper', a), ('observer', b)):
            event = expect_stopped(c, kc.STOP_STEP_DONE, a_id, step_id, who)
            assert event.pc == SPIN_ADDR + count, f'{who}: {event}'
            assert event.frame == before.stamp.frame, (who, event)
        regs = a.regs_get()
        assert regs.stamp.paused
        assert regs.pc == SPIN_ADDR + count, f'pc {regs.pc:#06x}'
        assert regs.af >> 8 == count, f'A = {regs.af >> 8}, not {count}'
        assert text_reg(core, 'pc') == regs.pc

        # A step from a running machine pauses it first.
        a.run()
        expect_resumed(b, a_id, a.last_request_id, 'observer')
        step_id = a.step()      # count 0 on the wire would mean 1 as well
        expect_stopped(a, kc.STOP_STEP_DONE, a_id, step_id, 'stepper')
        expect_stopped(b, kc.STOP_STEP_DONE, a_id, step_id, 'observer')
        assert a.run_state().stamp.paused

        refused(kc.OUT_OF_RANGE, 'STEP in over the limit', a.step,
                kc.MAX_STEP_IN_COUNT + 1)
        refused(kc.BAD_PAYLOAD, 'STEP mode 9', a.step, 1, 9)
        refused(kc.BAD_PAYLOAD, 'STEP with no payload', a.request,
                kc.OP_STEP)


def test_step_frames_counts_like_the_text_ipc() -> None:
    """Acceptance 4, second half: STEP frames N answers once it is armed,
    runs exactly N frames (as the text IPC's `step frame N` does) and ends
    with EVT_STOPPED STEP_DONE. While it runs RUN_STATE says so, a second
    STEP answers BUSY, and PAUSE ends it."""
    frames = 7
    with Core() as core, controller(core, 'stepper') as a, \
            observer(core, 'watcher', kc.CLASS_RUN) as b:
        a_id = a.hello_reply.client_id
        start = a.pause()
        expect_stopped(b, kc.STOP_PAUSED, a_id, a.last_request_id, 'observer')

        step_id = a.step(frames, kc.STEP_FRAMES)
        expect_resumed(b, a_id, step_id, 'observer')
        for who, c in (('stepper', a), ('observer', b)):
            done = expect_stopped(c, kc.STOP_STEP_DONE, a_id, step_id, who)
            assert done.frame == start.frame + frames, (who, start, done)
        kcpc_end = a.regs_get().stamp
        assert kcpc_end.paused and kcpc_end.frame == start.frame + frames

        assert core.ipc(f'step frame {frames}').startswith('OK')
        text_end = a.regs_get().stamp
        assert text_end.frame - kcpc_end.frame == frames, (
            f'text `step frame {frames}` ran '
            f'{text_end.frame - kcpc_end.frame} frames, STEP frames '
            f'{kcpc_end.frame - start.frame}')
        expect_resumed(b, 0, 0, 'observer')
        expect_stopped(b, kc.STOP_STEP_DONE, 0, 0, 'observer')

        # A long step: visible in RUN_STATE, exclusive, and ended by PAUSE.
        long_id = a.step(1_000_000, kc.STEP_FRAMES)
        expect_resumed(b, a_id, long_id, 'observer')
        state = a.run_state()
        assert state.walk_active and not state.stamp.paused, state
        refused(kc.BUSY, 'a second STEP during a step', a.step)
        refused(kc.BUSY, 'a second frame step', a.step, 2, kc.STEP_FRAMES)
        a.pause()
        pause_id = a.last_request_id
        for who, c in (('stepper', a), ('observer', b)):
            expect_stopped(c, kc.STOP_PAUSED, a_id, pause_id, who)
        state = a.run_state()
        assert not state.walk_active and state.stamp.paused, state
        # The counter of the step that was cut short is gone: the next
        # step runs its own frames and no others.
        here = a.regs_get().stamp
        step_id = a.step(2, kc.STEP_FRAMES)
        done = expect_stopped(a, kc.STOP_STEP_DONE, a_id, step_id, 'stepper')
        assert done.frame == here.frame + 2, (here, done)


def test_reset_is_pushed() -> None:
    """RESET resets the CPC and leaves it running, or stopped with the
    stay-paused flag, whichever it was; EVT_RESET names the request and the
    run-state event follows it."""
    with Core() as core, controller(core, 'resetter') as a, \
            observer(core, 'watcher', kc.CLASS_RUN) as b:
        a_id = a.hello_reply.client_id
        spin(core, a)
        for _ in range(2):
            run_event(b)        # the text IPC's pause and run inside spin()
        assert a.regs_get().pc == SPIN_ADDR

        # Running, stay paused: reset, then stopped.
        stamp = a.reset(stay_paused=True)
        reset_id = a.last_request_id
        assert stamp.paused, stamp
        event = run_event(b)
        assert (event.opcode, event.cause_client, event.cause_request) == (
            kc.EVT_RESET, a_id, reset_id), event
        expect_stopped(b, kc.STOP_PAUSED, a_id, reset_id, 'observer')
        regs = a.regs_get()
        assert regs.pc == 0 and regs.stamp.paused, regs
        assert (regs.iff1, regs.im) == (0, 0), regs

        # Paused, plain reset: reset, then running.
        stamp = a.reset()
        reset_id = a.last_request_id
        assert not stamp.paused, stamp
        event = run_event(b)
        assert (event.opcode, event.cause_request) == (kc.EVT_RESET,
                                                      reset_id), event
        expect_resumed(b, a_id, reset_id, 'observer')

        # Running, plain reset: the machine keeps running, so the reset is
        # the only event; the PAUSE behind it is the next.
        a.reset()
        reset_id = a.last_request_id
        event = run_event(b)
        assert (event.opcode, event.cause_request) == (kc.EVT_RESET,
                                                      reset_id), event
        a.pause()
        expect_stopped(b, kc.STOP_PAUSED, a_id, a.last_request_id,
                       'observer')
        refused(kc.BAD_PAYLOAD, 'RESET with no payload', a.request,
                kc.OP_RESET)


class LeaseTaker:
    """Another actor that holds one pause lease after another: the text IPC
    writing a byte of memory in a loop, on its own connections."""

    def __init__(self, core: Core):
        self.core = core
        self.stop = threading.Event()
        self.writes = 0
        self.errors: List[str] = []
        self.thread = threading.Thread(target=self._run)

    def _run(self) -> None:
        try:
            while not self.stop.is_set():
                resp = self.core.ipc('mem write 0x9000 00')
                assert resp.startswith('OK'), resp
                self.writes += 1
        except Exception as exc:  # noqa: BLE001 - reported by the test
            self.errors.append(f'{type(exc).__name__}: {exc}')

    def __enter__(self) -> 'LeaseTaker':
        self.thread.start()
        return self

    def __exit__(self, *exc) -> None:
        self.stop.set()
        self.thread.join()


def test_run_is_not_lost_to_another_actors_lease() -> None:
    """RUN must not vanish because somebody else happens to hold a pause
    lease (beads-pja3): with the text IPC writing memory in a loop on its
    own connections, every PAUSE is followed by EVT_STOPPED and every RUN
    by EVT_RESUMED, and nothing else is announced."""
    turns = 40
    with Core() as core, controller(core, 'runner') as a:
        a.subscribe(kc.CLASS_RUN)
        a_id = a.hello_reply.client_id
        with LeaseTaker(core) as other:
            turn = 0
            # At least `turns` pairs, and on until the other actor has taken
            # more leases than that: a text IPC write is a connection of its
            # own and on a slow host (the Windows CI runner) falls behind
            # the pairs, which then proved little. Bounded all the same.
            while turn < turns or (other.writes <= turns
                                   and turn < 50 * turns
                                   and not other.errors):
                a.pause()
                expect_stopped(a, kc.STOP_PAUSED, a_id, a.last_request_id,
                               f'turn {turn}')
                a.run()
                expect_resumed(a, a_id, a.last_request_id, f'turn {turn}')
                turn += 1
        assert not other.errors, other.errors
        assert other.writes > turns, (
            f'only {other.writes} leases by the other actor in {turn} '
            'turns: this proved little')
        print(f'    {turn} pause/run pairs beside {other.writes} text IPC '
              'writes: every RUN took effect')


def test_frame_events_arrive_at_the_interval() -> None:
    """Acceptance 5, first half: with frame_interval 5, EVT_FRAME arrives
    for every fifth frame and the events on the connection are numbered
    without a gap. SUBSCRIBE replaces the subscription."""
    with Core() as core, observer(core, 'frames') as c:
        assert c.subscribe(kc.CLASS_FRAME | 0x8000_0000, 5) == (
            kc.CLASS_FRAME, 5), 'an unknown class bit is dropped, not refused'
        events = [c.next_event(EVENT_TIMEOUT_S) for _ in range(8)]
        assert all(e.opcode == kc.EVT_FRAME for e in events), events
        frames = [e.frame for e in events]
        assert all(f % 5 == 0 for f in frames), frames
        assert frames == list(range(frames[0], frames[0] + 40, 5)), frames
        seqs = [e.seq for e in events]
        assert seqs == list(range(seqs[0], seqs[0] + 8)), seqs
        assert seqs[0] == 1, f'the first event on a connection is 1: {seqs}'
        # A frame read now is the frame the events are counting.
        stamp = c.machine_info().stamp
        assert stamp.frame >= frames[-1], (stamp, frames)

        # No class, no events: the PING's reply is the next frame.
        assert c.subscribe(0) == (0, 0)
        c.events.clear()
        time.sleep(0.2)
        c.ping()
        assert not c.events, f'events after unsubscribing: {c.events}'
        refused(kc.BAD_PAYLOAD, 'SUBSCRIBE with no payload', c.request,
                kc.OP_SUBSCRIBE)


def test_a_client_that_stops_reading_is_told_what_it_missed() -> None:
    """Acceptance 5, second half: a client that stops reading has its events
    dropped once its queue is over the event cap, and learns how many from
    EVT_OVERRUN when it reads again: the frames it received and the counts
    it was given add up to every frame. Another subscriber misses nothing
    meanwhile."""
    cap = 64 * 1024
    read_size = 128 * 1024
    reads = 128       # 16 MiB of replies: more than any socket buffer holds
    with Core(extra_env={'KONCPC_KCPC_EVENT_QUEUE_BYTES': str(cap)}) as core, \
            observer(core, 'steady', kc.CLASS_FRAME, 1) as steady:
        slow = core.client()
        slow.hello('slow')
        assert slow.mem_info().ram_bytes >= read_size
        slow.subscribe(kc.CLASS_FRAME, 1)
        first = slow.next_event(EVENT_TIMEOUT_S)
        assert first.opcode == kc.EVT_FRAME, first

        # Stop reading, with a lot of replies on their way.
        ids = [slow.send_request(kc.OP_MEM_READ, kc.mem_read_payload(
            kc.SPACE_PHYS_RAM, 0, read_size)) for _ in range(reads)]
        # Each read is taken at a frame boundary: when the steady client has
        # seen this many more frames, the replies have all been produced.
        seen = [steady.next_event(EVENT_TIMEOUT_S) for _ in range(reads + 40)]
        assert all(e.opcode == kc.EVT_FRAME for e in seen), (
            'the steady client was sent something else: '
            f'{[e.name for e in seen if e.opcode != kc.EVT_FRAME]}')
        frames = [e.frame for e in seen]
        assert frames == list(range(frames[0], frames[0] + len(frames))), (
            'the steady client missed frames while another client stalled')

        # Read again. Every reply is there, and the events account for
        # every frame: received, or counted in an EVT_OVERRUN.
        for request_id in ids:
            frame = slow.read_response(request_id)
            assert frame.status == kc.OK, kc.status_name(frame.status)
        events = [kc.Event.decode(f) for f in slow.events]
        slow.events.clear()
        # The last count comes once the queue has drained, behind the last
        # reply; read on until the events have caught up with the frames the
        # steady client saw.
        while not (events and events[-1].opcode == kc.EVT_FRAME
                   and events[-1].frame >= frames[-1]):
            events.append(slow.next_event(EVENT_TIMEOUT_S))
        expected = first.frame + 1
        seq = first.seq + 1
        dropped = 0
        for event in events:
            assert event.seq == seq, f'event ids have a gap at {event}'
            seq += 1
            if event.opcode == kc.EVT_OVERRUN:
                assert event.dropped > 0, event
                dropped += event.dropped
                expected += event.dropped
                continue
            assert event.opcode == kc.EVT_FRAME, event
            assert event.frame == expected, (
                f'frame {event.frame} after {dropped} reported lost: '
                f'expected frame {expected}')
            expected += 1
        assert dropped > reads // 2, (
            f'only {dropped} events were dropped while {reads} replies '
            'backed up: the test proved little')
        print(f'    stalled client: {dropped} events dropped and reported, '
              f'{len(events)} delivered, every frame accounted for')
        slow.close()


def test_control_changes_reach_every_client() -> None:
    """Every change of controller is pushed to every client, whatever it
    subscribed to: the demoted controller learns of a takeover from the
    event, not from a refused request."""
    with Core() as core, controller(core, 'first') as a, \
            observer(core, 'second') as b:
        a_id = a.hello_reply.client_id
        b_id = b.hello_reply.client_id
        event = a.wait_event(kc.EVT_CONTROL_CHANGED, EVENT_TIMEOUT_S)
        assert (event.controller, event.previous, event.reason) == (
            a_id, 0, kc.CONTROL_ACQUIRED), event
        assert event.seq == 1, event

        assert b.acquire_control(force=True).is_controller
        assert not [e for e in b.events
                    if e.opcode == kc.EVT_CONTROL_CHANGED], (
            'the event arrived before the reply to the CONTROL behind it')
        for who, c in (('demoted', a), ('new controller', b)):
            event = c.wait_event(kc.EVT_CONTROL_CHANGED, EVENT_TIMEOUT_S)
            assert (event.controller, event.previous, event.reason) == (
                b_id, a_id, kc.CONTROL_TAKEN), (who, event)
        refused(kc.NOT_CONTROLLER, 'the demoted controller', a.pause)

        b.release_control()
        for who, c in (('first', a), ('second', b)):
            event = c.wait_event(kc.EVT_CONTROL_CHANGED, EVENT_TIMEOUT_S)
            assert (event.controller, event.previous, event.reason) == (
                0, b_id, kc.CONTROL_RELEASED), (who, event)

        assert a.acquire_control().is_controller
        a.wait_event(kc.EVT_CONTROL_CHANGED, EVENT_TIMEOUT_S)
        b.wait_event(kc.EVT_CONTROL_CHANGED, EVENT_TIMEOUT_S)
        a.close()
        event = b.wait_event(kc.EVT_CONTROL_CHANGED, EVENT_TIMEOUT_S)
        assert (event.controller, event.previous, event.reason) == (
            0, a_id, kc.CONTROL_LEFT), event


def _stepped_by_kcpc(core: Core, frames: int, watchers: int) -> Tuple:
    """From a reset that stays paused, run `frames` frames with STEP frames
    while `watchers` clients are subscribed to every event (a frame event
    per frame), and describe the machine afterwards."""
    with controller(core, 'determinism') as a:
        a_id = a.hello_reply.client_id
        a.reset(stay_paused=True)
        before = a.regs_get().stamp
        assert before.paused
        clients = [observer(core, f'subscriber {i}', kc.CLASS_ALL, 1)
                   for i in range(watchers)]
        try:
            step_id = a.step(frames, kc.STEP_FRAMES)
            done = expect_stopped(a, kc.STOP_STEP_DONE, a_id, step_id,
                                  'stepper')
            received = []
            for c in clients:
                got = 0
                while True:
                    event = c.next_event(EVENT_TIMEOUT_S)
                    if event.opcode == kc.EVT_FRAME:
                        got += 1
                    elif (event.opcode == kc.EVT_STOPPED
                          and event.cause_request == step_id):
                        break
                received.append(got)
        finally:
            for c in clients:
                c.close()
        regs = core.ipc('regs') + ''.join(
            ' ' + core.ipc(f'reg get {name}')
            for name in ('I', 'R', 'IFF1', 'IFF2', 'AFX', 'BCX', 'DEX',
                         'HLX'))
        pages = a.mem_info().ram_bytes // 16384
        ram = ''.join(core.ipc(f'mem read 0 16384 --bank={page}')
                      for page in range(pages))
        return done.frame - before.frame, regs, ram, received


def test_subscribers_do_not_change_what_the_machine_computes() -> None:
    """Acceptance 7: the determinism check of slice 2 with two clients
    subscribed to everything. From the same reset, 100 frames end in the
    same registers and the same RAM with nobody listening and with two
    clients receiving an event for every frame."""
    frames = 100
    with Core() as core:
        alone = _stepped_by_kcpc(core, frames, 0)
        again = _stepped_by_kcpc(core, frames, 0)
        assert alone[:3] == again[:3], (
            'the reference run is not reproducible; the comparison below '
            'would prove nothing')
        assert alone[0] == frames, alone[0]
        watched = _stepped_by_kcpc(core, frames, 2)
        assert watched[0] == frames, watched[0]
        assert watched[3] == [frames, frames], (
            f'the subscribers received {watched[3]} frame events for '
            f'{frames} frames')
        assert watched[1] == alone[1], (
            f'registers differ:\n  alone   {alone[1]}\n  watched '
            f'{watched[1]}')
        assert watched[2] == alone[2], (
            f'RAM differs after the same {frames} frames')
        print(f'    2 subscribers, {frames} frame events each: registers '
              'and RAM identical')


# -- beads-pja3: another actor's pause lease is not a stop -----------------

DELAY_ADDR = 0x8000
DELAY_END = 0x8010


def delay_program(outer: int) -> bytes:
    """`outer` times 65536 turns of a counting loop (about half an emulated
    second each), then `JR $` at DELAY_END. Interrupts are expected off."""
    code = bytes([
        0x16, outer,        # 8000 LD D,outer
        0x01, 0x00, 0x00,   # 8002 LD BC,0
        0x0B,               # 8005 DEC BC
        0x78,               # 8006 LD A,B
        0xB1,               # 8007 OR C
        0x20, 0xFB,         # 8008 JR NZ,8005
        0x15,               # 800A DEC D
        0x20, 0xF5,         # 800B JR NZ,8002
        0x00, 0x00, 0x00,   # 800D NOP NOP NOP
        0x18, 0xFE,         # 8010 JR $
    ])
    assert len(code) == DELAY_END - DELAY_ADDR + 2
    return code


class KcpcWriter:
    """A KCPC controller that takes one pause lease after another: it writes
    memory in a loop on its own thread. `data` at `addr` must be what is
    there already, so the writes change nothing."""

    def __init__(self, c: KcpcClient, addr: int, data: bytes):
        self.c = c
        self.addr = addr
        self.data = data
        self.stop = threading.Event()
        self.writes = 0
        self.errors: List[str] = []
        self.thread = threading.Thread(target=self._run)

    def _run(self) -> None:
        try:
            while not self.stop.is_set():
                self.c.mem_write(kc.SPACE_CPU_RAM, self.addr, self.data)
                self.writes += 1
        except Exception as exc:  # noqa: BLE001 - reported by the test
            self.errors.append(f'{type(exc).__name__}: {exc}')

    def __enter__(self) -> 'KcpcWriter':
        self.thread.start()
        return self

    def __exit__(self, *exc) -> None:
        self.stop.set()
        self.thread.join()


def test_a_lease_does_not_end_a_text_ipc_walk() -> None:
    """beads-pja3, first half. The text IPC's `step to` runs the machine to
    an address about half an emulated second away. A KCPC client writing
    memory meanwhile pauses the machine under a lease for every write; the
    walk used to take the first of those for a stop and answer
    `stopped-elsewhere` with the machine still on its way."""
    with Core() as core, controller(core, 'writer') as a:
        pause_core(core, a)
        a.mem_write(kc.SPACE_CPU_RAM, DELAY_ADDR, delay_program(1))
        for turn in range(3):
            a.regs_set(pc=DELAY_ADDR, iff1=0, iff2=0)
            with KcpcWriter(a, 0x9000, bytes(64)) as writer:
                resp = core.ipc(f'step to {DELAY_END:#x}')
            assert not writer.errors, writer.errors
            assert resp.startswith('OK'), (
                f'turn {turn}, after {writer.writes} writes: {resp}')
            assert writer.writes >= 1, (
                'no lease during the walk: this proved nothing')
            regs = a.regs_get()
            assert regs.stamp.paused and regs.pc == DELAY_END, regs


def test_a_run_for_a_frame_step_is_not_lost_to_a_lease() -> None:
    """beads-pja3, second half. `wait vbl` from a paused machine arms the
    frame counter and runs the machine. With a bare resume that run was
    dropped whenever another actor held a pause lease at that instant, and
    the frames never came."""
    turns = 25
    with Core() as core, controller(core, 'writer') as a:
        spin(core, a)
        pause_core(core, a)
        ram = a.mem_read(kc.SPACE_CPU_RAM, 0, kc.CPU_SPACE_SIZE)
        with KcpcWriter(a, 0, ram) as writer:
            for turn in range(turns):
                resp = core.ipc('wait vbl 2 3000')
                assert resp.startswith('OK'), (
                    f'turn {turn}, after {writer.writes} writes: {resp}')
        assert not writer.errors, writer.errors
        assert writer.writes >= turns, (
            f'only {writer.writes} leases: this proved little')
        assert a.run_state().stamp.paused


# -- slice 5: breakpoints, pushed hits and the steps that walk --------------

CODE_ADDR = 0x8000


def load_program(c: KcpcClient, code: bytes, addr: int = CODE_ADDR) -> None:
    """Put `code` at `addr` and point the CPU at it with interrupts off. The
    machine must be paused."""
    c.mem_write(kc.SPACE_CPU_RAM, addr, code)
    c.regs_set(pc=addr, iff1=0, iff2=0)


def breakpoints_changed(c: KcpcClient) -> kc.Event:
    """The next EVT_BREAKPOINTS_CHANGED: a blocking read. Run-state events
    that come first are dropped."""
    return c.wait_event(kc.EVT_BREAKPOINTS_CHANGED, EVENT_TIMEOUT_S)


def keys(entries: List[kc.BpEntry]) -> List[Tuple[int, int, int]]:
    return sorted(entry.key for entry in entries)


def test_a_breakpoint_hit_is_pushed_to_both_clients() -> None:
    """Acceptance 1: the controller sets an execution breakpoint at a
    firmware address and runs. It and an observer both receive EVT_STOPPED,
    reason BREAKPOINT, with that pc, within 5 s and without asking: every
    wait is a blocking read. The stop describes the machine a read then
    finds, and the edit itself is pushed as EVT_BREAKPOINTS_CHANGED."""
    with Core() as core, controller(core, 'debugger') as a, \
            observer(core, 'watcher', kc.CLASS_RUN | kc.CLASS_DEBUG) as b:
        assert a.hello_reply.caps & kc.CAP_BREAKPOINTS, (
            f'caps {a.hello_reply.caps:#x}')
        a.subscribe(kc.CLASS_RUN)
        a_id = a.hello_reply.client_id
        a.pause()
        for who, c in (('controller', a), ('observer', b)):
            expect_stopped(c, kc.STOP_PAUSED, a_id, a.last_request_id, who)

        before, entries = b.bp_list()       # an observer may list
        assert entries == [], entries
        generation = a.bp_add(kc.BpEntry(kc.BP_EXEC, INTERRUPT_VECTOR))
        assert generation > before, (before, generation)
        changed = breakpoints_changed(b)
        assert changed.generation == generation, changed
        assert b.bp_list() == (generation, [
            kc.BpEntry(kc.BP_EXEC, INTERRUPT_VECTOR)])

        began = time.monotonic()
        a.run()
        for who, c in (('controller', a), ('observer', b)):
            expect_resumed(c, a_id, a.last_request_id, who)
            event = expect_stopped(c, kc.STOP_BREAKPOINT, 0, 0, who)
            assert (event.pc, event.addr, event.access) == (
                INTERRUPT_VECTOR, INTERRUPT_VECTOR, 0), f'{who}: {event}'
        took = time.monotonic() - began
        assert took < 5.0, f'the hit took {took:.1f}s to arrive'
        regs = b.regs_get()
        assert regs.stamp.paused and regs.pc == INTERRUPT_VECTOR, regs
        assert regs.stamp.frame == event.frame, (regs.stamp, event)
        hit = a.bp_list()[1][0]
        assert hit.hit_count >= 1, hit

        # An observer may not edit.
        refused(kc.NOT_CONTROLLER, 'BP_ADD by an observer', b.bp_add,
                kc.BpEntry(kc.BP_EXEC, 0x4000))
        refused(kc.NOT_CONTROLLER, 'BP_DEL by an observer', b.bp_del,
                kc.BP_EXEC, INTERRUPT_VECTOR)
        refused(kc.NOT_CONTROLLER, 'BP_CLEAR by an observer', b.bp_clear)

        gone = a.bp_del(kc.BP_EXEC, INTERRUPT_VECTOR)
        assert gone > generation
        assert breakpoints_changed(b).generation == gone
        refused(kc.NOT_FOUND, 'BP_DEL of what is not there', a.bp_del,
                kc.BP_EXEC, INTERRUPT_VECTOR)
        assert a.bp_list() == (gone, [])
        # With the breakpoint gone the machine runs on: the next stop anybody
        # hears of is the pause that ends the test.
        a.run()
        for who, c in (('controller', a), ('observer', b)):
            expect_resumed(c, a_id, a.last_request_id, who)
        wait_running(a, regs.stamp.frame + 5)
        a.pause()
        for who, c in (('controller', a), ('observer', b)):
            expect_stopped(c, kc.STOP_PAUSED, a_id, a.last_request_id, who)
        print(f'    pushed to both clients {took * 1000:.0f} ms after RUN')


def test_watch_and_io_hits_say_what_was_hit() -> None:
    """Acceptance 2: a write watchpoint reports WATCHPOINT with the address,
    the value written and the value it replaced; a read watchpoint the value
    read; an I/O breakpoint IO_BREAKPOINT with the port and the direction.
    The pc of each is the one REGS_GET then shows."""
    code = bytes([
        0x3E, 0x5A,         # 8000 LD A,&5A
        0x32, 0x00, 0x90,   # 8002 LD (&9000),A
        0x3A, 0x01, 0x90,   # 8005 LD A,(&9001)
        0x01, 0x10, 0x7F,   # 8008 LD BC,&7F10
        0xED, 0x49,         # 800B OUT (C),C   Gate Array: select the border
        0xED, 0x78,         # 800D IN A,(C)
        0x18, 0xFE,         # 800F JR $
    ])
    with Core() as core, controller(core, 'debugger') as a, \
            observer(core, 'watcher', kc.CLASS_RUN) as b:
        a.subscribe(kc.CLASS_RUN)
        a_id = a.hello_reply.client_id
        a.pause()
        for c in (a, b):
            assert run_event(c).reason == kc.STOP_PAUSED
        load_program(a, code)
        a.mem_write(kc.SPACE_CPU_RAM, 0x9000, b'\x11\x77')

        def hit(what: str) -> kc.Event:
            a.run()
            for who, c in (('controller', a), ('observer', b)):
                expect_resumed(c, a_id, a.last_request_id, f'{what}, {who}')
                event = run_event(c)
                assert event.opcode == kc.EVT_STOPPED, f'{what}, {who}: {event}'
                assert (event.cause_client, event.cause_request) == (0, 0), (
                    f'{what}, {who}: {event}')
            regs = a.regs_get()
            assert regs.stamp.paused and regs.pc == event.pc, (
                f'{what}: event {event}, registers {regs}')
            assert CODE_ADDR <= event.pc < CODE_ADDR + len(code), event
            return event

        a.bp_add(kc.BpEntry(kc.BP_WATCH, 0x9000, kc.ACCESS_WRITE, 1))
        event = hit('write watch')
        assert (event.reason, event.addr, event.access, event.value,
                event.old_value) == (kc.STOP_WATCHPOINT, 0x9000,
                                     kc.ACCESS_WRITE, 0x5A, 0x11), event

        a.bp_clear(kc.BP_WATCH)
        a.bp_add(kc.BpEntry(kc.BP_WATCH, 0x9001, kc.ACCESS_READ, 1))
        event = hit('read watch')
        assert (event.reason, event.addr, event.access, event.value,
                event.old_value) == (kc.STOP_WATCHPOINT, 0x9001,
                                     kc.ACCESS_READ, 0x77, 0), event

        a.bp_clear(kc.BP_WATCH)
        a.bp_add(kc.BpEntry(kc.BP_IO, 0x7F00, kc.ACCESS_WRITE, 0xFF00))
        event = hit('I/O write')
        assert (event.reason, event.addr, event.access, event.value) == (
            kc.STOP_IO_BREAKPOINT, 0x7F10, kc.ACCESS_WRITE, 0x10), event

        # The same port and mask again is the same entry: this replaces it.
        a.bp_add(kc.BpEntry(kc.BP_IO, 0x7F00, kc.ACCESS_READ, 0xFF00))
        assert a.bp_list()[1] == [
            kc.BpEntry(kc.BP_IO, 0x7F00, kc.ACCESS_READ, 0xFF00)]
        event = hit('I/O read')
        assert (event.reason, event.addr, event.access) == (
            kc.STOP_IO_BREAKPOINT, 0x7F10, kc.ACCESS_READ), event
        assert a.bp_list()[1][0].hit_count == 1

        # A condition on an I/O breakpoint is honoured: this one never
        # holds, so the OUT goes through and the machine reaches the JR $.
        a.bp_add(kc.BpEntry(kc.BP_IO, 0x7F00, kc.ACCESS_WRITE, 0xFF00,
                            condition='value = 0x99'))
        a.regs_set(pc=CODE_ADDR)
        a.run()
        wait_running(a, a.machine_info().stamp.frame + 3)
        a.pause()
        spin_at = CODE_ADDR + len(code) - 2
        # (A pause may land inside the JR, after its offset was fetched.)
        assert spin_at <= a.regs_get().pc <= spin_at + 2, a.regs_get()


def test_conditions_pass_counts_and_limits() -> None:
    """Acceptance 3: a conditional breakpoint and a pass count fire only
    when they should; a condition that does not parse is BAD_PAYLOAD and
    adds nothing; the 33rd execution breakpoint is LIMIT_REACHED (the probe
    arms 32, and one more would be listed and never fire), as are the 17th
    watchpoint and the 9th I/O breakpoint."""
    code = bytes([
        0x06, 0x00,     # 8000 LD B,0
        0x04,           # 8002 INC B
        0x18, 0xFD,     # 8003 JR &8002
    ])
    loop = CODE_ADDR + 2
    with Core() as core, controller(core, 'debugger') as a:
        a.pause()
        load_program(a, code)

        def run_to_stop() -> kc.Regs:
            a.run()
            event = expect_stopped(a, kc.STOP_BREAKPOINT, 0, 0, 'debugger')
            assert (event.pc, event.addr) == (loop, loop), event
            return a.regs_get()

        a.subscribe(kc.CLASS_RUN)
        a.bp_add(kc.BpEntry(kc.BP_EXEC, loop, condition='b = 5'))
        expect_run = run_event      # (the RUN's own EVT_RESUMED comes first)
        a.run()
        assert expect_run(a).opcode == kc.EVT_RESUMED
        event = expect_stopped(a, kc.STOP_BREAKPOINT, 0, 0, 'condition')
        regs = a.regs_get()
        assert event.pc == loop and regs.bc >> 8 == 5, (event, regs)

        # The same address again replaces the entry: a pass count now, no
        # condition, and its hits counted from nothing.
        a.bp_add(kc.BpEntry(kc.BP_EXEC, loop, pass_count=10))
        a.regs_set(pc=CODE_ADDR)
        a.run()
        assert expect_run(a).opcode == kc.EVT_RESUMED
        expect_stopped(a, kc.STOP_BREAKPOINT, 0, 0, 'pass count')
        regs = a.regs_get()
        assert regs.bc >> 8 == 9, f'stopped with B = {regs.bc >> 8}'
        generation, entries = a.bp_list()
        assert entries == [kc.BpEntry(kc.BP_EXEC, loop, pass_count=10,
                                      hit_count=10)], entries

        refused(kc.BAD_PAYLOAD, 'a condition that does not parse', a.bp_add,
                kc.BpEntry(kc.BP_EXEC, 0x4000, condition='b = = ('))
        assert a.bp_list() == (generation, entries), 'the refusal added it'
        for what, entry in (
                ('kind 3', kc.BpEntry(3, 0x4000)),
                ('access on an execution breakpoint',
                 kc.BpEntry(kc.BP_EXEC, 0x4000, kc.ACCESS_READ)),
                ('a watch with no access', kc.BpEntry(kc.BP_WATCH, 0x4000,
                                                      0, 1)),
                ('a watch of no bytes',
                 kc.BpEntry(kc.BP_WATCH, 0x4000, kc.ACCESS_WRITE, 0)),
                ('access 4', kc.BpEntry(kc.BP_IO, 0x7F00, 4, 0xFF00)),
                ('a pass count over 2^31',
                 kc.BpEntry(kc.BP_EXEC, 0x4000, pass_count=0x80000000))):
            refused(kc.BAD_PAYLOAD, what, a.bp_add, entry)
        refused(kc.OUT_OF_RANGE, 'a watch past &FFFF', a.bp_add,
                kc.BpEntry(kc.BP_WATCH, 0xFFFF, kc.ACCESS_WRITE, 2))
        refused(kc.BAD_PAYLOAD, 'BP_ADD cut short', a.request, kc.OP_BP_ADD,
                kc.BpEntry(kc.BP_EXEC, 0x4000).pack()[:-1])
        refused(kc.BAD_PAYLOAD, 'BP_DEL kind 7', a.bp_del, 7, 0x4000)
        refused(kc.BAD_PAYLOAD, 'BP_CLEAR kind 9', a.bp_clear, 9)
        refused(kc.BAD_PAYLOAD, 'BP_CLEAR with no payload', a.request,
                kc.OP_BP_CLEAR)
        assert a.bp_list() == (generation, entries)

        a.bp_clear()
        assert a.bp_list()[1] == []
        for n in range(kc.MAX_EXEC_BREAKPOINTS):
            a.bp_add(kc.BpEntry(kc.BP_EXEC, 0x4000 + n))
        refused(kc.LIMIT_REACHED, 'the 33rd execution breakpoint', a.bp_add,
                kc.BpEntry(kc.BP_EXEC, 0x5000))
        # Replacing one of the 32 needs no new comparator.
        a.bp_add(kc.BpEntry(kc.BP_EXEC, 0x4000, pass_count=2))
        # Nor is there a comparator for the one-shot breakpoint of a step
        # that runs to an address.
        refused(kc.LIMIT_REACHED, 'STEP over with the probe full', a.step, 1,
                kc.STEP_OVER)
        assert not a.run_state().walk_active
        for n in range(kc.MAX_WATCHPOINTS):
            a.bp_add(kc.BpEntry(kc.BP_WATCH, 0x9000 + 2 * n,
                                kc.ACCESS_BOTH, 2))
        refused(kc.LIMIT_REACHED, 'the 17th watchpoint', a.bp_add,
                kc.BpEntry(kc.BP_WATCH, 0xA000, kc.ACCESS_WRITE, 1))
        for n in range(kc.MAX_IO_BREAKPOINTS):
            a.bp_add(kc.BpEntry(kc.BP_IO, 0x1000 * n, kc.ACCESS_WRITE,
                                0xFF00))
        refused(kc.LIMIT_REACHED, 'the 9th I/O breakpoint', a.bp_add,
                kc.BpEntry(kc.BP_IO, 0xF800, kc.ACCESS_READ, 0xFF00))
        entries = a.bp_list()[1]
        assert len(entries) == (kc.MAX_EXEC_BREAKPOINTS + kc.MAX_WATCHPOINTS
                                + kc.MAX_IO_BREAKPOINTS), len(entries)
        assert len(set(keys(entries))) == len(entries)

        a.bp_clear(kc.BP_EXEC)
        kinds = {entry.kind for entry in a.bp_list()[1]}
        assert kinds == {kc.BP_WATCH, kc.BP_IO}, kinds
        a.bp_clear(kc.BP_ALL)
        assert a.bp_list()[1] == []
        # And with room again the step is taken.
        a.regs_set(pc=CODE_ADDR)
        step = a.step(1, kc.STEP_OVER)
        expect_stopped(a, kc.STOP_STEP_DONE, a.hello_reply.client_id, step,
                       'step over')


def test_text_ipc_edits_reach_the_binary_clients() -> None:
    """Acceptance 4: breakpoints set on the text IPC port are pushed as
    EVT_BREAKPOINTS_CHANGED and show up in BP_LIST; and a hit the binary
    clients were pushed is still there for the text IPC's `wait bp`."""
    with Core() as core, controller(core, 'ctl') as a, \
            observer(core, 'obs', kc.CLASS_RUN | kc.CLASS_DEBUG) as b:
        a.subscribe(kc.CLASS_RUN | kc.CLASS_DEBUG)
        generation = a.bp_list()[0]
        assert core.ipc('wp add 0x9000 2 w').startswith('OK')
        # (A port nothing answers on and the firmware never writes: the
        # machine is running, and this must not be what stops it.)
        assert core.ipc('iobp add 0xF900 0xFF00 out').startswith('OK')
        for c in (a, b):
            for _ in range(2):
                event = breakpoints_changed(c)
                assert event.generation > generation, event
                assert (event.cause_client, event.cause_request) == (0, 0)
        generation, entries = b.bp_list()
        assert entries == [
            kc.BpEntry(kc.BP_WATCH, 0x9000, kc.ACCESS_WRITE, 2),
            kc.BpEntry(kc.BP_IO, 0xF900, kc.ACCESS_WRITE, 0xFF00)], entries

        # The machine is running the firmware: the interrupt entry is hit
        # within a few milliseconds of the breakpoint being set.
        assert core.ipc(f'bp add {INTERRUPT_VECTOR:#x} if pc = 0x38'
                        ).startswith('OK')
        for who, c in (('controller', a), ('observer', b)):
            assert breakpoints_changed(c).generation > generation
        for who, c in (('controller', a), ('observer', b)):
            event = expect_stopped(c, kc.STOP_BREAKPOINT, 0, 0, who)
            assert event.pc == INTERRUPT_VECTOR, f'{who}: {event}'
        resp = core.ipc('wait bp 5000')
        assert resp.startswith('OK') and 'PC=0038' in resp, resp
        entries = a.bp_list()[1]
        assert kc.BpEntry(kc.BP_EXEC, INTERRUPT_VECTOR, hit_count=1,
                          condition='pc = 0x38') in entries, entries

        before = a.bp_list()[0]
        assert core.ipc('bp clear').startswith('OK')
        for c in (a, b):
            assert breakpoints_changed(c).generation > before
        assert keys(a.bp_list()[1]) == [(kc.BP_WATCH, 0x9000, 0),
                                       (kc.BP_IO, 0xF900, 0xFF00)]


# A routine to step over and out of, and a place to spin.
STEP_CODE = bytes([
    0xCD, 0x10, 0x80,   # 8000 CALL &8010
    0x3C,               # 8003 INC A
    0x00,               # 8004 NOP
    0x18, 0xFE,         # 8005 JR $
]) + bytes(9) + bytes([
    0x06, 0x80,         # 8010 LD B,&80
    0x10, 0xFE,         # 8012 DJNZ $
    0xC9,               # 8014 RET
])
STEP_SUB = CODE_ADDR + 0x10
STEP_SPIN = CODE_ADDR + 5
NOWHERE = 0x9999        # an address the code above never reaches


def test_step_over_out_and_to() -> None:
    """Acceptance 5, first half: STEP over a CALL lands on the instruction
    after it and STEP out returns to the caller, each with one EVT_STOPPED,
    reason STEP_DONE, naming the step. The one-shot breakpoint the walk runs
    to is nobody's business: an observer sees the machine resume and the
    step end, and nothing in between. The stepping client is told although
    it subscribed to nothing."""
    with Core() as core, controller(core, 'stepper') as a, \
            observer(core, 'watcher', kc.CLASS_RUN) as b:
        a_id = a.hello_reply.client_id
        pause_core(core, a)
        expect_stopped(b, kc.STOP_PAUSED, 0, 0, 'observer')
        load_program(a, STEP_CODE)

        def stepped(what: str, ran: bool, pc: int, **step) -> None:
            step_id = a.step(**step)
            assert not [e for e in a.events if e.opcode == kc.EVT_STOPPED], (
                f'{what}: the EVT_STOPPED arrived before the reply')
            if ran:
                expect_resumed(b, a_id, step_id, f'{what}, observer')
            for who, c in (('stepper', a), ('observer', b)):
                event = expect_stopped(c, kc.STOP_STEP_DONE, a_id, step_id,
                                       f'{what}, {who}')
                assert event.pc == pc, f'{what}, {who}: {event}'
            state = a.run_state()
            assert state.stamp.paused and not state.walk_active, (what, state)
            regs = a.regs_get()
            assert regs.pc == pc and regs.stamp.frame == event.frame, (
                what, regs, event)
            assert text_reg(core, 'pc') == pc, what

        stepped('over a CALL', True, CODE_ADDR + 3, mode=kc.STEP_OVER)
        assert a.regs_get().bc >> 8 == 0, 'the routine did not run to its end'
        # Not a call: the instruction is single-stepped and nothing runs.
        stepped('over INC A', False, CODE_ADDR + 4, mode=kc.STEP_OVER)

        a.regs_set(pc=CODE_ADDR)
        stepped('over three instructions', True, STEP_SPIN,
                mode=kc.STEP_OVER, count=3)

        a.regs_set(pc=CODE_ADDR)
        stepped('in', False, STEP_SUB, mode=kc.STEP_IN)
        # The routine calls nothing, so the walk single-steps it to its RET
        # and the machine never runs.
        stepped('out', False, CODE_ADDR + 3, mode=kc.STEP_OUT)

        a.regs_set(pc=CODE_ADDR)
        stepped('to an address', True, CODE_ADDR + 4, mode=kc.STEP_TO,
                addr=CODE_ADDR + 4)

        # From a running machine: paused first, quietly, then walked.
        a.regs_set(pc=CODE_ADDR, sp=0xBF00)
        a.mem_write(kc.SPACE_CPU_RAM, CODE_ADDR, b'\x18\xfe')   # JR $
        a.run()
        expect_resumed(b, a_id, a.last_request_id, 'observer')
        step_id = a.step(mode=kc.STEP_OVER)
        for who, c in (('stepper', a), ('observer', b)):
            event = expect_stopped(c, kc.STOP_STEP_DONE, a_id, step_id, who)
            assert event.pc == CODE_ADDR, f'{who}: {event}'

        # The text IPC's walks are one step to a client as well: its
        # `step over` of the CALL is a resume and a STEP_DONE, not the hit
        # of a breakpoint nobody set.
        a.mem_write(kc.SPACE_CPU_RAM, CODE_ADDR, STEP_CODE)
        a.regs_set(pc=CODE_ADDR)
        assert core.ipc('step over').startswith('OK')
        expect_resumed(b, 0, 0, 'text step over')
        event = expect_stopped(b, kc.STOP_STEP_DONE, 0, 0, 'text step over')
        assert event.pc == CODE_ADDR + 3, event
        assert a.bp_list()[1] == [], 'the one-shot breakpoint is listed'


def test_a_step_can_be_stopped_by_something_else() -> None:
    """Acceptance 5, second half: a step that meets a breakpoint reports
    BREAKPOINT with the step's request as the cause; a walk that cannot get
    anywhere ends with STEP_TIMEOUT (or STEP_STALLED) and the machine
    paused; a second STEP during a walk is BUSY; and PAUSE ends a walk, by
    whoever sends it, with the event still reaching the client that
    stepped."""
    with Core() as core, controller(core, 'stepper') as a, \
            observer(core, 'watcher', kc.CLASS_RUN) as b:
        a_id = a.hello_reply.client_id
        pause_core(core, a)
        expect_stopped(b, kc.STOP_PAUSED, 0, 0, 'observer')
        load_program(a, STEP_CODE)

        # A breakpoint inside the routine that is stepped over.
        inside = STEP_SUB + 2
        a.bp_add(kc.BpEntry(kc.BP_EXEC, inside))
        step_id = a.step(mode=kc.STEP_OVER)
        expect_resumed(b, a_id, step_id, 'observer')
        for who, c in (('stepper', a), ('observer', b)):
            event = expect_stopped(c, kc.STOP_BREAKPOINT, a_id, step_id, who)
            assert (event.pc, event.addr) == (inside, inside), event
        assert a.regs_get().pc == inside
        # And one on the way of a walk that single-steps: step out of the
        # routine walks DJNZ by DJNZ, and the probe is not what sees this.
        a.bp_clear()
        a.bp_add(kc.BpEntry(kc.BP_EXEC, STEP_SUB + 4))      # the RET
        step_id = a.step(mode=kc.STEP_OUT)
        for who, c in (('stepper', a), ('observer', b)):
            event = expect_stopped(c, kc.STOP_BREAKPOINT, a_id, step_id, who)
            assert (event.pc, event.addr) == (STEP_SUB + 4,
                                              STEP_SUB + 4), event
        a.bp_clear()

        # Nowhere to go: JR $.
        for mode in (kc.STEP_OUT, kc.STEP_TO):
            a.regs_set(pc=STEP_SPIN)
            began = time.monotonic()
            step_id = a.step(mode=mode, addr=NOWHERE, timeout_ms=300)
            event = run_event(a)
            took = time.monotonic() - began
            assert event.opcode == kc.EVT_STOPPED, event
            assert event.reason in (kc.STOP_STEP_TIMEOUT,
                                    kc.STOP_STEP_STALLED), event
            assert (event.cause_client, event.cause_request) == (
                a_id, step_id), event
            assert 0.25 < took < 3.0, f'a 300 ms deadline took {took:.2f}s'
            state = a.run_state()
            assert state.stamp.paused and not state.walk_active, state
            # (A deadline may pause the JR after its offset was fetched.)
            assert STEP_SPIN <= a.regs_get().pc <= STEP_SPIN + 2
            while True:     # the observer: a resume (STEP_TO ran it), a stop
                seen = run_event(b)
                if seen.opcode == kc.EVT_STOPPED:
                    assert seen.reason == event.reason, seen
                    break

        # PAUSE ends a walk.
        a.regs_set(pc=STEP_SPIN)
        step_id = a.step(mode=kc.STEP_TO, addr=NOWHERE, timeout_ms=60000)
        expect_resumed(b, a_id, step_id, 'observer')
        state = a.run_state()
        assert state.walk_active and not state.stamp.paused, state
        for mode in (kc.STEP_IN, kc.STEP_OVER, kc.STEP_OUT, kc.STEP_TO,
                     kc.STEP_FRAMES):
            refused(kc.BUSY, f'STEP mode {mode} during a walk', a.step, 1,
                    mode)
        stamp = a.pause()
        assert stamp.paused, stamp
        assert not [e for e in a.events if e.opcode == kc.EVT_STOPPED], (
            'the EVT_STOPPED arrived before the reply to PAUSE')
        for who, c in (('stepper', a), ('observer', b)):
            expect_stopped(c, kc.STOP_PAUSED, a_id, a.last_request_id, who)
        state = a.run_state()
        assert state.stamp.paused and not state.walk_active, state

        # The text IPC's pause ends one too, and names nobody.
        step_id = a.step(mode=kc.STEP_TO, addr=NOWHERE, timeout_ms=60000)
        expect_resumed(b, a_id, step_id, 'observer')
        assert core.ipc('pause').startswith('OK')
        for who, c in (('stepper', a), ('observer', b)):
            expect_stopped(c, kc.STOP_PAUSED, 0, 0, who)
        assert not a.run_state().walk_active

        # A walk survives a takeover, the new controller may end it, and the
        # client that stepped is told although it is an observer now and
        # subscribed to nothing.
        step_id = a.step(mode=kc.STEP_OUT, timeout_ms=60000)
        with core.client() as c:
            c.hello('usurper')
            assert c.acquire_control(force=True).is_controller
            assert c.run_state().walk_active
            c.pause()
            expect_stopped(a, kc.STOP_PAUSED, c.hello_reply.client_id,
                           c.last_request_id, 'the client that stepped')
        assert not a.run_state().walk_active


def test_quit_during_a_walk() -> None:
    """A walk only watches its own deadline, a minute away here; a core that
    quits ends it and exits at once all the same."""
    with Core() as core, controller(core, 'stepper') as a:
        pause_core(core, a)
        load_program(a, STEP_CODE)
        a.regs_set(pc=STEP_SPIN)
        a.step(mode=kc.STEP_TO, addr=NOWHERE, timeout_ms=60000)
        assert a.run_state().walk_active
        began = time.monotonic()
        core.ipc_quit()
        code = core.wait_exit()
        took = time.monotonic() - began
        assert code == 0, f'exit code {code}'
        assert took < 3.0, f'quit took {took:.1f}s with a walk in progress'
        print(f'    quit -> exit 0 in {took * 1000:.0f} ms during a walk')


def test_a_kcpc_walk_runs_beside_other_actors_leases() -> None:
    """beads-pja3 over this port: STEP over a routine that takes half an
    emulated second, while the text IPC writes memory in a loop and so
    pauses the machine under a lease again and again. The walk lands where
    it should every time, and an observer sees one resume and one stop for
    each step: the leases are nobody's event."""
    code = bytes([
        0xCD, 0x10, 0x80,   # 8000 CALL &8010
        0x18, 0xFE,         # 8003 JR $
    ]) + bytes(11) + bytes([
        0x01, 0x00, 0x00,   # 8010 LD BC,0
        0x0B,               # 8013 DEC BC
        0x78,               # 8014 LD A,B
        0xB1,               # 8015 OR C
        0x20, 0xFB,         # 8016 JR NZ,&8013
        0xC9,               # 8018 RET
    ])
    with Core() as core, controller(core, 'stepper') as a, \
            observer(core, 'watcher', kc.CLASS_RUN) as b:
        a_id = a.hello_reply.client_id
        a.pause()
        expect_stopped(b, kc.STOP_PAUSED, a_id, a.last_request_id, 'observer')
        load_program(a, code)
        with LeaseTaker(core) as other:
            for turn in range(3):
                a.regs_set(pc=CODE_ADDR)
                writes = other.writes
                step_id = a.step(mode=kc.STEP_OVER)
                expect_resumed(b, a_id, step_id, f'turn {turn}')
                for who, c in (('stepper', a), ('observer', b)):
                    event = expect_stopped(c, kc.STOP_STEP_DONE, a_id,
                                           step_id, f'turn {turn}, {who}')
                    assert event.pc == CODE_ADDR + 3, f'turn {turn}: {event}'
                # (One is enough to end a walk that mistakes it for a stop;
                # a slow host fits few text IPC connections into the walk.)
                assert other.writes - writes >= 1, (
                    f'turn {turn}: no lease during the walk: this proved '
                    'nothing')
        assert not other.errors, other.errors
        assert a.regs_get().pc == CODE_ADDR + 3


def test_step_over_a_restart_uses_a_prepared_rom() -> None:
    """Acceptance 6: an RST is stepped into and its handler finished, since
    a CPC restart need not come back to the byte after it. The handler has
    to be in ROM, where the restart vectors are, so this machine boots a
    ROM made for the test (make_test_rom in ipc_harness.py)."""
    sys.path.insert(0, str(ROOT / 'test' / 'integrated'))
    try:
        from ipc_harness import make_test_rom  # pylint: disable=import-outside-toplevel
    finally:
        sys.path.pop(0)
    # RST &30: load A and come back.
    romdir = make_test_rom({0x0030: bytes([0x3E, 0x42, 0xC9])})
    try:
        with Core('-O', f'rom.rom_path={romdir}', '-O',
                  'system.model=2') as core, \
                controller(core, 'stepper') as a:
            a_id = a.hello_reply.client_id
            a.pause()
            rom = a.mem_read(kc.SPACE_ROM_LOWER, 0x30, 3)
            assert rom == bytes([0x3E, 0x42, 0xC9]), (
                f'not the prepared ROM: {rom.hex()}')
            assert a.mem_info().lower_rom_on, 'the lower ROM is paged out'
            a.mem_write(kc.SPACE_CPU_RAM, 0x6000,
                        bytes([0xF7, 0x00, 0x18, 0xFE]))   # RST &30:NOP:JR $
            a.regs_set(pc=0x6000, sp=0xBF00, af=0, iff1=0, iff2=0)
            step_id = a.step(mode=kc.STEP_OVER)
            event = expect_stopped(a, kc.STOP_STEP_DONE, a_id, step_id,
                                   'step over RST')
            assert event.pc == 0x6001, event
            regs = a.regs_get()
            assert regs.pc == 0x6001 and regs.af >> 8 == 0x42, regs
            assert regs.sp == 0xBF00, f'sp {regs.sp:#06x}'
    finally:
        shutil.rmtree(romdir, ignore_errors=True)


# -- slice 6: input ----------------------------------------------------------

SCREEN_ADDR = 0xC000    # the screen after a reset, before anything scrolls
SCREEN_SIZE = 0x4000
FONT_ADDR = 0x3800      # the firmware's character matrices, in the lower ROM
TEXT_ROWS = 25
TEXT_COLS = 40          # mode 1, which BASIC starts in
CURSOR = '\x7f'         # how screen_text() shows the cursor block
BOOT_TIMEOUT_S = 60.0


def screen_text(c: KcpcClient) -> List[str]:
    """The mode 1 screen as 25 lines of text: every character cell compared
    with the character matrices of the firmware ROM. Pen 1 on paper 0, which
    is what BASIC prints in; the cursor is a solid cell and reads as CURSOR,
    anything else unknown as '?'. Trailing spaces are cut."""
    font = c.mem_read(kc.SPACE_ROM_LOWER, FONT_ADDR, 0x800)
    glyphs = {font[code * 8:code * 8 + 8]: chr(code)
              for code in range(126, 31, -1)}
    glyphs[b'\xff' * 8] = CURSOR
    screen = c.mem_read(kc.SPACE_CPU_RAM, SCREEN_ADDR, SCREEN_SIZE)
    lines = []
    for row in range(TEXT_ROWS):
        line = ''
        for col in range(TEXT_COLS):
            cell = bytearray()
            for scan in range(8):
                # A mode 1 byte is four pixels; pen 1 is its high nibble.
                at = scan * 0x800 + row * 80 + col * 2
                cell.append((screen[at] & 0xF0) | (screen[at + 1] >> 4))
            line += glyphs.get(bytes(cell), '?')
        lines.append(line.rstrip())
    return lines


def wait_for_screen(c: KcpcClient, what: str,
                    found: Callable[[List[str]], Optional[int]]) -> int:
    """Reads the screen until `found` returns a row. The firmware takes
    emulated time to print; there is no event for 'BASIC has printed'."""
    deadline = time.monotonic() + BOOT_TIMEOUT_S
    while True:
        lines = screen_text(c)
        row = found(lines)
        if row is not None:
            return row
        assert time.monotonic() < deadline, (
            f'{what}: the screen shows\n' + '\n'.join(lines[:14]))
        time.sleep(0.05)


def wait_for_ready(c: KcpcClient) -> int:
    """Waits for BASIC's prompt and returns the row the cursor is on."""
    def found(lines: List[str]) -> Optional[int]:
        for row in range(1, TEXT_ROWS):
            if lines[row] == CURSOR and lines[row - 1] == 'Ready':
                return row
        return None
    return wait_for_screen(c, 'no Ready prompt', found)


def run_frames(c: KcpcClient, frames: int) -> None:
    """Runs exactly `frames` frames and returns when they have ended: a
    blocking read of the step's EVT_STOPPED."""
    request = c.step(count=frames, mode=kc.STEP_FRAMES)
    while True:
        event = c.next_event(EVENT_TIMEOUT_S)
        if (event.opcode == kc.EVT_STOPPED
                and event.cause_request == request):
            assert event.reason == kc.STOP_STEP_DONE, event
            return


def held(rows: bytes) -> List[Tuple[int, int]]:
    """The (row, bit) of every key that is down in an INPUT_STATE reply."""
    return [(row, bit) for row in range(kc.KEY_ROWS) for bit in range(8)
            if not rows[row] & (1 << bit)]


# Long enough for the firmware to see a key change: it scans the keyboard
# once a frame and debounces over two scans.
KEY_FRAMES = 6


def test_a_key_reaches_the_matrix_and_the_firmware() -> None:
    """Acceptance 1: KEY down for the matrix position of A shows in
    INPUT_STATE (for an observer too) and in the text IPC's `input state`,
    and after frames have run the firmware has typed the character. SHIFT is
    a key like any other, held by the client itself."""
    with Core() as core, controller(core, 'typist') as c, \
            observer(core, 'watcher') as o:
        assert c.hello_reply.caps & kc.CAP_INPUT, hex(c.hello_reply.caps)
        row = wait_for_ready(c)
        c.subscribe(kc.CLASS_RUN)
        c.pause()
        assert held(o.input_state()) == []

        # The matrix is host state: a key lands in it on a paused machine.
        c.key(*kc.KEY_A)
        assert held(c.input_state()) == [kc.KEY_A]
        rows = o.input_state()
        assert rows[kc.KEY_A[0]] == 0xDF and held(rows) == [kc.KEY_A], rows
        resp = core.ipc(f'input state {kc.KEY_A[0]}')
        assert resp == f'OK row{kc.KEY_A[0]}=0xDF held=A', resp
        assert core.ipc('input state') == 'OK held=A'

        run_frames(c, KEY_FRAMES)
        c.key(*kc.KEY_A, down=False)
        assert held(o.input_state()) == []
        assert core.ipc('input state') == 'OK held=(none)'
        run_frames(c, KEY_FRAMES)
        line = screen_text(c)[row]
        assert line == 'a' + CURSOR, repr(line)

        # SHIFT and then A: pressing and releasing A leaves SHIFT down.
        c.key(*kc.KEY_SHIFT)
        c.key(*kc.KEY_A)
        assert held(o.input_state()) == [kc.KEY_SHIFT, kc.KEY_A]
        run_frames(c, KEY_FRAMES)
        c.key(*kc.KEY_A, down=False)
        assert held(o.input_state()) == [kc.KEY_SHIFT]
        run_frames(c, KEY_FRAMES)
        c.key(*kc.KEY_SHIFT, down=False)
        run_frames(c, KEY_FRAMES)
        line = screen_text(c)[row]
        assert line == 'aA' + CURSOR, repr(line)


SCAN_RESULT = CODE_ADDR + 0x100     # row 9 as the program last read it
SCAN_RIGHT = SCAN_RESULT + 1        # scans that found "right" down
SCAN_DOWN = SCAN_RESULT + 2         # scans that found "down" down

# Reads keyboard row 9 (joystick 0) the way software does: PSG register 14
# through the PPI, the row on port C. Each pass selects row 9, reads it and
# selects row 0 again, which is the cycle an AMX mouse counts its pulses by.
ROW9_SCAN = bytes([
    0xF3,                   # 8000 DI
    0x01, 0x0E, 0xF4,       # 8001 LD BC,&F40E   PSG register 14 ...
    0xED, 0x49,             # 8004 OUT (C),C
    0x01, 0xC0, 0xF6,       # 8006 LD BC,&F6C0   ... is latched
    0xED, 0x49,             # 8009 OUT (C),C
    0x01, 0x00, 0xF6,       # 800B LD BC,&F600   PSG inactive
    0xED, 0x49,             # 800E OUT (C),C
    0x01, 0x92, 0xF7,       # 8010 LD BC,&F792   PPI port A is an input
    0xED, 0x49,             # 8013 OUT (C),C
    0x01, 0x49, 0xF6,       # 8015 LD BC,&F649   row 9, PSG read
    0xED, 0x49,             # 8018 OUT (C),C
    0x06, 0xF4,             # 801A LD B,&F4
    0xED, 0x78,             # 801C IN A,(C)      the row
    0x01, 0x82, 0xF7,       # 801E LD BC,&F782   port A is an output again
    0xED, 0x49,             # 8021 OUT (C),C
    0x01, 0x00, 0xF6,       # 8023 LD BC,&F600   row 0, PSG inactive
    0xED, 0x49,             # 8026 OUT (C),C
    0x32, SCAN_RESULT & 0xFF, SCAN_RESULT >> 8,     # 8028 LD (result),A
    0xCB, 0x5F,             # 802B BIT 3,A       right
    0x20, 0x04,             # 802D JR NZ,&8033
    0x21, SCAN_RIGHT & 0xFF, SCAN_RIGHT >> 8,       # 802F LD HL,right
    0x34,                   # 8032 INC (HL)
    0xCB, 0x4F,             # 8033 BIT 1,A       down
    0x20, 0x04,             # 8035 JR NZ,&803B
    0x21, SCAN_DOWN & 0xFF, SCAN_DOWN >> 8,         # 8037 LD HL,down
    0x34,                   # 803A INC (HL)
    0x18, 0xC4,             # 803B JR &8001
])


def scan_bytes(c: KcpcClient) -> bytes:
    return c.mem_read(kc.SPACE_CPU_RAM, SCAN_RESULT, 3)


def start_row9_scan(c: KcpcClient) -> None:
    """Leaves the machine paused in ROW9_SCAN with its three bytes zeroed."""
    c.subscribe(kc.CLASS_RUN)
    c.pause()
    c.mem_write(kc.SPACE_CPU_RAM, SCAN_RESULT, bytes(3))
    load_program(c, ROW9_SCAN)


def scan_until(c: KcpcClient, what: str,
               done: Callable[[bytes], bool]) -> bytes:
    """Runs frames until the scan program's three bytes satisfy `done`."""
    deadline = time.monotonic() + IO_TIMEOUT_S
    while True:
        run_frames(c, 2)
        data = scan_bytes(c)
        if done(data):
            return data
        assert time.monotonic() < deadline, f'{what}: {data.hex()}'


def test_joystick_keys_reach_a_program_reading_the_ppi() -> None:
    """Acceptance 2: joystick 0 is row 9 of the matrix. A Z80 program that
    reads the row through the PPI sees the directions KEY presses, with the
    next frame's keyboard snapshot."""
    with Core() as core, controller(core, 'joystick') as c:
        wait_for_ready(c)
        start_row9_scan(c)
        run_frames(c, 2)
        assert scan_bytes(c)[0] == 0xFF, 'row 9 with nothing pressed'

        for what, key, down, expected in (
                ('up', kc.JOY0_UP, True, 0xFE),
                ('up and right', kc.JOY0_RIGHT, True, 0xF6),
                ('right', kc.JOY0_UP, False, 0xF7),
                ('right and fire 1', kc.JOY0_FIRE1, True, 0xD7),
                ('fire 1', kc.JOY0_RIGHT, False, 0xDF)):
            c.key(*key, down=down)
            run_frames(c, 2)
            got = scan_bytes(c)[0]
            assert got == expected, f'{what}: the program read {got:#04x}'

        # A key that is not on row 9 does not show there.
        c.key(*kc.KEY_A)
        c.keys_release()
        run_frames(c, 2)
        assert scan_bytes(c)[0] == 0xFF, 'KEYS_RELEASE left a key down'
        assert held(c.input_state()) == []


def wait_control_free(c: KcpcClient) -> kc.Event:
    """The next EVT_CONTROL_CHANGED that says nobody has control."""
    while True:
        event = c.wait_event(kc.EVT_CONTROL_CHANGED, EVENT_TIMEOUT_S)
        if event.controller == 0:
            return event


def test_held_keys_are_released_when_control_is_lost() -> None:
    """Acceptance 3: a controller that disconnects with keys held leaves
    none of them down, and INPUT_STATE from an observer shows it by the
    time the observer is told control is free. The same when control is
    taken by force or given up. A key another source holds is left alone;
    KEYS_RELEASE is what lets go of every key."""
    with Core() as core, observer(core, 'watcher') as o:
        assert core.ipc('input keydown SHIFT').startswith('OK')
        a = controller(core, 'holder')
        a.key(*kc.KEY_A)
        a.key(*kc.JOY0_UP)
        assert held(o.input_state()) == [kc.KEY_SHIFT, kc.KEY_A, kc.JOY0_UP]

        a.close()
        wait_control_free(o)
        assert held(o.input_state()) == [kc.KEY_SHIFT], (
            'a disconnected controller left keys down, or the text '
            "IPC's SHIFT was released with them")

        # Forced takeover: the demoted controller's keys are released, and
        # it can press nothing more.
        with controller(core, 'second holder') as b:
            b.key(*kc.KEY_A)
            assert o.acquire_control(force=True).is_controller
            assert held(o.input_state()) == [kc.KEY_SHIFT]
            refused(kc.NOT_CONTROLLER, 'KEY from the demoted controller',
                    b.key, *kc.KEY_A)

            # Giving control up.
            o.key(*kc.JOY0_FIRE1)
            assert held(b.input_state()) == [kc.KEY_SHIFT, kc.JOY0_FIRE1]
            assert not o.release_control().is_controller
            assert held(b.input_state()) == [kc.KEY_SHIFT]

            # KEYS_RELEASE lets go of every key, the text IPC's too.
            assert b.acquire_control().is_controller
            b.key(*kc.KEY_A)
            b.keys_release()
            assert held(o.input_state()) == []
            assert core.ipc('input state') == 'OK held=(none)'


def test_text_is_typed_by_the_autotype_queue() -> None:
    """Acceptance 4: TEXT 'print 1+1~RETURN~' makes BASIC print 2, and the
    reply says how many key actions are queued. Text the queue cannot parse
    is refused and types nothing."""
    with Core() as core, controller(core, 'typist') as c:
        row = wait_for_ready(c)
        c.subscribe(kc.CLASS_RUN)
        c.pause()   # so that nothing is typed between the request and reply
        try:
            c.text('print 1~NOSUCHKEY~')
            raise AssertionError('an unknown ~KEY~ token was accepted')
        except KcpcError as e:
            assert e.status == kc.BAD_PAYLOAD, e
            assert 'NOSUCHKEY' in e.message, e.message
        assert c.text('') == 0, 'the refused text was queued'

        # Nine characters and one token.
        assert c.text('print 1+1~RETURN~') == 10
        assert core.ipc('autotype status').startswith('OK active: 10'), (
            core.ipc('autotype status'))
        c.run()

        def printed(lines: List[str]) -> Optional[int]:
            return row if lines[row + 2] == 'Ready' else None
        wait_for_screen(c, 'BASIC did not answer', printed)
        lines = screen_text(c)
        assert lines[row:row + 4] == ['print 1+1', ' 2', 'Ready', CURSOR], (
            lines[row:row + 4])
        assert c.text('') == 0, 'the queue did not drain'


def test_mouse_needs_a_mouse_and_moves_the_amx_counters() -> None:
    """Acceptance 5: MOUSE answers NOT_PRESENT when no mouse is enabled.
    With the AMX mouse enabled, a movement arrives as that many pulses on
    joystick row 9, counted by a program that scans the row, and a button
    as a fire line; a button still held when control is lost is let go."""
    with Core() as core, controller(core, 'no mouse') as c:
        refused(kc.NOT_PRESENT, 'MOUSE with no mouse', c.mouse, 5, 5)
        refused(kc.NOT_PRESENT, 'MOUSE buttons with no mouse', c.mouse, 0, 0,
                kc.MOUSE_LEFT)

    with Core('-O', 'input.amx_mouse=1') as core, \
            controller(core, 'mouse') as c, observer(core, 'watcher') as o:
        wait_for_ready(c)
        start_row9_scan(c)
        run_frames(c, 4)
        assert scan_bytes(c) == b'\xff\x00\x00', scan_bytes(c).hex()

        # The movement is handed to the mouse by the main loop, once a frame
        # of its own, so the test runs frames until it has arrived. One
        # pulse a scan while there is movement left: the program's count is
        # the distance, give or take the pulse the first scan may eat.
        c.mouse(10, 7)
        _, right, down = scan_until(
            c, 'MOUSE 10, 7',
            lambda d: d[0] == 0xFF and d[1] >= 9 and d[2] >= 6)
        run_frames(c, 4)
        assert scan_bytes(c) == bytes([0xFF, right, down]), 'still counting'
        assert right <= 10, f'{right} pulses right for dx=10'
        assert down <= 7, f'{down} pulses down for dy=7'

        # Left and up are the other two lines, which the program does not
        # count. The left button is fire 2 of the joystick port.
        c.mouse(-5, -5)
        c.mouse(0, 0, kc.MOUSE_LEFT)
        scan_until(c, 'the left button', lambda d: d[0] == 0xEF)
        assert scan_bytes(c)[1:] == bytes([right, down])

        # A movement without the buttons flag leaves the button down.
        c.mouse(4, 0)
        scan_until(c, 'MOUSE 4, 0',
                   lambda d: d[0] == 0xEF and d[1] >= right + 3)
        assert scan_bytes(c)[1] <= right + 4

        # Lost control: the button goes up, as a key would.
        c.run()
        c.close()
        wait_control_free(o)
        deadline = time.monotonic() + IO_TIMEOUT_S
        while scan_bytes(o)[0] != 0xFF:
            assert time.monotonic() < deadline, (
                'the mouse button stayed down after its controller left')
            time.sleep(0.02)


def test_input_needs_control_and_is_checked() -> None:
    """Acceptance 6: an observer's KEY, KEYS_RELEASE, TEXT and MOUSE answer
    NOT_CONTROLLER and change nothing, malformed or not; INPUT_STATE is
    anyone's. A controller's malformed requests are refused by what is
    wrong with them."""
    with Core() as core, controller(core, 'controller') as c, \
            observer(core, 'observer') as o:
        refused(kc.NOT_CONTROLLER, 'KEY', o.key, *kc.KEY_A)
        refused(kc.NOT_CONTROLLER, 'KEYS_RELEASE', o.keys_release)
        refused(kc.NOT_CONTROLLER, 'TEXT', o.text, 'print 1')
        refused(kc.NOT_CONTROLLER, 'MOUSE', o.mouse, 1, 1)
        for opcode in (kc.OP_KEY, kc.OP_TEXT, kc.OP_MOUSE):
            refused(kc.NOT_CONTROLLER, f'short {opcode:#06x}', o.request,
                    opcode, b'\x01')
        assert held(o.input_state()) == []
        assert core.ipc('autotype status') == 'OK idle', (
            core.ipc('autotype status'))

        refused(kc.BAD_PAYLOAD, 'KEY cut short', c.request, kc.OP_KEY,
                b'\x08\x05\x01')
        refused(kc.BAD_PAYLOAD, 'KEY down=2', c.request, kc.OP_KEY,
                kc.key_payload(8, 5, 2))
        refused(kc.OUT_OF_RANGE, 'KEY row 10', c.key, 10, 0)
        refused(kc.OUT_OF_RANGE, 'KEY bit 8', c.key, 0, 8)
        refused(kc.BAD_PAYLOAD, 'TEXT cut short', c.request, kc.OP_TEXT,
                b'\x05\x00ab')
        refused(kc.BAD_PAYLOAD, 'MOUSE cut short', c.request, kc.OP_MOUSE,
                b'\x01\x00\x01\x00\x00')
        assert held(c.input_state()) == []
        # The last key of the matrix exists, and bytes after the fields a
        # request has are ignored.
        c.request(kc.OP_KEY, kc.key_payload(9, 7, 1) + b'later')
        assert held(o.input_state()) == [(9, 7)]
        c.request(kc.OP_KEYS_RELEASE, b'later')
        assert held(o.input_state()) == []


# -- slice 7: what the DevTools show ----------------------------------------

def text_fields(core: Core, command: str) -> Dict[str, int]:
    """The NAME=HEX fields of a text IPC chip dump."""
    resp = core.ipc(command)
    assert resp.startswith('OK '), f'{command}: {resp[:80]}'
    return {name: int(value, 16)
            for name, value in (field.split('=') for field in resp[3:].split())}


def chips_match_the_text_ipc(core: Core, c: KcpcClient, where: str) -> None:
    """CHIP_GET of the CRTC, the Gate Array and the PSG against `regs crtc`,
    `regs ga` and `regs psg`, field for field. The machine must be paused."""
    crtc = c.chip_get(kc.CHIP_CRTC)
    ga = c.chip_get(kc.CHIP_GATE_ARRAY)
    psg = c.chip_get(kc.CHIP_PSG)
    for chip in (crtc, ga, psg):
        assert chip.stamp.paused and chip.stamp == crtc.stamp, (where, chip)

    text = text_fields(core, 'regs crtc')
    assert [text[f'R{i}'] for i in range(18)] == list(crtc.reg), (where, text)
    assert (text['VCC'], text['VLC'], text['HCC'], text['HSC'], text['VSC'],
            text['VMA'], text['SL']) == (
        crtc.vcc, crtc.ra, crtc.hcc, crtc.hsw, crtc.vsw, crtc.ma,
        crtc.scanline & 0xFF), (where, text, crtc)
    # R52 is the Gate Array's line counter, printed with the CRTC's.
    assert text['R52'] == ga.sl_count, (where, text, ga)
    resp = core.ipc('config get crtc_type')
    assert resp == f'OK {crtc.type}', (where, resp, crtc)

    text = text_fields(core, 'regs ga')
    assert [text[f'INK{i}'] for i in range(17)] == list(ga.ink), (where, text)
    # The text IPC prints MODE in decimal; a mode is one digit either way.
    assert (text['MODE'], text['PEN'], text['ROM_CFG'], text['RAM_CFG'],
            text['SL'], text['INT_DELAY']) == (
        ga.mode, ga.pen, ga.rom_config, ga.ram_config, ga.sl_count,
        ga.hs_count), (where, text, ga)
    info = c.mem_info()
    assert (ga.rom_config, ga.ram_config, ga.upper_rom) == (
        info.rom_config, info.ram_config, info.upper_rom), (where, ga, info)

    text = text_fields(core, 'regs psg')
    assert [text[f'R{i}'] for i in range(16)] == list(psg.reg), (where, text)
    assert text['SELECT'] == psg.reg_select, (where, text, psg)


# Sets a CRTC register, a pen, the border and two PSG registers to values
# the firmware does not leave there, then spins.
CHIP_CODE = bytes([
    0x01, 0x0C, 0xBC,   # LD BC,&BC0C
    0xED, 0x49,         # OUT (C),C        CRTC: select R12
    0x01, 0x21, 0xBD,   # LD BC,&BD21
    0xED, 0x49,         # OUT (C),C        CRTC: R12 = &21
    0x01, 0x03, 0x7F,   # LD BC,&7F03
    0xED, 0x49,         # OUT (C),C        Gate Array: select pen 3
    0x01, 0x4B, 0x7F,   # LD BC,&7F4B
    0xED, 0x49,         # OUT (C),C        Gate Array: ink &0B
    0x18, 0xFE,         # JR $
])
CHIP_SPIN = CODE_ADDR + len(CHIP_CODE) - 2


def run_to(c: KcpcClient, addr: int) -> kc.Event:
    """Run the paused machine to `addr` and return the stop. The client must
    be the controller."""
    c.subscribe(kc.CLASS_RUN)
    c.step(mode=kc.STEP_TO, addr=addr, timeout_ms=5000)
    while True:
        event = run_event(c)
        if event.opcode == kc.EVT_STOPPED:
            break
    assert event.reason == kc.STOP_STEP_DONE and event.pc == addr, event
    return event


def test_chip_get_matches_the_text_ipc() -> None:
    """Acceptance 1: CHIP_GET of the CRTC, the Gate Array and the PSG equals
    the text IPC's `regs crtc|ga|psg`, field for field, at the same paused
    instant: between two frames, in the middle of a frame (where the beam
    counters are not zero), and after a program has written the chips."""
    with Core() as core, controller(core, 'chips') as c:
        assert c.hello_reply.caps & kc.CAP_CHIPS, hex(c.hello_reply.caps)
        wait_for_ready(c)
        pause_core(core, c)
        chips_match_the_text_ipc(core, c, 'between frames')

        # In the middle of a frame: the interrupt entry. (Reached with a
        # step of this port, not with the text IPC's `bp add`, `run` and
        # `wait bp`: that `wait bp` now and then reports a hit from before
        # the `run` while the machine is running again,
        # beads-1ew8.)
        run_to(c, INTERRUPT_VECTOR)
        crtc = c.chip_get(kc.CHIP_CRTC)
        assert crtc.scanline != 0 and crtc.reg[0] == 63, crtc
        chips_match_the_text_ipc(core, c, 'at the interrupt')

        load_program(c, CHIP_CODE)
        run_to(c, CHIP_SPIN)
        crtc = c.chip_get(kc.CHIP_CRTC)
        ga = c.chip_get(kc.CHIP_GATE_ARRAY)
        assert crtc.reg_select == 12 and crtc.reg[12] == 0x21, crtc
        assert ga.pen == 3 and ga.ink[3] == 0x0B, ga
        chips_match_the_text_ipc(core, c, 'after the program')

        # The PSG's register file is what the firmware left: the mixer has
        # every channel off, which is not the reset value.
        psg = c.chip_get(kc.CHIP_PSG)
        assert psg.reg[7] != 0, psg

        # Reading changes nothing and needs no control.
        with observer(core, 'chip observer') as b:
            assert b.chip_get(kc.CHIP_CRTC) == crtc
        assert c.machine_info().stamp.paused

        # On a running machine the reads are taken at a frame boundary.
        spin(core, c)
        frames = set()
        for _ in range(20):
            chip = c.chip_get(kc.CHIP_CRTC)
            assert not chip.stamp.paused, chip.stamp
            frames.add(chip.stamp.frame)
        assert len(frames) > 1, 'the machine did not run under the reads'

        # A chip that does not exist, and a request cut short.
        for what, payload in (('chip 0', b'\x00\x00\x00\x00'),
                              ('chip 6', b'\x06\x00\x00\x00'),
                              ('short', b'')):
            refused(kc.BAD_PAYLOAD, what, c.request, kc.OP_CHIP_GET, payload)
        c.ping()
        # None of these reads cost the machine a pause lease.
        served = core_reads(core)
        assert served['fallback'] == 0 and served['busy'] == 0, served
        assert served['boundary'] >= 20, served


def core_reads(core: Core) -> Dict[str, int]:
    """Quit the core and say where the gate took its reads."""
    core.ipc_quit()
    core.wait_exit()
    return core.reads_served()


def pending(c: KcpcClient, *opcodes: int) -> List[kc.Event]:
    """The events of these kinds that `c` has received and not yet taken."""
    return [event for event in map(kc.Event.decode, c.events)
            if event.opcode in opcodes]


def test_fdc_chip_and_media_events() -> None:
    """Acceptance 4, first half: the text IPC's `load game.dsk` pushes
    EVT_MEDIA_CHANGED for slot 0 with a new generation, and CHIP_GET FDC
    shows the disc; ejecting it, and loading and ejecting a tape, are pushed
    too; a file that is no tape changes nothing. A client that did not
    subscribe to MEDIA is sent none of it."""
    with Core() as core, observer(core, 'media', kc.CLASS_MEDIA) as c, \
            observer(core, 'deaf', kc.CLASS_RUN) as deaf:
        tmp = Path(os.path.realpath(core.tmpdir))
        disk = tmp / 'game.dsk'
        resp = core.ipc(f'disk new {disk} data sector')
        assert resp.startswith('OK'), resp

        before = c.chip_get(kc.CHIP_FDC)
        assert before.present == (0, 0) and before.motor == 0, before

        def media_event(slot: int, present: bool) -> kc.Event:
            """The next EVT_MEDIA_CHANGED for `slot` that says `present`."""
            while True:
                event = c.wait_event(kc.EVT_MEDIA_CHANGED, EVENT_TIMEOUT_S)
                if event.slot == slot and event.present == present:
                    return event

        assert core.ipc(f'load {disk}').startswith('OK')
        event = media_event(kc.SLOT_DRIVE_A, True)
        assert event.generation > before.generation[0], (event, before)
        assert Path(event.path) == disk, event
        # The machine takes the disc at its next frame boundary, and says so
        # with one more event; from then on the FDC has it.
        deadline = time.monotonic() + IO_TIMEOUT_S
        while True:
            fdc = c.chip_get(kc.CHIP_FDC)
            if fdc.present[0]:
                break
            assert time.monotonic() < deadline, f'the FDC has no disc: {fdc}'
            time.sleep(0.02)
        assert fdc.present == (1, 0), fdc
        assert fdc.generation[0] >= event.generation, (fdc, event)
        assert fdc.generation[1] == before.generation[1], (fdc, before)

        loaded = fdc.generation[0]
        assert core.ipc('disk eject A').startswith('OK')
        while True:
            event = media_event(kc.SLOT_DRIVE_A, False)
            if event.generation > loaded:
                break
        assert event.path == '', event
        fdc = c.chip_get(kc.CHIP_FDC)
        assert fdc.present == (0, 0), fdc
        assert fdc.generation[0] >= event.generation, (fdc, event)

        # A tape: a CDT with one pause block.
        tape = tmp / 'tape.cdt'
        tape.write_bytes(b'ZXTape!\x1a\x01\x0d' + b'\x20\xe8\x03')
        assert core.ipc(f'load {tape}').startswith('OK')
        event = media_event(kc.SLOT_TAPE, True)
        assert Path(event.path) == tape and event.generation > 0, event
        inserted = event.generation
        assert core.ipc('tape eject').startswith('OK')
        event = media_event(kc.SLOT_TAPE, False)
        assert event.generation > inserted and event.path == '', event

        # A file that is no tape changes nothing: the deck keeps its tape,
        # and no event says otherwise (beads-ow6r).
        assert core.ipc(f'load {tape}').startswith('OK')
        event = media_event(kc.SLOT_TAPE, True)
        inserted = event.generation
        junk = tmp / 'junk.cdt'
        junk.write_bytes(b'not a tape at all')
        assert core.ipc(f'load {junk}').startswith('ERR')
        c.ping()
        assert not pending(c, kc.EVT_MEDIA_CHANGED), c.events
        info = c.media_info(kc.SLOT_TAPE)
        assert info.present and Path(info.path) == tape, info
        assert info.generation == inserted, (info, inserted)
        assert info.tape.blocks == 1, info

        # Nothing of this reached the client that asked for RUN events only.
        deaf.ping()
        assert not pending(deaf, kc.EVT_MEDIA_CHANGED), deaf.events


# -- slice 9: media, snapshots, configuration -------------------------------

def new_disc(core: Core, path: Path) -> None:
    resp = core.ipc(f'disk new {path} data sector')
    assert resp.startswith('OK'), resp


def media_events(c: KcpcClient, slot: int, upto: int) -> List[kc.Event]:
    """The EVT_MEDIA_CHANGED events of `slot`, up to the one that carries
    generation `upto`: blocking reads, no polling."""
    seen = []
    while True:
        event = c.wait_event(kc.EVT_MEDIA_CHANGED, EVENT_TIMEOUT_S)
        if event.slot != slot:
            continue
        seen.append(event)
        if event.generation >= upto:
            return seen


def test_a_disc_runs_and_a_snapshot_round_trips() -> None:
    """Acceptance 1: the client loads a DSK, runs a program from it, saves an
    SNA, changes the machine, loads the SNA and reads byte-identical RAM.
    The machine has the disc when MEDIA_INSERT is answered; the events reach
    an observer, and the controller after its reply; an observer may ask
    MEDIA_INFO and may change nothing."""
    with Core() as core, controller(core, 'media') as c, \
            observer(core, 'watcher',
                     kc.CLASS_MEDIA | kc.CLASS_RUN) as watcher:
        me = c.hello_reply.client_id
        c.subscribe(kc.CLASS_MEDIA | kc.CLASS_RUN)
        tmp = Path(os.path.realpath(core.tmpdir))
        disc = tmp / 'game.dsk'
        new_disc(core, disc)
        # A program on the disc, put there through the text IPC's disc
        # tools, which need the disc in a drive: the eject writes it back.
        source = tmp / 'prog.bas'
        source.write_bytes(b'10 PRINT "KCPC":POKE &8000,123\r\n')
        assert core.ipc(f'load {disc}').startswith('OK')
        resp = core.ipc(f'disk put A {source} PROG.BAS ascii')
        assert resp.startswith('OK'), resp
        assert core.ipc('disk eject A').startswith('OK')
        wait_for_ready(c)
        c.ping()
        c.events.clear()
        watcher.ping()
        watcher.events.clear()

        empty = c.media_info(kc.SLOT_DRIVE_A)
        assert not empty.present and empty.path == '' and empty.flags == 0, (
            empty)
        generation = c.media_insert(kc.SLOT_DRIVE_A, str(disc))
        request = c.last_request_id
        assert generation > empty.generation, (generation, empty)
        # No event of the insert overtook its reply...
        assert not pending(c, kc.EVT_MEDIA_CHANGED), c.events
        # ...and the machine has the disc already: no waiting for a frame.
        fdc = c.chip_get(kc.CHIP_FDC)
        assert fdc.present == (1, 0) and fdc.generation[0] == generation, fdc
        for who in (c, watcher):
            events = media_events(who, kc.SLOT_DRIVE_A, generation)
            assert events[-1].generation == generation, events
            assert events[-1].present, events
            assert Path(events[-1].path) == disc, events
        info = watcher.media_info(kc.SLOT_DRIVE_A)
        assert (info.slot, info.present, info.flags, info.generation) == (
            kc.SLOT_DRIVE_A, True, 0, generation), info
        assert Path(info.path) == disc and info.tape is None, info

        # The program on the disc runs.
        assert c.text('run"prog~RETURN~') > 0

        def printed(lines: List[str]) -> Optional[int]:
            for row in range(1, TEXT_ROWS - 1):
                if lines[row] == 'KCPC' and lines[row + 1] == 'Ready':
                    return row
            return None
        wait_for_screen(c, 'the program on the disc did not run', printed)
        assert c.mem_read(kc.SPACE_CPU_RAM, 0x8000, 1) == b'\x7b'

        # Save, damage, load: the RAM is the same, byte for byte.
        c.pause()
        expect_stopped(c, kc.STOP_PAUSED, me, c.last_request_id, 'controller')
        size = c.mem_info().ram_bytes
        ram = c.mem_read(kc.SPACE_PHYS_RAM, 0, size)
        regs = c.regs_get()
        sna = tmp / 'state.sna'
        stamp = c.snapshot_save(str(sna))
        assert stamp.paused, stamp
        assert sna.read_bytes()[:8] == b'MV - SNA', 'not an SNA file'
        assert sna.stat().st_size == 0x100 + size, sna.stat().st_size
        c.mem_write(kc.SPACE_PHYS_RAM, 0, bytes(0x10000))
        c.regs_set(hl=regs.hl ^ 0xFFFF, pc=0x4000)
        assert c.mem_read(kc.SPACE_PHYS_RAM, 0, size) != ram
        c.ping()
        c.events.clear()
        watcher.ping()
        watcher.events.clear()

        stamp = c.snapshot_load(str(sna))
        request = c.last_request_id
        assert stamp.paused, f'a paused machine stays paused: {stamp}'
        assert not pending(c, kc.EVT_RESET), 'the event overtook the reply'
        for who in (c, watcher):
            event = run_event(who)
            assert event.opcode == kc.EVT_RESET, event
            assert (event.cause_client, event.cause_request) == (
                me, request), event
        assert c.mem_read(kc.SPACE_PHYS_RAM, 0, size) == ram, (
            'the RAM after SNAPSHOT_LOAD is not the RAM that was saved')
        after = c.regs_get()
        assert after.values() == regs.values(), (regs, after)
        # The disc is still in the drive: a snapshot is not a medium.
        assert c.media_info(kc.SLOT_DRIVE_A).generation == generation

        # A running machine runs on after a load; with the flag it stops.
        c.run()
        expect_resumed(c, me, c.last_request_id, 'controller')
        stamp = c.snapshot_load(str(sna))
        request = c.last_request_id
        event = run_event(c)
        assert event.opcode == kc.EVT_RESET and event.cause_request == (
            request), event
        wait_running(c)
        stamp = c.snapshot_load(str(sna), stay_paused=True)
        request = c.last_request_id
        assert stamp.paused, stamp
        event = run_event(c)
        assert event.opcode == kc.EVT_RESET and event.cause_request == (
            request), event
        expect_stopped(c, kc.STOP_PAUSED, me, request, 'controller')
        assert c.mem_read(kc.SPACE_PHYS_RAM, 0, size) == ram

        # An observer changes nothing, whatever it sends.
        other = tmp / 'other.sna'
        for what, call, args in (
                ('MEDIA_INSERT', watcher.media_insert,
                 (kc.SLOT_DRIVE_B, str(disc))),
                ('MEDIA_EJECT', watcher.media_eject, (kc.SLOT_DRIVE_A,)),
                ('TAPE_CONTROL', watcher.tape_control, (kc.TAPE_PLAY,)),
                ('SNAPSHOT_SAVE', watcher.snapshot_save, (str(other),)),
                ('SNAPSHOT_LOAD', watcher.snapshot_load, (str(sna),)),
                ('CONFIG_SET', watcher.config_set, ('model', '3')),
                ('CONFIG_APPLY', watcher.config_apply, ())):
            refused(kc.NOT_CONTROLLER, f'{what} from an observer', call,
                    *args)
        for opcode in (kc.OP_MEDIA_INSERT, kc.OP_MEDIA_EJECT,
                       kc.OP_TAPE_CONTROL, kc.OP_SNAPSHOT_SAVE,
                       kc.OP_SNAPSHOT_LOAD, kc.OP_CONFIG_SET):
            refused(kc.NOT_CONTROLLER, f'a malformed {opcode:#06x}',
                    watcher.request, opcode, b'')
        assert not other.exists(), 'an observer wrote a file'
        assert watcher.media_info(kc.SLOT_DRIVE_A).generation == generation
        assert watcher.media_info(kc.SLOT_DRIVE_B).present is False
        assert watcher.config_get('model').pending is None


def test_media_requests_are_checked() -> None:
    """What does not exist is NOT_FOUND; what does not fit the slot, is no
    image, or leads out through '..' is BAD_PAYLOAD, and changes nothing: a
    drive keeps the disc it had; a cartridge needs a 6128+. A relative path
    is the core's."""
    with Core() as core, controller(core, 'checks') as c:
        tmp = Path(os.path.realpath(core.tmpdir))
        disc = tmp / 'good.dsk'
        new_disc(core, disc)
        tape = tmp / 'tape.cdt'
        tape.write_bytes(b'ZXTape!\x1a\x01\x0d' + b'\x20\xe8\x03')
        junk = tmp / 'junk.dsk'
        junk.write_bytes(b'this is not a disc image at all' * 40)
        flux = tmp / 'dump.hfe'
        flux.write_bytes(b'HXCPICFE' + bytes(1024))

        refused(kc.NOT_FOUND, 'a file that is not there', c.media_insert,
                kc.SLOT_DRIVE_A, str(tmp / 'nowhere.dsk'))
        refused(kc.NOT_FOUND, 'a directory', c.media_insert,
                kc.SLOT_DRIVE_A, str(tmp))
        refused(kc.BAD_PAYLOAD, 'a tape in a drive', c.media_insert,
                kc.SLOT_DRIVE_A, str(tape))
        refused(kc.BAD_PAYLOAD, 'a disc in the deck', c.media_insert,
                kc.SLOT_TAPE, str(disc))
        refused(kc.BAD_PAYLOAD, 'flux in drive B', c.media_insert,
                kc.SLOT_DRIVE_B, str(flux))
        refused(kc.BAD_PAYLOAD, 'a slot that does not exist',
                c.media_insert, 4, str(disc))
        refused(kc.BAD_PAYLOAD, 'a reserved flag', c.media_insert,
                kc.SLOT_DRIVE_A, str(disc), 1)
        refused(kc.BAD_PAYLOAD, 'no path', c.media_insert,
                kc.SLOT_DRIVE_A, '')
        refused(kc.BAD_PAYLOAD, "a path through '..'", c.media_insert,
                kc.SLOT_DRIVE_A, '../../etc/good.dsk')
        refused(kc.BAD_PAYLOAD, 'a short MEDIA_INSERT', c.request,
                kc.OP_MEDIA_INSERT, b'\x00')
        refused(kc.BAD_PAYLOAD, 'a short MEDIA_EJECT', c.request,
                kc.OP_MEDIA_EJECT, b'')
        refused(kc.BAD_PAYLOAD, 'MEDIA_INFO of slot 7', c.media_info, 7)
        refused(kc.BAD_PAYLOAD, 'MEDIA_EJECT of slot 7', c.media_eject, 7)
        refused(kc.NOT_PRESENT, 'a cartridge in a 6128', c.media_insert,
                kc.SLOT_CARTRIDGE, str(ROOT / 'rom' / 'system.cpr'))
        refused(kc.BAD_PAYLOAD, 'ejecting the cartridge', c.media_eject,
                kc.SLOT_CARTRIDGE)
        for slot in (kc.SLOT_DRIVE_A, kc.SLOT_DRIVE_B, kc.SLOT_TAPE):
            assert not c.media_info(slot).present, slot
        assert c.chip_get(kc.CHIP_FDC).present == (0, 0)

        # A file that is refused changes nothing: the drive keeps its disc,
        # the host and the machine alike, and no event says otherwise. The
        # text IPC's `load` does the same (ipc_harness.py,
        # test_disk_failed_load_does_not_redirect_write_back).
        loaded = c.media_insert(kc.SLOT_DRIVE_B, str(disc))
        assert c.chip_get(kc.CHIP_FDC).present == (0, 1)
        refused(kc.BAD_PAYLOAD, 'a file that is no disc', c.media_insert,
                kc.SLOT_DRIVE_B, str(junk))
        refused(kc.NOT_FOUND, 'a disc that is not there', c.media_insert,
                kc.SLOT_DRIVE_B, str(tmp / 'nowhere.dsk'))
        refused(kc.BAD_PAYLOAD, 'flux with nothing in it', c.media_insert,
                kc.SLOT_DRIVE_A, str(flux))
        info = c.media_info(kc.SLOT_DRIVE_B)
        assert info.present and Path(info.path) == disc, info
        assert info.generation == loaded, (info, loaded)
        assert c.chip_get(kc.CHIP_FDC).present == (0, 1)
        assert not c.media_info(kc.SLOT_DRIVE_A).present
        assert c.media_eject(kc.SLOT_DRIVE_B) > info.generation
        info = c.media_info(kc.SLOT_DRIVE_B)

        # The deck is the same: a file that is no tape leaves the tape there
        # (beads-ow6r).
        cut = tmp / 'cut.cdt'
        cut.write_bytes(b'ZXTape!\x1a\x01\x0d' + b'\x10\xe8')
        voc = tmp / 'junk.voc'
        voc.write_bytes(b'Creative but no voice in it')
        loaded = c.media_insert(kc.SLOT_TAPE, str(tape))
        refused(kc.BAD_PAYLOAD, 'a CDT cut short', c.media_insert,
                kc.SLOT_TAPE, str(cut))
        refused(kc.BAD_PAYLOAD, 'a VOC with no voice', c.media_insert,
                kc.SLOT_TAPE, str(voc))
        refused(kc.NOT_FOUND, 'a tape that is not there', c.media_insert,
                kc.SLOT_TAPE, str(tmp / 'nowhere.cdt'))
        deck = c.media_info(kc.SLOT_TAPE)
        assert deck.present and Path(deck.path) == tape, deck
        assert deck.generation == loaded, (deck, loaded)
        assert deck.tape.blocks == 1, deck
        assert c.media_eject(kc.SLOT_TAPE) > loaded

        # Ejecting an empty slot succeeds.
        assert c.media_eject(kc.SLOT_DRIVE_B) > info.generation
        assert c.media_eject(kc.SLOT_TAPE) >= 0

        # A relative path is resolved in the core's working directory and
        # reported absolute.
        relative = 'test_tapes/valid-minimal.cdt'
        c.media_insert(kc.SLOT_TAPE, relative)
        info = c.media_info(kc.SLOT_TAPE)
        assert info.present and Path(info.path) == ROOT / relative, info

        # Snapshots are checked the same way.
        refused(kc.NOT_FOUND, 'a snapshot that is not there',
                c.snapshot_load, str(tmp / 'nowhere.sna'))
        refused(kc.BAD_PAYLOAD, 'a file that is no SNA', c.snapshot_load,
                str(junk))
        refused(kc.NOT_FOUND, 'a directory that is not there',
                c.snapshot_save, str(tmp / 'nowhere' / 'state.sna'))
        refused(kc.BAD_PAYLOAD, 'a directory as the file', c.snapshot_save,
                str(tmp))
        refused(kc.BAD_PAYLOAD, "a snapshot through '..'", c.snapshot_save,
                '../state.sna')
        refused(kc.BAD_PAYLOAD, 'no path', c.snapshot_save, '')
        refused(kc.BAD_PAYLOAD, 'no path', c.snapshot_load, '')
        # None of the refusals stopped or reset the machine.
        wait_running(c)


def test_unsaved_writes_go_back_to_their_own_disc() -> None:
    """A disc that was written to is saved to its own file when another disc
    takes its place, and when it is ejected; the new disc's file is not
    touched. MEDIA_INFO says when a disc has unsaved writes."""
    with Core() as core, controller(core, 'writes') as c:
        tmp = Path(os.path.realpath(core.tmpdir))
        first, second = tmp / 'first.dsk', tmp / 'second.dsk'
        new_disc(core, first)
        new_disc(core, second)
        blank = first.read_bytes()
        assert second.read_bytes() == blank
        payload = tmp / 'hello.bin'
        payload.write_bytes(b'KCPC' * 64)

        c.media_insert(kc.SLOT_DRIVE_A, str(first))
        assert not c.media_info(kc.SLOT_DRIVE_A).unsaved
        resp = core.ipc(f'disk put A {payload} HELLO.BIN')
        assert resp.startswith('OK'), resp
        assert c.media_info(kc.SLOT_DRIVE_A).unsaved
        assert first.read_bytes() == blank, 'written back too early'

        c.media_insert(kc.SLOT_DRIVE_A, str(second))
        written = first.read_bytes()
        assert written != blank, 'the write to the first disc was lost'
        assert second.read_bytes() == blank, (
            "the first disc's writes went into the second disc's file")
        info = c.media_info(kc.SLOT_DRIVE_A)
        assert Path(info.path) == second and not info.unsaved, info
        assert 'HELLO.BIN' not in core.ipc('disk ls A')

        # The same on eject.
        assert core.ipc(f'disk put A {payload} BYE.BIN').startswith('OK')
        assert c.media_info(kc.SLOT_DRIVE_A).unsaved
        c.media_eject(kc.SLOT_DRIVE_A)
        assert second.read_bytes() != blank, 'the eject lost the write'
        assert first.read_bytes() == written
        info = c.media_info(kc.SLOT_DRIVE_A)
        assert not info.present and info.flags == 0, info

        # And it is the first disc that has the first file.
        c.media_insert(kc.SLOT_DRIVE_B, str(first))
        assert 'HELLO.BIN' in core.ipc('disk ls B')


def test_the_tape_deck_is_driven_and_reported() -> None:
    """TAPE_CONTROL plays, stops, rewinds and seeks, and answers with the
    deck as the text IPC's `tape status` shows it; without a tape it is
    NOT_PRESENT. A tape may come out of a .zip."""
    with Core() as core, controller(core, 'deck') as c, \
            observer(core, 'watcher', kc.CLASS_MEDIA) as watcher:
        tmp = Path(os.path.realpath(core.tmpdir))
        refused(kc.NOT_PRESENT, 'PLAY with no tape', c.tape_control,
                kc.TAPE_PLAY)
        info = c.media_info(kc.SLOT_TAPE)
        assert not info.present and info.tape == kc.TapeState(
            False, False, 0, 0), info

        # Three pause blocks.
        tape = tmp / 'tape.cdt'
        tape.write_bytes(b'ZXTape!\x1a\x01\x0d' + b'\x20\xe8\x03' * 3)
        generation = c.media_insert(kc.SLOT_TAPE, str(tape))
        events = media_events(watcher, kc.SLOT_TAPE, generation)
        assert events[-1].present and Path(events[-1].path) == tape, events
        info = watcher.media_info(kc.SLOT_TAPE)
        assert info.present and Path(info.path) == tape, info
        assert info.tape == kc.TapeState(False, False, 0, 3), info

        def text_status() -> Dict[str, str]:
            resp = core.ipc('tape status')
            assert resp.startswith('OK '), resp
            return dict(field.split('=', 1) for field in resp[3:].split())

        deck = c.tape_control(kc.TAPE_PLAY)
        assert deck.playing and deck.blocks == 3, deck
        status = text_status()
        assert status['play'] == '1' and status['attached'] == '1', status
        deck = c.tape_control(kc.TAPE_SEEK, 2)
        assert deck.block == 2 and deck.playing, deck
        assert text_status()['block'] == '2'
        assert watcher.media_info(kc.SLOT_TAPE).tape.block == 2
        refused(kc.OUT_OF_RANGE, 'a block past the end', c.tape_control,
                kc.TAPE_SEEK, 3)
        deck = c.tape_control(kc.TAPE_STOP)
        assert not deck.playing and deck.block == 2, deck
        assert text_status()['play'] == '0'
        deck = c.tape_control(kc.TAPE_REWIND)
        assert deck.block == 0, deck
        assert text_status()['block'] == '0'
        refused(kc.BAD_PAYLOAD, 'an action that does not exist',
                c.tape_control, 4)
        refused(kc.BAD_PAYLOAD, 'a short TAPE_CONTROL', c.request,
                kc.OP_TAPE_CONTROL, b'\x01')

        ejected = c.media_eject(kc.SLOT_TAPE)
        assert ejected > generation
        info = c.media_info(kc.SLOT_TAPE)
        assert not info.present and info.tape.blocks == 0, info
        assert text_status()['attached'] == '0'
        refused(kc.NOT_PRESENT, 'SEEK with no tape', c.tape_control,
                kc.TAPE_SEEK, 0)

        # The first member of an archive that the slot takes.
        archive = tmp / 'tapes.zip'
        with zipfile.ZipFile(archive, 'w') as z:
            z.writestr('readme.txt', 'not a tape')
            z.writestr('side-a.cdt', tape.read_bytes())
        c.media_insert(kc.SLOT_TAPE, str(archive))
        info = c.media_info(kc.SLOT_TAPE)
        assert info.present and Path(info.path) == archive, info
        assert info.tape.blocks == 3, info
        refused(kc.BAD_PAYLOAD, 'an archive with no disc in it',
                c.media_insert, kc.SLOT_DRIVE_A, str(archive))


def text_config(core: Core, key: str) -> str:
    resp = core.ipc(f'config get {key}')
    assert resp.startswith('OK'), (key, resp)
    return resp[3:]


def test_configuration_is_read_set_and_applied() -> None:
    """Acceptance 2: CONFIG_SET model 3 stages it, CONFIG_APPLY rebuilds the
    machine and EVT_MACHINE_CHANGED is pushed, to the controller after its
    reply. The keys and values are the text IPC's. On the rebuilt 6128+ a
    cartridge can be inserted, which resets the machine."""
    with Core() as core, controller(core, 'settings') as c, \
            observer(core, 'watcher',
                     kc.CLASS_MACHINE | kc.CLASS_MEDIA) as watcher:
        me = c.hello_reply.client_id
        c.subscribe(kc.CLASS_MACHINE | kc.CLASS_RUN | kc.CLASS_MEDIA)
        for key in ('model', 'crtc_type', 'crtc_info', 'ram_size',
                    'silicon_disc', 'm4board', 'm4_rom_slot', 'file'):
            got = watcher.config_get(key)
            assert got.value == text_config(core, key) and (
                got.pending is None), (key, got)
        assert watcher.config_get('model').value == '2'
        refused(kc.NOT_FOUND, 'a key that does not exist', c.config_get,
                'no_such_key')
        refused(kc.NOT_FOUND, 'setting a key that does not exist',
                c.config_set, 'no_such_key', '1')
        refused(kc.BAD_PAYLOAD, 'a model that does not exist', c.config_set,
                'model', '9')
        refused(kc.BAD_PAYLOAD, 'a model that is no number', c.config_set,
                'model', 'plus')
        refused(kc.BAD_PAYLOAD, 'a RAM size that does not exist',
                c.config_set, 'ram_size', '100')
        refused(kc.BAD_PAYLOAD, 'a short CONFIG_SET', c.request,
                kc.OP_CONFIG_SET, kc.pack_str('model'))
        # A window is the GUI's.
        if MODE == 'gui':
            assert c.config_get('fullscreen').value in ('0', '1')
            assert c.config_get('window').value == text_config(core,
                                                                'window')
        else:
            refused(kc.NOT_PRESENT, 'fullscreen without a window',
                    c.config_get, 'fullscreen')
            refused(kc.NOT_PRESENT, 'a window without a window',
                    c.config_set, 'fullscreen', '1')

        # Applied at once: the machine has the other CRTC.
        assert c.config_set('crtc_type', '1') == kc.CONFIG_NOW
        assert c.config_get('crtc_type').value == '1'
        assert c.machine_info().crtc_type == 1
        assert c.chip_get(kc.CHIP_CRTC).type == 1
        assert text_config(core, 'crtc_type') == '1'

        # Staged: nothing is rebuilt, and both servers say what is waiting.
        assert c.config_set('model', '3') == kc.CONFIG_STAGED
        got = watcher.config_get('model')
        assert (got.value, got.pending) == ('2', '3'), got
        assert text_config(core, 'model') == '2 pending=3'
        assert c.machine_info().model == 2
        c.ping()
        assert not pending(c, kc.EVT_MACHINE_CHANGED), c.events

        stamp = c.config_apply()
        request = c.last_request_id
        assert not stamp.paused, f'a running machine runs on: {stamp}'
        assert not pending(c, kc.EVT_MACHINE_CHANGED), (
            'the event overtook the reply')
        for who in (c, watcher):
            event = who.wait_event(kc.EVT_MACHINE_CHANGED, EVENT_TIMEOUT_S)
            assert event.machine.model == 3 and event.machine.has_asic, event
        info = c.machine_info()
        assert info.model == 3 and info.has_asic, info
        got = c.config_get('model')
        assert (got.value, got.pending) == ('3', None), got
        assert text_config(core, 'model') == '3'
        assert request > 0 and me > 0
        wait_running(c)

        # RAM size is staged too.
        assert c.config_set('ram_size', '512') == kc.CONFIG_STAGED
        c.config_apply()
        event = watcher.wait_event(kc.EVT_MACHINE_CHANGED, EVENT_TIMEOUT_S)
        assert event.machine.ram_bytes == 512 * 1024, event
        assert c.mem_info().ram_bytes == 512 * 1024
        c.wait_event(kc.EVT_MACHINE_CHANGED, EVENT_TIMEOUT_S)

        # A 6128+ takes a cartridge, and starts it from a reset.
        cartridge = ROOT / 'rom' / 'system.cpr'
        before = c.media_info(kc.SLOT_CARTRIDGE)
        assert before.present, before
        c.events.clear()
        generation = c.media_insert(kc.SLOT_CARTRIDGE, str(cartridge))
        request = c.last_request_id
        assert generation > before.generation, (generation, before)
        seen = {kc.EVT_RESET: None, kc.EVT_MEDIA_CHANGED: None}
        while None in seen.values():
            event = c.next_event(EVENT_TIMEOUT_S)
            if event.opcode == kc.EVT_MEDIA_CHANGED and (
                    event.slot != kc.SLOT_CARTRIDGE
                    or event.generation < generation):
                continue
            if event.opcode in seen:
                seen[event.opcode] = event
        assert (seen[kc.EVT_RESET].cause_client,
                seen[kc.EVT_RESET].cause_request) == (me, request), seen
        assert Path(seen[kc.EVT_MEDIA_CHANGED].path) == cartridge, seen
        info = c.media_info(kc.SLOT_CARTRIDGE)
        assert info.present and Path(info.path) == cartridge, info
        # The reset left it running, as it was.
        wait_running(c, beyond=c.machine_info().stamp.frame + 5)

        # A file that is no cartridge leaves the one that was there, and a
        # machine that still runs.
        refused(kc.BAD_PAYLOAD, 'a file that is no cartridge',
                c.media_insert, kc.SLOT_CARTRIDGE,
                str(ROOT / 'test' / 'cartridge' / 'invalid_noriff.cpr'))
        info = c.media_info(kc.SLOT_CARTRIDGE)
        assert info.present and Path(info.path) == cartridge, info
        frame = wait_running(c).frame
        wait_running(c, beyond=frame + 5)
        # It executes the cartridge that was put back, not freed memory: the
        # lower ROM the CPU sees is the cartridge's first page.
        page = cartridge.read_bytes()
        first = page.index(b'cb00') + 8
        assert c.mem_read(kc.SPACE_ROM_LOWER, 0, 0x4000) == (
            page[first:first + 0x4000]), 'the lower ROM is not the cartridge'


# The ASIC's unlock sequence, as the Plus firmware sends it to the CRTC's
# register-select port.
ASIC_UNLOCK = bytes([0xFF, 0x00, 0xFF, 0x77, 0xB3, 0x51, 0xA8, 0xD4, 0x62,
                     0x39, 0x9C, 0x46, 0x2B, 0x15, 0x8A, 0xCD, 0xEE])
ASIC_SEQ = CODE_ADDR + 0x40
ASIC_CODE = bytes([
    0x21, ASIC_SEQ & 0xFF, ASIC_SEQ >> 8,   # LD HL,sequence
    0x1E, len(ASIC_UNLOCK),                 # LD E,17
    0x01, 0x00, 0xBC,                       # LD BC,&BC00
    0x7E,                                   # loop: LD A,(HL)
    0xED, 0x79,                             # OUT (C),A
    0x23,                                   # INC HL
    0x1D,                                   # DEC E
    0x20, 0xF9,                             # JR NZ,loop
    0x00,                                   # unlocked: NOP
    0x01, 0xB8, 0x7F,                       # LD BC,&7FB8
    0xED, 0x49,                             # OUT (C),C   RMR2: page in
    0x21, 0x5A, 0x0C,                       # LD HL,&0C5A
    0x22, 0x02, 0x64,                       # LD (&6402),HL   pen 1 = &C5A
    0x21, 0x21, 0x03,                       # LD HL,&0321
    0x22, 0x20, 0x64,                       # LD (&6420),HL   border = &321
    0x3E, 0x37,                             # LD A,&37
    0x32, 0x00, 0x68,                       # LD (&6800),A    PRI line &37
    0x01, 0xA0, 0x7F,                       # LD BC,&7FA0
    0xED, 0x49,                             # OUT (C),C   RMR2: page out
    0x18, 0xFE,                             # JR $
])
ASIC_UNLOCKED = CODE_ADDR + 15
ASIC_SPIN = CODE_ADDR + len(ASIC_CODE) - 2


def test_asic_state_and_register_page() -> None:
    """Acceptance 2: on a 6128+, after the unlock sequence, CHIP_GET ASIC
    reports the ASIC unlocked and the memory space ASIC_PAGE returns 16384
    bytes whose palette bytes are what the text IPC's `asic palette` prints.
    On a 6128 both answer NOT_PRESENT."""
    with Core('-O', 'system.model=3') as core, \
            controller(core, 'asic') as c:
        assert c.hello_reply.machine.has_asic, c.hello_reply.machine
        # The Plus boots through its cartridge; give it the second it takes.
        run_frames_plain(core, c, 100)
        pause_core(core, c)
        assert len(ASIC_CODE) <= 0x40
        c.mem_write(kc.SPACE_CPU_RAM, ASIC_SEQ, ASIC_UNLOCK)
        load_program(c, ASIC_CODE)
        run_to(c, ASIC_UNLOCKED)
        asic = c.chip_get(kc.CHIP_ASIC)
        assert asic.unlocked and asic.stamp.paused, asic
        assert not asic.page_on, asic

        run_to(c, ASIC_SPIN)
        asic = c.chip_get(kc.CHIP_ASIC)
        assert asic.unlocked and asic.pri_line == 0x37, asic
        assert not asic.page_on, 'the program paged the registers out'
        assert len(asic.dma) == 3 and len(asic.sprite) == 16, asic

        stamp, page = c.mem_read_stamped(kc.SPACE_ASIC_PAGE, 0,
                                         kc.ASIC_PAGE_SIZE)
        assert len(page) == kc.ASIC_PAGE_SIZE and stamp == asic.stamp, stamp
        # `asic palette`: 32 entries 'penN=0GRB' / 'inkN=0GRB'.
        resp = core.ipc('asic palette')
        assert resp.startswith('OK'), resp
        text = [int(entry.split('=')[1], 16) for entry in resp.split()[1:]]
        assert len(text) == 32, resp
        palette = page[kc.ASIC_PALETTE_OFFSET:kc.ASIC_PALETTE_OFFSET + 64]
        ours = [((palette[2 * i + 1] & 0x0F) << 8) | palette[2 * i]
                for i in range(32)]
        assert ours == text, (ours, text)
        assert ours[1] == 0xC5A and ours[16] == 0x321, ours
        # What the layout carries decoded is what the page holds raw.
        assert page[0x2800] == asic.pri_line, page[0x2800:0x2808].hex()

        # A part of the page, and its limits.
        assert c.mem_read(kc.SPACE_ASIC_PAGE, kc.ASIC_PALETTE_OFFSET,
                          64) == palette
        assert c.mem_read(kc.SPACE_ASIC_PAGE, kc.ASIC_PAGE_SIZE, 0) == b''
        expect_status(c, kc.OUT_OF_RANGE, 'past the page',
                      kc.SPACE_ASIC_PAGE, kc.ASIC_PAGE_SIZE - 1, 2)
        expect_status(c, kc.BAD_PAYLOAD, 'a bank', kc.SPACE_ASIC_PAGE, 0, 16,
                      bank=1)
        # The page is read-only here: the ASIC acts on writes to it.
        refused(kc.BAD_PAYLOAD, 'a write', c.mem_write, kc.SPACE_ASIC_PAGE,
                0, b'\x00')

        # The CPU view does not show the page, whatever is paged in.
        assert c.mem_read(kc.SPACE_CPU, 0x6402, 2) != bytes([0x5A, 0x0C])

    with Core() as core, core.client() as c:
        hello = c.hello('no asic')
        assert not hello.machine.has_asic, hello.machine
        refused(kc.NOT_PRESENT, 'CHIP_GET ASIC on a 6128', c.chip_get,
                kc.CHIP_ASIC)
        expect_status(c, kc.NOT_PRESENT, 'ASIC_PAGE on a 6128',
                      kc.SPACE_ASIC_PAGE, 0, kc.ASIC_PAGE_SIZE)
        # What is wrong with the request itself is said first.
        expect_status(c, kc.OUT_OF_RANGE, 'past the page, on a 6128',
                      kc.SPACE_ASIC_PAGE, 0, kc.ASIC_PAGE_SIZE + 1)
        c.ping()


def run_frames_plain(core: Core, c: KcpcClient, frames: int) -> None:
    """Let a running machine complete `frames` more frames."""
    target = c.machine_info().stamp.frame + frames
    wait_running(c, target)


def symbols_changed(c: KcpcClient) -> kc.Event:
    return c.wait_event(kc.EVT_SYMBOLS_CHANGED, EVENT_TIMEOUT_S)


def test_symbols_round_trip_and_are_pushed() -> None:
    """Acceptance 3: SYM_ADD then SYM_LIST round-trips; a second client is
    pushed EVT_SYMBOLS_CHANGED, and so is everybody when the text IPC's
    `sym add` edits the table. A name stands for one address."""
    with Core() as core, controller(core, 'symbols') as a, \
            observer(core, 'watcher', kc.CLASS_DEBUG) as b:
        assert a.hello_reply.caps & kc.CAP_SYMBOLS, hex(a.hello_reply.caps)
        a.subscribe(kc.CLASS_DEBUG)
        generation, symbols = b.sym_list()      # an observer may list
        assert symbols == [], symbols

        added = a.sym_add(0x4000, 'start')
        assert added > generation, (added, generation)
        assert not pending(a, kc.EVT_SYMBOLS_CHANGED), (
            'the event came before the reply')
        for who, c in (('controller', a), ('observer', b)):
            assert symbols_changed(c).generation == added, who
        assert a.sym_add(0xBB5A, 'TXT_OUTPUT') > added
        assert a.sym_add(0x0038, 'int.entry') > added
        for c in (a, b):
            for _ in range(2):
                symbols_changed(c)
        generation, symbols = b.sym_list()
        assert symbols == [(0x0038, 'int.entry'), (0x4000, 'start'),
                           (0xBB5A, 'TXT_OUTPUT')], symbols
        assert a.sym_list() == (generation, symbols)
        # The text IPC reads the same table.
        assert core.ipc('sym lookup start') == 'OK 4000', core.ipc(
            'sym lookup start')

        # The text IPC's edit is pushed to both, and listed.
        assert core.ipc('sym add 0x4100 other').startswith('OK')
        for who, c in (('controller', a), ('observer', b)):
            assert symbols_changed(c).generation > generation, who
        generation, symbols = a.sym_list()
        assert (0x4100, 'other') in symbols, symbols

        # A name is one address: adding it elsewhere moves it. An address has
        # one name: a second one replaces the first.
        a.sym_add(0x4200, 'start')
        a.sym_add(0x4100, 'another')
        symbols = a.sym_list()[1]
        assert symbols == [(0x0038, 'int.entry'), (0x4100, 'another'),
                           (0x4200, 'start'), (0xBB5A, 'TXT_OUTPUT')], symbols
        for c in (a, b):
            for _ in range(2):
                symbols_changed(c)

        before = a.sym_list()[0]
        assert a.sym_del('another') > before
        for c in (a, b):
            symbols_changed(c)
        refused(kc.NOT_FOUND, 'a name that is not there', a.sym_del, 'another')
        refused(kc.NOT_FOUND, 'a name the table never had', a.sym_del, 'nope')
        assert a.sym_list()[0] > before
        assert [name for _, name in a.sym_list()[1]] == [
            'int.entry', 'start', 'TXT_OUTPUT']

        # A file of symbols is one change, however many it holds.
        path = Path(os.path.realpath(core.tmpdir)) / 'game.sym'
        path.write_text('; labels\nal  $c000 .rom_entry\n'
                        'al  $4200 .main\nb  $c000\n', encoding='utf-8')
        before = a.sym_list()[0]
        generation, loaded = a.sym_load(str(path))
        assert loaded == 2 and generation > before, (loaded, generation)
        for c in (a, b):
            assert symbols_changed(c).generation == generation
        symbols = a.sym_list()[1]
        assert (0xC000, 'rom_entry') in symbols, symbols
        assert (0x4200, 'main') in symbols, symbols
        assert (0x4200, 'start') not in symbols, symbols
        refused(kc.NOT_FOUND, 'a file that is not there', a.sym_load,
                str(path) + '.missing')
        # The rule MEDIA_INSERT and SNAPSHOT_* apply to a path.
        refused(kc.BAD_PAYLOAD, "a path through '..'", a.sym_load,
                '../game.sym')

        # Checked before anything is changed.
        for what, name in (('no name', ''), ('a blank', 'two words'),
                           ('a control character', 'a\tb'),
                           ('too long', 'x' * (kc.MAX_SYMBOL_NAME + 1))):
            refused(kc.BAD_PAYLOAD, what, a.sym_add, 0x1234, name)
        refused(kc.BAD_PAYLOAD, 'SYM_ADD cut short', a.request,
                kc.OP_SYM_ADD, b'\x00')
        for call, args in ((b.sym_add, (0x5000, 'mine')),
                           (b.sym_del, ('start',)),
                           (b.sym_load, (str(path),))):
            refused(kc.NOT_CONTROLLER, 'an observer', call, *args)
        assert a.sym_list() == b.sym_list()
        # Nothing more was pushed for what was refused.
        a.ping()
        b.ping()
        for c in (a, b):
            assert not pending(c, kc.EVT_SYMBOLS_CHANGED), c.events
        # None of it paused or stopped the machine.
        assert not a.machine_info().stamp.paused


def test_fb_info_describes_the_frame() -> None:
    """Acceptance 5: FB_INFO reports the machine's canonical frame, 768 by
    272 pixels of RGB24 with 2304 bytes a row, and no transport yet."""
    with Core() as core, core.client() as c:
        hello = c.hello('fb')
        assert hello.caps & kc.CAP_FB_INFO, hex(hello.caps)
        fb = c.fb_info()
        assert fb == kc.FbInfo(768, 272, kc.FB_FORMAT_RGB24,
                               kc.FB_TRANSPORT_NONE, 2304, 0, ''), fb
        c.request(kc.OP_FB_INFO, b'from a later version')


def test_a_rebuilt_machine_is_announced() -> None:
    """Acceptance 4, second half: `config set model 3` and `config apply` on
    the text IPC push EVT_MACHINE_CHANGED with the new model, and from then
    on the ASIC is there. Loading a cartridge is a change of medium."""
    with Core() as core, observer(core, 'machine',
                                  kc.CLASS_MACHINE | kc.CLASS_MEDIA) as c, \
            observer(core, 'deaf', kc.CLASS_RUN) as deaf:
        before = c.machine_info()
        assert before.model == 2 and not before.has_asic, before
        refused(kc.NOT_PRESENT, 'the ASIC of a 6128', c.chip_get,
                kc.CHIP_ASIC)
        assert core.ipc('config set model 3').startswith('OK')
        # Staged, not applied: nothing was rebuilt yet.
        c.ping()
        assert not pending(c, kc.EVT_MACHINE_CHANGED), c.events
        assert core.ipc('config apply').startswith('OK')
        event = c.wait_event(kc.EVT_MACHINE_CHANGED, EVENT_TIMEOUT_S)
        assert event.machine.model == 3 and event.machine.has_asic, event
        assert event.machine.ram_bytes == before.ram_bytes, event
        after = c.machine_info()
        assert (after.model, after.flags, after.ram_bytes,
                after.rom_slots) == (
            event.machine.model, event.machine.flags,
            event.machine.ram_bytes, event.machine.rom_slots), (event, after)
        assert c.chip_get(kc.CHIP_ASIC).stamp.frame >= event.frame
        assert len(c.mem_read(kc.SPACE_ASIC_PAGE, 0,
                              kc.ASIC_PAGE_SIZE)) == kc.ASIC_PAGE_SIZE

        # RAM size is part of the machine too.
        assert core.ipc('config set ram_size 512').startswith('OK')
        assert core.ipc('config apply').startswith('OK')
        event = c.wait_event(kc.EVT_MACHINE_CHANGED, EVENT_TIMEOUT_S)
        assert event.machine.ram_bytes == 512 * 1024, event
        assert event.machine.model == 3, event

        cartridge = ROOT / 'rom' / 'system.cpr'
        generation = 0
        assert core.ipc(f'load {cartridge}').startswith('OK')
        while True:
            event = c.wait_event(kc.EVT_MEDIA_CHANGED, EVENT_TIMEOUT_S)
            if event.slot == kc.SLOT_CARTRIDGE:
                break
        assert event.present and event.generation > generation, event
        assert Path(event.path) == cartridge, event

        # A client that asked for neither class was sent neither event (the
        # rebuild's EVT_RESET it did ask for).
        deaf.ping()
        assert not pending(deaf, kc.EVT_MEDIA_CHANGED,
                           kc.EVT_MACHINE_CHANGED), deaf.events


def test_polling_every_chip_changes_nothing() -> None:
    """Acceptance 6: a client that reads all five chips and the ASIC's
    register page at every frame boundary does not change what the machine
    computes, and every one of its reads is taken at a frame boundary: none
    pauses the machine. On a 6128+, so that the ASIC is among them."""
    frames = 100
    with Core('-O', 'system.model=3') as core:
        alone = _stepped_state(core, frames, 0)
        again = _stepped_state(core, frames, 0)
        assert alone[:4] == again[:4], (
            'the reference run is not reproducible; the comparison below '
            'would prove nothing')
        assert alone[0] == frames and alone[1], alone[:2]
        running_reads = 0
        for readers in (1, 3):
            watched = _stepped_state(core, frames, readers, 'chips')
            r = watched[4]
            label = f'{readers} x chips'
            # How many reads fit into the step depends on how fast the host
            # turns a request round (a slow CI Mac got 41 of a hundred
            # boundaries). That they were taken at a boundary is what the
            # core's own count says, below.
            assert r.running_reads >= readers, (
                f'{label}: {r.calls} reads, {r.running_reads} of a running '
                'machine')
            running_reads += r.running_reads
            assert watched[0] == frames and watched[1], (label, watched[:2])
            assert watched[2] == alone[2], (
                f'{label}: registers differ:\n  alone   {alone[2]}\n'
                f'  watched {watched[2]}')
            assert watched[3] == alone[3], (
                f'{label}: RAM differs after the same {frames} frames')
            print(f'    {label}: {r.calls} reads during {frames} frames '
                  f'({r.running_reads} of a running machine, slowest '
                  f'{r.slowest * 1000:.0f} ms); registers and RAM identical')

        # And a running machine keeps its 50 frames a second under them.
        # RUN_STATE is two atomics: asking it does not wait for a frame.
        with core.client() as c:
            c.hello('fps')
            assert core.ipc('run').startswith('OK')
            wait_running(c)

            def frames_a_second(seconds: float) -> float:
                begin = time.monotonic()
                first = c.run_state().stamp.frame
                time.sleep(seconds)
                last = c.run_state().stamp.frame
                return (last - first) / (time.monotonic() - begin)

            # One window of each was a coin toss on a shared CI Mac: a host
            # hiccup lands on whichever window is open (49.7 alone, 46.5
            # beside the readers). Interleaved windows share the host's
            # weather, and the best of each is what the machine can do: a
            # cost of reading is paid in every window beside the readers.
            alone_fps: List[float] = []
            watched_fps: List[float] = []
            calls = 0
            for _ in range(4):
                alone_fps.append(frames_a_second(1.5))
                with Readers(core, 2, 'chips') as r:
                    watched_fps.append(frames_a_second(1.5))
                assert not r.errors, r.errors
                running_reads += r.running_reads
                calls += r.calls
            print('    frames a second alone '
                  f'{", ".join(f"{f:.1f}" for f in alone_fps)}; beside '
                  f'{calls} chip reads '
                  f'{", ".join(f"{f:.1f}" for f in watched_fps)}')
            # Against the pace when the machine alone read above it: the
            # claim is that reading costs no frames, not that it adds some.
            best = max(watched_fps)
            assert best > 45 and best > min(max(alone_fps), 50.0) - 3, (
                alone_fps, watched_fps)
        served = core_reads(core)
        # Every read that saw the machine running was copied by the runner
        # at a frame boundary (a lease's read of a running machine is
        # stamped running too), but for the odd one the gate's safety net
        # took under a lease after a quarter second without a frame: a
        # loaded host stalls the runner that long now and then (2 of 378
        # reads on the efficiency cores of a saturated Mac). A gate that
        # pauses for reads takes them all that way.
        assert served['busy'] == 0, served
        assert served['fallback'] * 20 <= served['boundary'], served
        assert served['boundary'] + served['fallback'] >= running_reads, (
            served, running_reads)
        print(f'    reads served: {served}')


def test_a_stop_is_not_caught_up() -> None:
    """Time a machine spent stopped is not owed: after a stop shorter than
    the pacer's quarter second resync, the machine runs at its 50 frames a
    second, not a burst of the frames it would have run meanwhile. (It ran
    them unpaced: 54 frames a second for the two seconds after a stop of
    170 ms, which is what a slow host showed of it, beads-drzi.)"""
    with Core() as core, core.client() as c:
        c.hello('stop')
        wait_running(c)
        time.sleep(0.5)  # at pace, on the pacer's grid
        for stop in (0.0, 0.05, 0.1):
            stopped = time.monotonic()
            first = pause_core(core, c)
            time.sleep(stop)
            begin = time.monotonic()
            assert core.ipc('run').startswith('OK')
            time.sleep(1.0)
            last = c.run_state().stamp
            seconds = time.monotonic() - begin
            ran = last.frame - first.frame
            print(f'    stopped {(begin - stopped) * 1000:.0f} ms: {ran} '
                  f'frames in the {seconds:.2f} s after')
            assert not last.paused, last
            # Two frames of slack: the one in flight and the clock's grain.
            assert ran <= seconds * 50 + 2, (stop, ran, seconds)


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

        hello = c.hello('test_kcpc recoverable', max_frame=1 << 20)
        assert hello.max_frame == 1 << 20, hello.max_frame

        # An opcode of a range that is reserved, and one that exists
        # nowhere.
        for opcode in (kc.OP_RESERVED_INPUT, 0x7FFF, kc.OP_ERROR):
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


# Requests that carry no payload: an empty one is the whole request, so their
# refusal is the role or the order of frames, never BAD_PAYLOAD.
EMPTY_REQUESTS = {'PING', 'MACHINE_INFO', 'RUN_STATE', 'PAUSE', 'RUN',
                  'REGS_GET', 'MEM_INFO', 'BP_LIST', 'KEYS_RELEASE',
                  'INPUT_STATE', 'SYM_LIST', 'FB_INFO', 'CONFIG_APPLY'}


def doc_opcode_table() -> List[Tuple[int, str, str]]:
    """(opcode, name, who) for every row of section 13 of the protocol
    document, the opcode table."""
    doc = (ROOT / 'docs' / 'protocol' / 'kcpc-binary-v1.md').read_text(
        encoding='utf-8')
    section = doc.split('## 13. Opcode table', 1)[1].split('\n## ', 1)[0]
    rows = re.findall(r'^\| `0x([0-9A-Fa-f]{4})` \| `([A-Z_]+)` \| [^|]+\| '
                      r'([a-z ]+?) \|', section, re.MULTILINE)
    assert len(rows) > 30, f'only {len(rows)} rows in the opcode table'
    return [(int(code, 16), name, who) for code, name, who in rows]


def test_every_opcode_has_a_documented_refusal() -> None:
    """Every opcode of the document's table is the client's, with the same
    number, and each is refused as the document says: sent as a first frame
    it is HELLO_REQUIRED and closes the connection; from an observer, one
    that is the controller's is NOT_CONTROLLER; one that carries a payload
    is BAD_PAYLOAD without it. ERROR is the server's alone. Every refusal
    keeps the connection open, and none changes the run state."""
    table = doc_opcode_table()
    client = {name[3:]: value for name, value in vars(kc).items()
              if name.startswith('OP_') and name != 'OP_RESERVED_INPUT'}
    assert {name for _, name, _ in table} == set(client), (
        sorted({name for _, name, _ in table} ^ set(client)))
    for code, name, who in table:
        assert client[name] == code, (name, hex(code), hex(client[name]))
        assert who in ('anyone', 'controller', 'server only'), (name, who)

    with Core() as core:
        port = core.binary_port
        for code, name, _ in table:
            if name == 'HELLO':
                continue
            sock = connect(port)
            try:
                sock.sendall(kc.encode_frame(kc.KIND_REQUEST, code, 1, b''))
                expect_error_and_close(sock, kc.HELLO_REQUIRED,
                                       f'{name} before HELLO')
            finally:
                sock.close()

        with controller(core, 'refusals') as a, \
                observer(core, 'refused') as b:
            running = a.run_state().stamp
            for code, name, who in table:
                if who == 'server only':
                    refused(kc.UNKNOWN_OPCODE, f'{name} from a client',
                            a.request, code, b'')
                    continue
                if name == 'HELLO':
                    continue   # a second HELLO closes: test_bad_frames_...
                if who == 'controller':
                    refused(kc.NOT_CONTROLLER, f'{name} from an observer',
                            b.request, code, b'')
                if name not in EMPTY_REQUESTS:
                    refused(kc.BAD_PAYLOAD, f'{name} with no payload',
                            a.request, code, b'')
            a.ping()
            b.ping()
            assert a.run_state().stamp.paused == running.paused


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
            # Acceptance 6 of slice 4: EVT_SHUTDOWN, then the ERROR, then
            # end of stream.
            expect_error_and_close(said_hello.sock, kc.SHUTTING_DOWN,
                                   'client at quit', shutdown=True)
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
                                   'client at SIGTERM', shutdown=True)
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
    test_control_roles_and_forced_takeover,
    test_silent_controller_is_dropped,
    test_regs_set_changes_exactly_one_register,
    test_pc_edit_at_a_breakpoint_matches_the_text_ipc,
    test_mem_write_lands_in_the_ram_it_names,
    test_writes_leave_the_run_state_alone,
    test_readers_and_a_writer_at_once,
    test_pause_and_run_are_pushed_to_every_subscriber,
    test_stops_from_the_text_ipc_are_pushed,
    test_reads_and_writes_are_not_run_state_events,
    test_step_in_ends_with_an_event,
    test_step_frames_counts_like_the_text_ipc,
    test_reset_is_pushed,
    test_run_is_not_lost_to_another_actors_lease,
    test_frame_events_arrive_at_the_interval,
    test_a_client_that_stops_reading_is_told_what_it_missed,
    test_control_changes_reach_every_client,
    test_subscribers_do_not_change_what_the_machine_computes,
    test_a_lease_does_not_end_a_text_ipc_walk,
    test_a_run_for_a_frame_step_is_not_lost_to_a_lease,
    test_a_breakpoint_hit_is_pushed_to_both_clients,
    test_watch_and_io_hits_say_what_was_hit,
    test_conditions_pass_counts_and_limits,
    test_text_ipc_edits_reach_the_binary_clients,
    test_step_over_out_and_to,
    test_a_step_can_be_stopped_by_something_else,
    test_a_kcpc_walk_runs_beside_other_actors_leases,
    test_quit_during_a_walk,
    test_step_over_a_restart_uses_a_prepared_rom,
    test_a_key_reaches_the_matrix_and_the_firmware,
    test_joystick_keys_reach_a_program_reading_the_ppi,
    test_held_keys_are_released_when_control_is_lost,
    test_text_is_typed_by_the_autotype_queue,
    test_mouse_needs_a_mouse_and_moves_the_amx_counters,
    test_input_needs_control_and_is_checked,
    test_chip_get_matches_the_text_ipc,
    test_fdc_chip_and_media_events,
    test_asic_state_and_register_page,
    test_symbols_round_trip_and_are_pushed,
    test_fb_info_describes_the_frame,
    test_a_rebuilt_machine_is_announced,
    test_polling_every_chip_changes_nothing,
    test_a_disc_runs_and_a_snapshot_round_trips,
    test_media_requests_are_checked,
    test_unsaved_writes_go_back_to_their_own_disc,
    test_the_tape_deck_is_driven_and_reported,
    test_configuration_is_read_set_and_applied,
    test_a_stop_is_not_caught_up,
    test_quit_while_clients_read,
    test_bad_frames_close_the_connection,
    test_recoverable_errors_keep_the_connection,
    test_every_opcode_has_a_documented_refusal,
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
