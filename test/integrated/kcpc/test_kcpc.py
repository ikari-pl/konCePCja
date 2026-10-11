#!/usr/bin/env python3
"""End-to-end tests for the KCPC binary frontend server (beads-3o1).

Each test starts a real core, finds the binary port the way a frontend must
(`ports.binary` of the startup manifest, never a hardcoded 6545) and talks to
it with kcpc_client.py. The suite grows with every slice of
docs/protocol/kcpc-binary-v1.md; this is slice 1: framing, HELLO, PING,
MACHINE_INFO, ERROR, the client limit, the manifest line, the
[ipc] binary_port_enabled key and shutdown with clients connected.

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
            # Slice 1 completes no capability group.
            assert hello.caps == 0, f'caps {hello.caps:#x}'

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
    """`count` clients that each call MACHINE_INFO in a loop, as fast as the
    server answers, until stopped. Every call takes a pause lease, so this is
    the heaviest thing slice 1 lets a client do to the machine."""

    def __init__(self, core: Core, count: int):
        self.core = core
        self.stop = threading.Event()
        self.calls = 0
        self.errors: List[str] = []
        self.threads = [threading.Thread(target=self._run)
                        for _ in range(count)]

    def _run(self) -> None:
        try:
            with self.core.client() as c:
                c.hello('test_kcpc reader')
                while not self.stop.is_set():
                    c.machine_info()
                    self.calls += 1
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


def _stepped_state(core: Core, frames: int, readers: int) -> Tuple[
        int, bool, str, str, int]:
    """Reset to a known paused state, run `frames` frames with the text
    IPC's `step frame` while `readers` clients read, and describe the
    machine afterwards: (frames run, paused, registers, RAM, reads)."""
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
        with Readers(core, readers) as r:
            resp = core.ipc(f'step frame {frames}')
        assert resp.startswith('OK'), resp
        assert not r.errors, r.errors
        after = c.machine_info().stamp
        regs = core.ipc('regs')
        ram = ''.join(core.ipc(f'mem read {addr} 4096 --view=ram')
                      for addr in range(0, 0x10000, 0x1000))
        return after.frame - before.frame, after.paused, regs, ram, r.calls


def test_reads_do_not_change_what_the_machine_computes() -> None:
    """Epic invariant 2: a run with clients attached makes the same
    observations as a run without. From the same reset, 100 stepped frames
    end in the same registers and the same 64 KiB of RAM whether or not two
    clients read MACHINE_INFO all the way through, and the step itself runs
    exactly 100 frames and ends paused. (A read used to abandon the text
    IPC's frame step after a few frames and leave the machine running.)"""
    frames = 100
    with Core() as core:
        alone = _stepped_state(core, frames, 0)
        again = _stepped_state(core, frames, 0)
        assert alone[:4] == again[:4], (
            'the reference run is not reproducible; the comparison below '
            'would prove nothing')
        watched = _stepped_state(core, frames, 2)
    assert alone[0] == frames and alone[1], alone[:2]
    assert watched[4] > 50, f'only {watched[4]} reads during the step'
    assert watched[0] == frames, (
        f'the step ran {watched[0]} frames with clients reading, not '
        f'{frames}')
    assert watched[1], 'the step did not end paused with clients reading'
    assert watched[2] == alone[2], (
        f'registers differ:\n  alone   {alone[2]}\n  watched {watched[2]}')
    assert watched[3] == alone[3], 'RAM differs after the same 100 frames'
    print(f'    {watched[4]} reads during {frames} stepped frames; '
          'registers and RAM identical')


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
        for opcode in (kc.OP_MEM_READ, 0x7FFF, kc.OP_ERROR):
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
