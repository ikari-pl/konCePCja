#!/usr/bin/env python3
"""
IPC Test Harness for konCePCja Emulator

Connects to the emulator's IPC server (port 6543) to run automated tests.
"""

import hashlib
import queue
import re
import signal
import socket
import subprocess
import shutil
import tempfile
import threading
import time
import sys
import os
from pathlib import Path
from typing import List, Optional, Tuple

class KoncepcjaIPC:
    """Client for konCePCja IPC protocol."""

    def __init__(self, host: str = 'localhost', port: int = 6543, timeout: float = 5.0):
        self.host = host
        self.port = port
        self.timeout = timeout

    def connect(self, retries: int = 10, delay: float = 0.5) -> bool:
        """Verify emulator IPC server is reachable with retries."""
        for i in range(retries):
            try:
                sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
                sock.settimeout(self.timeout)
                sock.connect((self.host, self.port))
                sock.close()
                return True
            except (ConnectionRefusedError, socket.timeout):
                if i < retries - 1:
                    time.sleep(delay)
        return False

    def disconnect(self):
        """No-op since we use per-request connections."""
        pass

    def send_command(self, cmd: str) -> Tuple[bool, str]:
        """Send command and return (success, response).

        Note: one fresh connection per request, for simplicity. The server
        itself keeps a connection open across commands ('disconnect' closes
        it); nothing here depends on that.
        """
        try:
            sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            sock.settimeout(self.timeout)
            sock.connect((self.host, self.port))
            sock.sendall((cmd + '\n').encode())
            response = sock.recv(65536).decode().strip()
            sock.close()
            success = response.startswith('OK')
            return success, response
        except socket.timeout:
            return False, "Timeout"
        except Exception as e:
            return False, str(e)

    def ping(self) -> bool:
        """Test connection."""
        ok, resp = self.send_command('ping')
        return ok and 'pong' in resp

    def pause(self) -> bool:
        """Pause emulation."""
        ok, _ = self.send_command('pause')
        return ok

    def run(self) -> bool:
        """Resume emulation."""
        ok, _ = self.send_command('run')
        return ok

    def reset(self) -> bool:
        """Reset CPC."""
        ok, _ = self.send_command('reset')
        return ok

    def wait_pc(self, addr: int, timeout_ms: int = 10000) -> bool:
        """Wait for program counter to reach address."""
        ok, _ = self.send_command(f'wait pc 0x{addr:04X} {timeout_ms}')
        return ok

    def wait_vbl(self, count: int, timeout_ms: int = 10000) -> bool:
        """Wait for N vertical blanks."""
        ok, _ = self.send_command(f'wait vbl {count} {timeout_ms}')
        return ok

    def read_mem(self, addr: int, length: int, ascii: bool = False) -> Tuple[bool, str]:
        """Read memory."""
        cmd = f'mem read 0x{addr:04X} {length}'
        if ascii:
            cmd += ' ascii'
        return self.send_command(cmd)

    def get_regs(self) -> Tuple[bool, dict]:
        """Get all registers."""
        ok, resp = self.send_command('regs')
        if not ok:
            return False, {}

        regs = {}
        # Parse "OK A=00 F=00 BC=0000 ..." format
        parts = resp.replace('OK ', '').split()
        for part in parts:
            if '=' in part:
                name, val = part.split('=')
                regs[name] = int(val, 16)
        return True, regs

    def screenshot(self, path: str) -> bool:
        """Take screenshot."""
        ok, _ = self.send_command(f'screenshot {path}')
        return ok

    def load_file(self, path: str) -> bool:
        """Load file (disk, snapshot, etc.)."""
        ok, _ = self.send_command(f'load {path}')
        return ok

    def step_in(self, count: int = 1) -> Tuple[bool, str]:
        """Step N instructions (enters CALLs). Requires emulator to be paused."""
        return self.send_command(f'step in {count}')

    def wait_bp(self, timeout_ms: int = 5000) -> Tuple[bool, str]:
        """Block until a breakpoint/watchpoint fires or timeout expires.

        Returns (True, 'OK PC=XXXX ...') on hit, (False, 'ERR 408 ...') on timeout.
        Use as a deadlock detector: if wait_bp times out when a breakpoint *should*
        have fired, the Z80 thread is likely stuck.
        """
        return self.send_command(f'wait bp {timeout_ms}')

    def bp_add(self, addr: int) -> bool:
        ok, _ = self.send_command(f'bp add 0x{addr:04X}')
        return ok

    def bp_clear(self) -> bool:
        ok, _ = self.send_command('bp clear')
        return ok

    def snapshot_save(self, path: str) -> bool:
        ok, _ = self.send_command(f'snapshot save {path}')
        return ok

    def snapshot_load(self, path: str) -> bool:
        ok, _ = self.send_command(f'snapshot load {path}')
        return ok

    def write_mem(self, addr: int, hexdata: str) -> bool:
        ok, _ = self.send_command(f'mem write 0x{addr:04X} {hexdata}')
        return ok

    def get_reg(self, name: str) -> Tuple[bool, int]:
        """Get a single register value."""
        ok, resp = self.send_command(f'reg get {name}')
        if not ok:
            return False, 0
        try:
            return True, int(resp.replace('OK', '').strip(), 16)
        except ValueError:
            return False, 0

    def is_threaded(self) -> bool:
        """Returns True if the emulator runs the threaded (GUI) loop.

        'gui' reports g_headless: gui=0 for the single-threaded -H loop (or a
        video init that fell back to it), gui=1 for the GUI build -- which is
        what SDL_VIDEODRIVER=dummy still runs, Z80 thread + render thread.
        (This used to send 'devtools', on the belief that it fails headless;
        it returns OK in both modes, so every run looked threaded.)
        """
        ok, resp = self.send_command('gui')
        return ok and 'gui=1' in resp


# The checkout this harness lives in: rom/, koncepcja.cfg.example, the make
# build's ./koncepcja.
PROJECT_ROOT = Path(__file__).resolve().parent.parent.parent


class EmulatorRunner:
    """Manages emulator process lifecycle."""

    test_engine: Optional[int] = None

    def __init__(self, exe_path: Optional[str] = None):
        if exe_path is None:
            # $KONCPC_EXE names another build of the binary (the CMake or
            # UI-free one in CI); otherwise use the make build at the root.
            exe_path = os.environ.get('KONCPC_EXE') or str(
                PROJECT_ROOT / 'koncepcja')
        self.exe_path = exe_path
        self.process: Optional[subprocess.Popen] = None
        self.ipc = KoncepcjaIPC()
        self._stderr_q: "queue.Queue[Optional[str]]" = queue.Queue()
        # Every stderr line, kept because _await_ipc_port consumes the queue:
        # a later probe (the telnet port) would otherwise find its line gone.
        self._stderr_lines: List[str] = []
        self._cfg_tmp = None  # throwaway config copy, removed on stop()

    def _pump_stderr(self) -> None:
        """Drain child stderr into _stderr_q for the process's whole life.

        Keeps the pipe from filling (which would block the emulator) and lets
        start() read the 'IPC: listening on port N' line to learn which port
        this specific instance bound. Puts None on EOF (process exited).
        """
        assert self.process is not None and self.process.stderr is not None
        for line in self.process.stderr:
            self._stderr_lines.append(line)
            self._stderr_q.put(line)
        self._stderr_q.put(None)

    def _await_ipc_port(self, timeout: float = 20.0) -> Optional[int]:
        """Return the port this spawned instance bound, parsed from its stderr.

        The server probe-forwards past busy ports (6543+), so the port is not
        knowable a priori. Returns None on timeout or if the process exits
        before logging it.
        """
        deadline = time.monotonic() + timeout
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                return None
            try:
                line = self._stderr_q.get(timeout=remaining)
            except queue.Empty:
                return None
            if line is None:  # process exited before logging the port
                return None
            # Match the IPC server's line specifically — the telnet console
            # logs a near-identical 'Telnet console: listening on port N' and
            # probes a neighbouring range, so a loose match could grab it.
            m = re.search(r'\bIPC: listening on port (\d+)', line)
            if m:
                return int(m.group(1))

    def await_logged_port(self, needle: str, timeout: float = 20.0) -> Optional[int]:
        """Return a port this instance logged, e.g. 'Telnet console'.

        Ports must always come from the process's own output: the servers
        probe forward past busy ports, so a hardcoded or derived number can
        silently address a different, already-running emulator.
        """
        pattern = re.compile(re.escape(needle) + r': listening on port (\d+)')
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            for line in list(self._stderr_lines):
                m = pattern.search(line)
                if m:
                    return int(m.group(1))
            time.sleep(0.1)
        return None

    def _drain_stderr_tail(self, max_lines: int = 20) -> str:
        """Non-blocking grab of buffered stderr lines, for error messages."""
        lines = []
        while len(lines) < max_lines:
            try:
                line = self._stderr_q.get_nowait()
            except queue.Empty:
                break
            if line is None:
                break
            lines.append(line.rstrip())
        return "\n".join(lines)

    def _await_ready(self, timeout: float = 15.0) -> bool:
        """Block until the emulated core is executing, or timeout.

        The IPC server accepts connections before the core finishes
        initializing (InputMapper and emulator_init() run after g_ipc->start()),
        so commands sent immediately land in a not-ready window: 'input state'
        returns 503, key/joy injection can touch a null InputMapper, and
        'pause' hangs. ('wait vbl' is no use here — it counts frames the core
        runs, so before the core is up it can only time out.) A non-zero Z80 PC
        proves the main loop is running, which only happens after
        emulator_init() completes — i.e. InputMapper and devices are ready too.
        """
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            ok, pc = self.ipc.get_reg('PC')
            if ok and pc:
                return True
            time.sleep(0.05)
        return False

    def start(self, *args, headless: bool = True, engine: Optional[int] = None) -> bool:
        """Start emulator with given arguments."""
        env = os.environ.copy()
        # A test must never raise a native modal: stop() terminates the
        # emulator, possibly with a disk the test itself dirtied, and the
        # "unsaved changes" dialog would then block the run (and land on the
        # maintainer's desktop). The emulator honours this in every mode.
        env['KONCPC_NO_DIALOGS'] = '1'
        # Windows that never take focus, clicks or host keystrokes: whoever
        # uses the desktop during a run can't type into the CPC.
        env['KONCPC_TEST_WINDOW'] = '1'
        if headless:
            env['SDL_VIDEODRIVER'] = 'dummy'
            env['SDL_AUDIODRIVER'] = 'dummy'

        cmd = [self.exe_path] + list(args)
        # KONCPC_E2E_HEADLESS=1 runs every test against the -H loop instead
        # of the GUI build (whose dummy-driver run still has a Z80 thread and
        # a render thread). Tests that need the threaded loop check
        # is_threaded() and skip.
        if (os.environ.get('KONCPC_E2E_HEADLESS') == '1'
                and '--headless' not in args and '-H' not in args):
            cmd.append('--headless')
        # Pin the config unless the caller chose one. Started from the repo
        # root the emulator would otherwise pick up $CWD/koncepcja.cfg — the
        # maintainer's live config, which makes runs depend on whoever is
        # sitting at the machine. A throwaway copy of the shipped example also
        # keeps a mid-run save from editing the example itself.
        if not any(a == '-c' or a.startswith('-c') or a == '--cfg_file'
                   or a.startswith('--cfg_file=') for a in args):
            example = PROJECT_ROOT / 'koncepcja.cfg.example'
            if not example.exists():
                # Silently carrying on would hand the emulator whoever's
                # koncepcja.cfg happens to sit in the working directory —
                # exactly the nondeterminism this pinning removes. A missing
                # example is a broken checkout, not a condition to absorb.
                raise FileNotFoundError(
                    f"{example} is missing; integration runs must not fall "
                    f"back to an arbitrary koncepcja.cfg")
            self._cfg_tmp = tempfile.NamedTemporaryFile(
                mode='w', suffix='.cfg', delete=False)
            self._cfg_tmp.write(example.read_text())
            self._cfg_tmp.close()
            cmd += ['-c', self._cfg_tmp.name]
        eng = engine if engine is not None else EmulatorRunner.test_engine
        if eng is not None:
            cmd += ['-O', f'system.engine={eng}']
        try:
            self.process = subprocess.Popen(
                cmd,
                env=env,
                stdout=subprocess.DEVNULL,  # verbose logs unused; an unread PIPE could block the emulator
                stderr=subprocess.PIPE,
                text=True,
                bufsize=1,  # line-buffered
            )
        except Exception as e:
            print(f"Failed to start emulator: {e}")
            self.process = None
            return False

        threading.Thread(target=self._pump_stderr, daemon=True).start()

        # Connect to THIS instance's actual IPC port, parsed from its stderr.
        # The server probe-forwards past busy ports, so hardcoding 6543 could
        # silently bind us to a different, already-running emulator
        # (beads-p6im). This also removes the need to kill whatever holds 6543.
        port = self._await_ipc_port()
        if port is None:
            print("Failed to start emulator: no 'IPC: listening on port' line "
                  f"on stderr. Recent output:\n{self._drain_stderr_tail()}")
            self.stop()
            return False
        self.ipc.port = port

        # Wait for IPC server to accept connections on its reported port.
        if not self.ipc.connect():
            print(f"Failed to start emulator: IPC not reachable on reported port {port}")
            self.stop()
            return False

        # The IPC server (g_ipc->start()) accepts connections before the core
        # finishes initializing (InputMapper and emulator_init() run after it),
        # so commands sent immediately land in a not-ready window: 'input state'
        # returns 503, input devices aren't constructed yet, and key/joy
        # injection can touch a null InputMapper. Block until the core is
        # running before handing the instance to a test.
        if not self._await_ready():
            print("Failed to start emulator: core did not become ready "
                  "(Z80 PC never advanced)")
            self.stop()
            return False
        return True

    def stop(self):
        """Stop emulator."""
        self.ipc.disconnect()
        if self.process:
            self.process.terminate()
            try:
                self.process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.process.kill()
            self.process = None
        if self._cfg_tmp is not None:
            try:
                os.unlink(self._cfg_tmp.name)
            except OSError:
                pass
            self._cfg_tmp = None

    def __enter__(self):
        return self

    def __exit__(self, *args):
        self.stop()


def test_z80_basic():
    """Basic Z80 test - verify registers after reset."""
    print("Running Z80 basic test...")

    with EmulatorRunner() as emu:
        if not emu.start():
            print("FAIL: Could not start emulator")
            return False

        emu.ipc.pause()
        emu.ipc.reset()
        time.sleep(0.1)

        ok, regs = emu.ipc.get_regs()
        if not ok:
            print("FAIL: Could not read registers")
            return False

        # After reset, PC should be at ROM entry point
        # SP should be initialized
        print(f"  PC=0x{regs.get('PC', 0):04X}")
        print(f"  SP=0x{regs.get('SP', 0):04X}")

        print("PASS: Z80 basic test")
        return True


def test_memory_rw():
    """Test memory read/write via IPC."""
    print("Running memory R/W test...")

    with EmulatorRunner() as emu:
        if not emu.start():
            print("FAIL: Could not start emulator")
            return False

        emu.ipc.pause()

        # Write test pattern
        ok, _ = emu.ipc.send_command('mem write 0x4000 DEADBEEF')
        if not ok:
            print("FAIL: Could not write memory")
            return False

        # Read it back
        ok, resp = emu.ipc.read_mem(0x4000, 4)
        if not ok:
            print("FAIL: Could not read memory")
            return False

        if 'DE AD BE EF' in resp or 'DEADBEEF' in resp.upper():
            print("PASS: Memory R/W test")
            return True
        else:
            print(f"FAIL: Unexpected memory content: {resp}")
            return False


def test_mem_ram_view_under_rom_overlay():
    """beads-wxy6: --view=ram must see RAM under a paged-in lower ROM.

    The Fruity Frank lives-counter case: &1AF1 holds &3E in the 6128 OS ROM
    while the game stores a counter underneath. Default mem read returns the
    firmware byte; --view=ram returns the stored value. Also asserts bad-view
    rejection so agents cannot silently fall back to the CPU view.
    """
    print("Running mem --view=ram under ROM overlay test...")

    with EmulatorRunner() as emu:
        if not emu.start('--headless'):
            print("FAIL: Could not start emulator")
            return False

        ipc = emu.ipc
        if not ipc.pause():
            print("FAIL: Could not pause")
            return False

        addr = 0x1AF1
        ok, _ = ipc.send_command(f'mem write 0x{addr:04X} 03')
        if not ok:
            print("FAIL: Could not write under-ROM RAM")
            return False

        ok, _ = ipc.send_command('mem write 0x4000 03')
        if not ok:
            print("FAIL: Could not write reference byte at 0x4000")
            return False

        ok, cpu_resp = ipc.send_command(f'mem read 0x{addr:04X} 1')
        if not ok:
            print(f"FAIL: CPU-view read failed: {cpu_resp}")
            return False

        ok, ram_resp = ipc.send_command(f'mem read 0x{addr:04X} 1 --view=ram')
        if not ok:
            print(f"FAIL: RAM-view read failed: {ram_resp}")
            return False

        ram_hex = ram_resp.replace('OK', '').strip().upper()
        if ram_hex != '03':
            print(f"FAIL: --view=ram expected 03, got {ram_resp!r}")
            return False

        ok, bad = ipc.send_command(f'mem read 0x{addr:04X} 1 --view=bogus')
        if ok or 'bad-view' not in bad:
            print(f"FAIL: expected ERR 400 bad-view, got ok={ok} {bad!r}")
            return False

        ok, cmp_ram = ipc.send_command(
            f'mem compare 0x{addr:04X} 0x4000 1 --view=ram')
        if not ok or 'diffs=0' not in cmp_ram:
            print(f"FAIL: compare --view=ram expected diffs=0, got {cmp_ram!r}")
            return False

        ok, find_ram = ipc.send_command(
            f'mem find hex 0x1AF0 0x1AF2 03 --view=ram')
        if not ok or '1AF1' not in find_ram.upper():
            print(f"FAIL: find --view=ram missed under-ROM byte: {find_ram!r}")
            return False

        cpu_hex = cpu_resp.replace('OK', '').strip().upper()
        if cpu_hex != '03':
            # Strong path: lower ROM still overlays &1AF1.
            ok, cmp_cpu = ipc.send_command(
                f'mem compare 0x{addr:04X} 0x4000 1')
            if not ok or 'diffs=0' in cmp_cpu:
                print(f"FAIL: CPU-view compare should differ under ROM, "
                      f"got {cmp_cpu!r}")
                return False
            print(f"PASS: mem --view=ram (CPU={cpu_hex} RAM=03 under ROM)")
        else:
            print("PASS: mem --view=ram (ROM banked out; alias + bad-view OK)")
        return True


def test_breakpoint():
    """Test breakpoint functionality."""
    print("Running breakpoint test...")

    with EmulatorRunner() as emu:
        if not emu.start():
            print("FAIL: Could not start emulator")
            return False

        emu.ipc.pause()

        # Set breakpoint at a ROM address
        ok, _ = emu.ipc.send_command('bp add 0x0038')  # RST 38h handler
        if not ok:
            print("FAIL: Could not add breakpoint")
            return False

        # Verify it's set
        ok, resp = emu.ipc.send_command('bp list')
        if not ok or '0038' not in resp.upper():
            print("FAIL: Breakpoint not listed")
            return False

        # Clear it
        ok, _ = emu.ipc.send_command('bp clear')
        if not ok:
            print("FAIL: Could not clear breakpoints")
            return False

        print("PASS: Breakpoint test")
        return True


def test_headless_runs_subcycle_engine():
    """beads-iymn: --headless must run the configured engine, not legacy.

    The bridge start used to sit inside the !g_headless gate and the headless
    loop called z80_execute() unconditionally — headless silently ran the
    LEGACY core whatever system.engine said.  PASS = the 'tier' command
    answers (bridge active) and PC advances under --headless.
    """
    print("Running headless subcycle-engine dispatch test...")

    with EmulatorRunner() as emu:
        if not emu.start('--headless'):
            print("FAIL: Could not start emulator (--headless)")
            return False
        ipc = emu.ipc
        ok, resp = ipc.send_command('tier')
        if not ok or 'effective=' not in resp:
            print(f"FAIL: bridge inactive under --headless: {resp}")
            return False
        moved, pc1, pc2 = pc_is_moving(ipc)
        if not moved:
            print(f"FAIL: PC frozen headless ({pc1} / {pc2})")
            return False
        print("PASS: headless runs the sub-cycle engine (tier OK, PC moves)")
        return True


def test_headless_honours_the_speed_limiter():
    """beads-gnx3: -H must run at real time when limit_speed=1.

    The -H loop asked the bridge for an unpaced frame whatever limit_speed
    said, so 500 frames took 0.27 s (~1850 FPS) where the GUI took 10 s. Both
    loops now pace through the bridge's 50 Hz deadline, and limit_speed=0
    keeps -H unpaced for benches. 100 frames are 99 periods: >= 1.98 s paced.
    """
    print("Running headless speed-limiter test...")
    exe = EmulatorRunner().exe_path
    example = PROJECT_ROOT / 'koncepcja.cfg.example'
    env = os.environ.copy()
    env['KONCPC_NO_DIALOGS'] = '1'
    env['SDL_AUDIODRIVER'] = 'dummy'
    cfg = tempfile.NamedTemporaryFile(mode='w', suffix='.cfg', delete=False)
    try:
        cfg.write(example.read_text())
        cfg.close()

        def run(limit):
            cmd = [exe, '--headless', '--exit-after=100f', '-c', cfg.name,
                   '-O', f'system.limit_speed={limit}']
            started = time.monotonic()
            proc = subprocess.run(cmd, env=env, stdout=subprocess.DEVNULL,
                                  stderr=subprocess.DEVNULL, timeout=60)
            return proc.returncode, time.monotonic() - started

        rc_paced, paced = run(1)
        rc_fast, fast = run(0)
    finally:
        os.unlink(cfg.name)
    if rc_paced != 0 or rc_fast != 0:
        print(f"FAIL: -H runs exited {rc_paced}/{rc_fast}, expected 0")
        return False
    if paced < 1.8:
        print(f"FAIL: -H with limit_speed=1 ran 100 frames in {paced:.2f}s "
              f"(real time is 2.0s): the speed limiter is ignored")
        return False
    if fast > paced - 0.8:
        print(f"FAIL: -H with limit_speed=0 took {fast:.2f}s against "
              f"{paced:.2f}s paced: the unpaced mode is gone")
        return False
    print(f"PASS: -H paced 100 frames in {paced:.2f}s, unpaced in {fast:.2f}s")
    return True


def test_programmatic_quit_exit_codes():
    """IPC `quit N` and SIGTERM end the process with the right exit code.

    Headless polls no SDL events, so the SDL_EVENT_QUIT that `quit` used to
    push from the IPC thread was never read: the command answered OK and the
    emulator ran on. A signal is not a success either: it exits 128+signo.
    """
    cases = [('ipc', 3), ('sigterm', 143), ('sigint', 130)]
    if os.name != 'posix':
        # Popen.send_signal(SIGTERM) on Windows is TerminateProcess(h, 1): no
        # signal reaches the emulator, so only the IPC case means anything.
        print("  (not POSIX: signal cases skipped, IPC quit only)")
        cases = cases[:1]
    for how, expected in cases:
        with EmulatorRunner() as emu:
            if not emu.start('--headless'):
                print(f"FAIL: could not start emulator ({how})")
                return False
            proc = emu.process
            # The runner pins a throwaway copy of the shipped example config.
            # A signalled exit is not a clean exit, so it must not rewrite it:
            # a harness that kills the emulator would otherwise leave settings
            # behind that no one asked to save.
            cfg = Path(emu._cfg_tmp.name) if emu._cfg_tmp else None
            before = hashlib.md5(cfg.read_bytes()).hexdigest() if cfg else None
            if how == 'ipc':
                emu.ipc.send_command('quit 3')
            elif how == 'sigterm':
                proc.send_signal(signal.SIGTERM)
            else:
                proc.send_signal(signal.SIGINT)
            try:
                rc = proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                print(f"FAIL: {how} quit left the emulator running")
                return False
            if rc != expected:
                print(f"FAIL: {how} quit exited {rc}, expected {expected}")
                return False
            if cfg is not None and how != 'ipc':
                after = hashlib.md5(cfg.read_bytes()).hexdigest()
                if after != before:
                    print(f"FAIL: {how} rewrote the config file")
                    return False
    print("PASS: IPC quit exits 3, SIGTERM exits 143, SIGINT exits 130, "
          "and a signalled exit leaves the config alone")
    return True


def test_double_signal_terminates():
    """A second SIGTERM must kill the emulator even if the first is unhandled.

    on_terminate_signal() records the first signal for the main loop to drain;
    a second one restores the OS default disposition and re-raises, so an
    operator's second Ctrl+C still kills a wedged main thread. Whichever of
    the two paths wins the race, the contract the operator sees is the same:
    the process is gone, and not because the harness's own SIGKILL fallback
    reaped it 5 seconds later.
    """
    if os.name != 'posix':
        # No POSIX signals on Windows: send_signal is TerminateProcess.
        print("SKIP: double SIGTERM needs POSIX signals")
        return True
    with EmulatorRunner() as emu:
        if not emu.start('--headless'):
            print("FAIL: could not start emulator")
            return False
        proc = emu.process
        proc.send_signal(signal.SIGTERM)
        proc.send_signal(signal.SIGTERM)
        try:
            rc = proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            print("FAIL: two SIGTERMs left the emulator running")
            return False
    # -SIGTERM: the second signal hit the force-kill branch (default
    # disposition, re-raised). 143: the main loop drained the first one
    # before the second landed, so the second was itself a first.
    if rc not in (-signal.SIGTERM, 143):
        print(f"FAIL: double SIGTERM exited {rc}, expected -15 or 143")
        return False
    how = "force-killed by the second signal" if rc < 0 else "clean 128+signo"
    print(f"PASS: double SIGTERM terminated the emulator ({how})")
    return True


def test_engine1_bp_clear_resume():
    """beads-4gf9: clearing a hit breakpoint and resuming must truly resume.

    Under the sub-cycle engine (system.engine=1) the legacy breakpoint lists
    mirror into the probe.  The mirror must refresh BEFORE the resumed frame
    runs — otherwise a just-cleared breakpoint re-fires off the stale probe
    within milliseconds and re-pauses forever (the resume livelock).  PASS =
    PC visibly advances after bp clear + run.
    """
    print("Running engine=1 bp-clear-resume livelock test...")

    with EmulatorRunner() as emu:
        if not emu.start('-O', 'system.engine=1'):
            print("FAIL: Could not start emulator (engine=1)")
            return False
        ipc = emu.ipc
        ok, _ = ipc.send_command('bp add 0x0038')
        if not ok:
            print("FAIL: bp add")
            return False
        ok, resp = ipc.send_command('wait bp 5000')  # RST38 fires ~every 20 ms
        if not ok:
            print(f"FAIL: breakpoint never hit: {resp}")
            return False
        ipc.send_command('bp clear')
        ipc.send_command('run')
        time.sleep(0.5)
        ok1, pc1 = ipc.send_command('reg get PC')
        time.sleep(0.3)
        ok2, pc2 = ipc.send_command('reg get PC')
        if not (ok1 and ok2):
            print("FAIL: reg get PC after resume")
            return False
        if pc1 == pc2:
            print(f"FAIL: PC frozen at {pc1} after bp clear + run (livelock)")
            return False
        print("PASS: engine=1 resumed after bp clear (PC advances)")
        return True


def _firmware_time(ipc) -> Optional[int]:
    """The firmware TIME counter at &B8B4 (4 bytes, LE, 300 ticks/s).

    The frame flyback interrupt advances it 6 ticks per 50Hz frame, so a
    difference divided by 6 is the number of frames the machine ran.
    """
    ok, resp = ipc.read_mem(0xB8B4, 4)
    if not ok:
        return None
    digits = ''.join(resp.replace('OK', '').split())[:8]
    try:
        return int.from_bytes(bytes.fromhex(digits), 'little')
    except ValueError:
        return None


def test_wait_vbl_counts_real_frames():
    """`wait vbl N` must run N frames of emulation, not N x 20ms of wall time.

    It used to resume, sleep 20ms per blank and pause, so the frame count was
    whatever the host managed in that time: 143-182 frames for `wait vbl 50`
    unloaded, 260-275 with the E-cores saturated (beads-71pg). It now rides
    the frame-step counter `step frame` uses, so it ends paused exactly N
    frames on. Measured with the firmware TIME counter in the threaded GUI
    loop (dummy video) AND the -H headless loop, which count frames in
    different places. From a paused machine the count is exact; from a
    running one the frame in flight is the first of the N blanks.
    """
    print("Running wait vbl frame-count test...")
    for mode_args, mode in (((), 'gui'), (('--headless',), 'headless')):
        with EmulatorRunner() as emu:
            if not emu.start(*mode_args):
                print(f"FAIL [{mode}]: Could not start emulator")
                return False
            ipc = emu.ipc
            ipc.timeout = 20.0
            # Past the boot: the firmware sets TIME up during initialisation.
            ok, resp = ipc.send_command('step frame 100')
            if not ok:
                print(f"FAIL [{mode}]: step frame 100: {resp}")
                return False
            for n in (1, 7, 50):
                for start_state in ('paused', 'running'):
                    if start_state == 'running':
                        ipc.run()
                        time.sleep(0.05)
                    else:
                        ipc.pause()
                    t0 = _firmware_time(ipc)
                    ok, resp = ipc.send_command(f'wait vbl {n} 15000')
                    t1 = _firmware_time(ipc)
                    if not ok:
                        print(f"FAIL [{mode}]: wait vbl {n} ({start_state}): "
                              f"{resp}")
                        return False
                    if t0 is None or t1 is None:
                        print(f"FAIL [{mode}]: could not read TIME")
                        return False
                    # Exact from a paused start. From a running one t0 is
                    # read before the wait arms, and an unpaced headless loop
                    # (~1800 fps) runs a dozen frames in that round trip, so
                    # there only the outcome is checked: OK, then paused.
                    frames = (t1 - t0) / 6
                    if start_state == 'paused' and frames != n:
                        print(f"FAIL [{mode}]: wait vbl {n} (paused) ran "
                              f"{frames:g} frames, want {n}")
                        return False
                    # It ends paused, like every other wait.
                    t2 = _firmware_time(ipc)
                    time.sleep(0.1)
                    if _firmware_time(ipc) != t2:
                        print(f"FAIL [{mode}]: machine still running after "
                              f"wait vbl {n}")
                        return False
            # The reference: `step frame 50` runs exactly the same 50.
            t0 = _firmware_time(ipc)
            ipc.send_command('step frame 50')
            step_frames = (_firmware_time(ipc) - t0) / 6
            t0 = _firmware_time(ipc)
            ipc.send_command('wait vbl 50')
            vbl_frames = (_firmware_time(ipc) - t0) / 6
            if step_frames != vbl_frames:
                print(f"FAIL [{mode}]: step frame 50 ran {step_frames:g}, "
                      f"wait vbl 50 ran {vbl_frames:g}")
                return False
            # A breakpoint stopping the machine first ends the wait with
            # ERR 409 and leaves the hit for `wait bp`. RST &38 runs on every
            # interrupt, so it fires inside the first frame.
            ipc.send_command('bp add 0x0038')
            ok, resp = ipc.send_command('wait vbl 50 5000')
            if ok or 'stopped-elsewhere' not in resp:
                print(f"FAIL [{mode}]: wait vbl through a breakpoint: {resp!r}")
                return False
            ok, resp = ipc.send_command('wait bp 1000')
            if not ok or 'PC=0038' not in resp:
                print(f"FAIL [{mode}]: wait bp after the stop: {resp!r}")
                return False
            ipc.send_command('bp clear')
            # A wait that cannot finish times out and still ends paused.
            ok, resp = ipc.send_command('wait vbl 100000 200')
            if ok or not resp.startswith('ERR 408'):
                print(f"FAIL [{mode}]: expected ERR 408, got {resp!r}")
                return False
            t2 = _firmware_time(ipc)
            time.sleep(0.1)
            if _firmware_time(ipc) != t2:
                print(f"FAIL [{mode}]: machine left running after ERR 408")
                return False
            # ...and leaves no frame step armed: a later run is not cut short.
            ipc.run()
            moved, pc1, pc2 = pc_is_moving(ipc)
            if not moved:
                print(f"FAIL [{mode}]: machine stopped after run ({pc1}/{pc2})")
                return False
        print(f"  {mode}: wait vbl N ran N frames")
    print("PASS: wait vbl counts real frames")
    return True


def test_wait_vbl_timeout_ends_stopped():
    """`wait vbl` that times out must answer only once the machine HAS stopped.

    The timeout path paused the machine and answered ERR 408 at once, but a
    pause only stops the machine between frames: in the threaded GUI loop the
    frame in flight ran on after the reply, so the firmware clock moved on a
    machine the reply called paused (beads-wrkg, seen on a slow CI Mac). The
    Faithful tier, unpaced, makes a frame cost ~14 ms of host time, so the
    pause nearly always lands mid-frame: before the fix 2 runs in 10 moved.
    """
    print("Running wait vbl timeout stop test...")
    with EmulatorRunner() as emu:
        if not emu.start('-O', 'system.limit_speed=0'):
            print("FAIL: Could not start emulator")
            return False
        ipc = emu.ipc
        ipc.timeout = 20.0
        ok, resp = ipc.send_command('step frame 100')
        if not ok:
            print(f"FAIL: step frame 100: {resp}")
            return False
        ipc.send_command('tier set faithful')
        for i in range(25):
            ok, resp = ipc.send_command('wait vbl 100000 100')
            if ok or not resp.startswith('ERR 408'):
                print(f"FAIL: expected ERR 408, got {resp!r}")
                return False
            t2 = _firmware_time(ipc)
            time.sleep(0.05)
            t3 = _firmware_time(ipc)
            if t2 is None or t3 is None:
                print("FAIL: could not read TIME")
                return False
            if t3 != t2:
                print(f"FAIL: machine ran {t3 - t2} TIME ticks after ERR 408 "
                      f"(try {i + 1})")
                return False
    print("PASS: wait vbl timeout answers once the machine has stopped")
    return True


def _machine_moved_after(ipc, what: str) -> Optional[str]:
    """Why the machine is still running after `what` answered, or None.

    Reads TIME straight after the reply and again 50 ms later: a machine the
    reply called stopped must not have advanced in between.
    """
    t2 = _firmware_time(ipc)
    time.sleep(0.05)
    t3 = _firmware_time(ipc)
    if t2 is None or t3 is None:
        return "could not read TIME"
    if t3 != t2:
        return f"machine ran {t3 - t2} TIME ticks after {what}"
    return None


def test_pause_replies_once_stopped():
    """`pause` must answer only once the machine HAS stopped.

    It answered right after asking for the pause, which only stops the
    machine between frames: in the threaded GUI loop the frame in flight ran
    on after `OK`, so a read that followed saw the clock still moving
    (beads-veb0). Faithful, unpaced (~14 ms of host time per frame), makes
    the pause land mid-frame nearly every time.
    """
    print("Running pause stop test...")
    with EmulatorRunner() as emu:
        if not emu.start('-O', 'system.limit_speed=0'):
            print("FAIL: Could not start emulator")
            return False
        ipc = emu.ipc
        ipc.timeout = 20.0
        ok, resp = ipc.send_command('step frame 100')
        if not ok:
            print(f"FAIL: step frame 100: {resp}")
            return False
        ipc.send_command('tier set faithful')
        for i in range(25):
            ok, resp = ipc.send_command('run')
            if not ok:
                print(f"FAIL: run: {resp}")
                return False
            time.sleep(0.03 + 0.007 * (i % 3))
            ok, resp = ipc.send_command('pause')
            if not ok:
                print(f"FAIL: pause: {resp}")
                return False
            why = _machine_moved_after(ipc, 'pause')
            if why:
                print(f"FAIL: {why} (try {i + 1})")
                return False
    print("PASS: pause answers once the machine has stopped")
    return True


def test_config_set_machine_keys_on_a_running_machine():
    """`config set crtc_type|silicon_disc` on a machine that is running.

    Both keys change the machine (the CRTC device, the banking). They were
    set from the IPC thread while the frame runner could be mid-frame, with
    no pause lease (beads-7h32); now the set holds the machine still, alone,
    and puts the run state back. Unpaced, so the frame runner is nearly
    always inside a frame when a set arrives.
    """
    print("Running config set on a running machine test...")
    with EmulatorRunner() as emu:
        if not emu.start('-O', 'system.limit_speed=0'):
            print("FAIL: Could not start emulator")
            return False
        ipc = emu.ipc
        for i in range(60):
            crtc = i % 4
            for cmd in (f'config set crtc_type {crtc}',
                        f'config set silicon_disc {i % 2}'):
                ok, resp = ipc.send_command(cmd)
                if not ok:
                    print(f"FAIL: {cmd}: {resp}")
                    return False
            ok, resp = ipc.send_command('config get crtc_type')
            if not ok or not resp.startswith(f'OK {crtc}'):
                print(f"FAIL: config get crtc_type after {crtc}: {resp}")
                return False
        ok, resp = ipc.send_command('status')
        if not ok or 'paused=0' not in resp:
            print(f"FAIL: the sets left the machine paused: {resp}")
            return False
        ok, resp = ipc.send_command('sdisc status')
        if not ok or 'enabled' not in resp or 'disabled' in resp:
            print(f"FAIL: silicon disc after the last set: {resp}")
            return False
    print("PASS: config set of machine keys on a running machine")
    return True


def test_sdisc_commands_reach_the_running_machine():
    """`sdisc save|load|clear` act on the Silicon Disc the CPC is using.

    The board keeps the disc in its own expansion RAM (banks 4-7); the host
    buffer the commands used was only synced with it around a machine
    rebuild, so a save wrote what the disc held at the last rebuild and a
    load or clear never reached the CPC (beads-mtfy). Here a Z80 program
    writes the disc and another reads it back, through the real banking.
    """
    print("Running sdisc commands reach the running machine test...")
    size = 256 * 1024
    # Above &7FFF: configs 4 and 5 swap a disc bank in at &4000-&7FFF.
    base, park = 0x8000, 0x8020

    def program(ram_config, body):
        # DI : select ram_config : body : back to config 0 : park at `park`
        code = bytes([0xF3, 0x01, ram_config, 0x7F, 0xED, 0x49]) + body + \
            bytes([0x01, 0xC0, 0x7F, 0xED, 0x49])
        code += bytes(park - base - len(code)) + bytes([0x18, 0xFE])
        return code

    # bank 4, config 4: bank 4's first 16K at &4000; store &5A, &A5 there
    write_bank4 = program(0xE4, bytes([0x21, 0x00, 0x40, 0x36, 0x5A, 0x23,
                                       0x36, 0xA5]))
    # bank 5, config 5: bank 5's second 16K at &4000; LD HL,(&4000) and
    # LD (&9000),HL, into base RAM the test can read
    read_bank5 = program(0xED, bytes([0x2A, 0x00, 0x40, 0x22, 0x00, 0x90]))

    def run(ipc, code):
        for cmd in (f'mem write 0x{base:04X} {code.hex()}',
                    f'reg set PC 0x{base:04X}'):
            ok, resp = ipc.send_command(cmd)
            if not ok:
                print(f"FAIL: {cmd}: {resp}")
                return False
        ok, resp = ipc.send_command(f'wait pc 0x{park:04X} 5000')
        if not ok:
            print(f"FAIL: the program never parked: {resp}")
            return False
        return True

    def bank5_word(ipc):
        ok, resp = ipc.send_command('mem read 0x9000 2')
        return resp.split()[-1].upper() if ok else resp

    def sdisc(ipc, cmd):
        ok, resp = ipc.send_command(f'sdisc {cmd}')
        if not ok:
            print(f"FAIL: sdisc {cmd}: {resp}")
        return ok

    tmpdir = tempfile.mkdtemp(prefix='sdisc_')
    try:
        saved = os.path.join(tmpdir, 'saved.ksdx')
        loaded = os.path.join(tmpdir, 'loaded.ksdx')
        image = bytearray(size)
        image[0x14000:0x14002] = b'\x3c\xc3'  # bank 5, its second 16K
        with open(loaded, 'wb') as f:
            f.write(b'KSDX\x01\x00\x00\x00' + bytes(image))

        with EmulatorRunner() as emu:
            if not emu.start('-O', 'system.silicon_disc=1'):
                print("FAIL: Could not start emulator")
                return False
            ipc = emu.ipc
            ipc.pause()

            if not run(ipc, write_bank4) or not sdisc(ipc, f'save {saved}'):
                return False
            with open(saved, 'rb') as f:
                data = f.read()
            if len(data) != 8 + size or data[8:10] != b'\x5a\xa5':
                print(f"FAIL: sdisc save wrote {data[8:10].hex()} where the "
                      "CPC wrote 5aa5 (a stale copy of the disc)")
                return False

            if not sdisc(ipc, f'load {loaded}') or not run(ipc, read_bank5):
                return False
            word = bank5_word(ipc)
            if word != '3CC3':
                print(f"FAIL: after sdisc load the CPC reads {word}, "
                      "expected 3CC3")
                return False
            if not sdisc(ipc, f'save {saved}'):
                return False
            with open(saved, 'rb') as f:
                if f.read()[8:] != bytes(image):
                    print("FAIL: a save after a load is not the loaded image")
                    return False

            if not sdisc(ipc, 'clear') or not run(ipc, read_bank5):
                return False
            word = bank5_word(ipc)
            if word != '0000':
                print(f"FAIL: after sdisc clear the CPC reads {word}, "
                      "expected 0000")
                return False
            ok, resp = ipc.send_command('status')
            if not ok or 'paused=1' not in resp:
                print(f"FAIL: the sdisc commands ran the machine: {resp}")
                return False
    finally:
        shutil.rmtree(tmpdir, ignore_errors=True)
    print("PASS: sdisc commands reach the running machine")
    return True


def test_sdisc_enabled_at_run_time_is_fitted():
    """A Silicon Disc switched on or off at run time is on the board at once.

    `config set silicon_disc` and `sdisc load` on a disabled disc only set
    the host flag; the board gained banks 4-7 at the next rebuild, so on a
    128K machine the CPC's bank 4 was still expansion bank 0 while `sdisc
    status` said enabled (beads-t9vs). Here a Z80 program writes bank 4
    and the disc's image, expansion bank 0 and a re-fitted disc are checked.
    """
    print("Running sdisc enabled at run time is fitted test...")
    base, park = 0x8000, 0x8020

    def program(ram_config, body):
        # DI : select ram_config : body : back to config 0 : park at `park`
        code = bytes([0xF3, 0x01, ram_config, 0x7F, 0xED, 0x49]) + body + \
            bytes([0x01, 0xC0, 0x7F, 0xED, 0x49])
        code += bytes(park - base - len(code)) + bytes([0x18, 0xFE])
        return code

    # config 4 of bank 4 (&E4) / bank 0 (&C4): that bank's first 16K at &4000
    write_bank4 = program(0xE4, bytes([0x21, 0x00, 0x40, 0x36, 0x5A, 0x23,
                                       0x36, 0xA5]))
    copy = bytes([0x2A, 0x00, 0x40, 0x22, 0x00, 0x90])  # (&9000) = (&4000)
    read_bank4, read_bank0 = program(0xE4, copy), program(0xC4, copy)

    def run(ipc, code):
        for cmd in (f'mem write 0x{base:04X} {code.hex()}',
                    f'reg set PC 0x{base:04X}',
                    f'wait pc 0x{park:04X} 5000'):
            ok, resp = ipc.send_command(cmd)
            if not ok:
                print(f"FAIL: {cmd}: {resp}")
                return False
        return True

    def word(ipc, code):
        if not run(ipc, code):
            return None
        ok, resp = ipc.send_command('mem read 0x9000 2')
        return resp.split()[-1].upper() if ok else resp

    def send(ipc, cmd):
        ok, resp = ipc.send_command(cmd)
        if not ok:
            print(f"FAIL: {cmd}: {resp}")
        return ok

    tmpdir = tempfile.mkdtemp(prefix='sdisc_')
    try:
        saved = os.path.join(tmpdir, 'saved.ksdx')
        with EmulatorRunner() as emu:
            if not emu.start('-O', 'system.silicon_disc=0',
                             '-O', 'system.ram_size=128'):
                print("FAIL: Could not start emulator")
                return False
            ipc = emu.ipc
            ipc.pause()
            if not send(ipc, 'config set silicon_disc 1') or \
                    not run(ipc, write_bank4):
                return False
            got = word(ipc, read_bank0)
            if got != '0000':
                print(f"FAIL: expansion bank 0 reads {got} after the CPC "
                      "wrote bank 4: the disc was not fitted")
                return False
            if not send(ipc, f'sdisc save {saved}'):
                return False
            with open(saved, 'rb') as f:
                data = f.read()
            if data[8:10] != b'\x5a\xa5':
                print(f"FAIL: the disc holds {data[8:10].hex()} where the "
                      "CPC wrote 5aa5 to bank 4")
                return False

            # Taken out, bank 4 is no longer there; put back, the battery
            # kept what the CPC wrote.
            if not send(ipc, 'config set silicon_disc 0'):
                return False
            got = word(ipc, read_bank4)
            if got == '5AA5':
                print("FAIL: bank 4 still reads the disc after it was "
                      "switched off")
                return False
            if not send(ipc, 'config set silicon_disc 1'):
                return False
            got = word(ipc, read_bank4)
            if got != '5AA5':
                print(f"FAIL: the re-fitted disc reads {got}, expected 5AA5")
                return False

            # `sdisc load` on a switched-off disc fits it too.
            if not send(ipc, 'config set silicon_disc 0') or \
                    not send(ipc, f'sdisc load {saved}'):
                return False
            got = word(ipc, read_bank4)
            if got != '5AA5':
                print(f"FAIL: after sdisc load on a disabled disc bank 4 "
                      f"reads {got}, expected 5AA5")
                return False
            ok, resp = ipc.send_command('status')
            if not ok or 'paused=1' not in resp:
                print(f"FAIL: the sets ran the machine: {resp}")
                return False
    finally:
        shutil.rmtree(tmpdir, ignore_errors=True)
    print("PASS: sdisc enabled at run time is fitted")
    return True


def test_wait_pc_timeout_ends_stopped():
    """`wait pc` that times out must answer only once the machine HAS stopped.

    The walk's deadline paused the machine and returned Timeout at once, so
    ERR 408 went out while the frame in flight still ran (beads-veb0), the
    same shape `wait vbl` had (beads-wrkg). `step to` and `step over` share
    the walk. &9000 is RAM the idle BASIC prompt never executes.
    """
    print("Running wait pc timeout stop test...")
    with EmulatorRunner() as emu:
        if not emu.start('-O', 'system.limit_speed=0'):
            print("FAIL: Could not start emulator")
            return False
        ipc = emu.ipc
        ipc.timeout = 20.0
        ok, resp = ipc.send_command('step frame 100')
        if not ok:
            print(f"FAIL: step frame 100: {resp}")
            return False
        ipc.send_command('tier set faithful')
        for i in range(25):
            ok, resp = ipc.send_command('wait pc 0x9000 100')
            if ok or not resp.startswith('ERR 408'):
                print(f"FAIL: expected ERR 408, got {resp!r}")
                return False
            why = _machine_moved_after(ipc, 'ERR 408')
            if why:
                print(f"FAIL: {why} (try {i + 1})")
                return False
    print("PASS: wait pc timeout answers once the machine has stopped")
    return True


def test_wait_pc_is_exact():
    """`wait pc` must stop on an address the CPU only passes through.

    It used to poll the register view, which is published once per frame, so
    it saw only the PC a frame happened to end on. The -i launch test waits
    for &6000 while its program sits in KM READ CHAR almost all the time, and
    that wait timed out now and then on slow CI Macs with PC in the firmware.
    Here the target is one NOP in 1024: a frame ends on it about one time in a
    thousand, so a sampled wait nearly always misses while an exact one lands
    every time.
    """
    print("Running exact wait pc test...")
    program = bytes([0x00] * 1024 + [0xC3, 0x00, 0x60])  # NOPs; JP &6000
    with tempfile.NamedTemporaryFile(suffix='.bin', delete=False) as f:
        f.write(program)
        bin_path = f.name
    try:
        with EmulatorRunner() as emu:
            if not emu.start('-i', bin_path, '-o', '0x6000'):
                print("FAIL: Could not start emulator")
                return False
            ipc = emu.ipc
            ipc.timeout = 20.0
            ok, resp = ipc.send_command('wait pc 0x6000 15000')
            if not ok:
                print(f"FAIL: program never started: {resp}")
                return False
            for i in range(10):
                # Step off first: a wait issued while already standing on
                # `addr` is satisfied at once (the old contract), which would
                # prove nothing about catching it in flight.
                ipc.send_command('step 1')
                ok, resp = ipc.send_command('wait pc 0x6200 2000')
                if not ok:
                    print(f"FAIL: wait {i} missed &6200: {resp}")
                    return False
                _, regs = ipc.get_regs()
                if regs.get('PC') != 0x6200:
                    print(f"FAIL: wait {i} returned OK at PC="
                          f"{regs.get('PC', -1):04X}, not &6200")
                    return False
            # A breakpoint on the way is reported, not waited through.
            for cmd in ('step 1', 'bp add 0x6100'):
                ipc.send_command(cmd)
            ok, resp = ipc.send_command('wait pc 0x6200 2000')
            if ok or 'stopped-elsewhere' not in resp:
                print(f"FAIL: breakpoint at &6100 not reported: {resp}")
                return False
            ipc.send_command('bp clear')
            print("PASS: wait pc lands exactly, 10/10, and reports a "
                  "breakpoint on the way")
            return True
    finally:
        os.unlink(bin_path)


def test_run_to_address_breakpoint_polarity():
    """`wait pc` and `step to` must agree on what OK means.

    Two rules, one walk (z80_run_until_ephemeral), so both commands answer the
    same way:
      * a breakpoint of the caller's OWN armed at the target address is an
        arrival — the CPU is standing exactly where it was asked to stop — and
        used to be reported as `ERR 409 stopped-elsewhere` with the target in
        its own trailer;
      * anything else stopping the walk short is `ERR 409 stopped-elsewhere`,
        where `step to` used to answer `OK breakpoint-hit` — so a client whose
        success test is "the reply starts with OK" believed it had arrived.
    """
    print("Running run-to-address breakpoint polarity test...")
    program = bytes([0x00] * 1024 + [0xC3, 0x00, 0x60])  # NOPs; JP &6000
    with tempfile.NamedTemporaryFile(suffix='.bin', delete=False) as f:
        f.write(program)
        bin_path = f.name
    try:
        with EmulatorRunner() as emu:
            if not emu.start('-i', bin_path, '-o', '0x6000'):
                print("FAIL: Could not start emulator")
                return False
            ipc = emu.ipc
            ipc.timeout = 20.0
            ok, resp = ipc.send_command('wait pc 0x6000 15000')
            if not ok:
                print(f"FAIL: program never started: {resp}")
                return False
            ok, resp = ipc.send_command('bp add 0x6200')
            if not ok:
                print(f"FAIL: bp add: {resp}")
                return False
            # 1. wait pc onto its own breakpoint.
            ok, resp = ipc.send_command('wait pc 0x6200 3000')
            if not ok:
                print(f"FAIL: wait pc onto its own breakpoint at the target "
                      f"must be OK, got: {resp}")
                return False
            _, regs = ipc.get_regs()
            if regs.get('PC') != 0x6200:
                print(f"FAIL: OK at PC={regs.get('PC', -1):04X}, not &6200")
                return False
            # 2. step to onto the same breakpoint. Step off it first, or the
            #    walk would be satisfied where it already stands.
            ipc.send_command('step 1')
            ok, resp = ipc.send_command('step to 0x6200')
            if not ok:
                print(f"FAIL: step to onto its own breakpoint at the target "
                      f"must be OK, got: {resp}")
                return False
            # 3. step to interrupted by that breakpoint on the way to the
            #    target. The target is the breakpoint's own successor, and
            #    that choice is load-bearing: this program is 1024 NOPs at
            #    &6000 with a JP &6000 after them, so execution is a ring and
            #    a walk reaches &6201 only by executing &6200 immediately
            #    before it — from ANY starting PC. Asking for a target
            #    further ahead (&6300, as this first did) only crosses &6200
            #    when the machine happens to sit behind it, which is why that
            #    version passed here and failed on a loaded CI runner: under
            #    load the machine drifts between commands and the breakpoint
            #    falls behind the walk instead of in front of it.
            #
            #    The walk must also not already stand on the target: run_until
            #    resumes into a fetch of the PC it is parked on, so a machine
            #    sitting at &6201 lands immediately without going round at
            #    all. Step clear of it and say so, rather than assuming where
            #    the previous command left us -- under load the machine drifts
            #    between commands.
            ipc.send_command('step 2')
            _, regs = ipc.get_regs()
            if regs.get('PC') == 0x6201:
                ipc.send_command('step 1')
                _, regs = ipc.get_regs()
            if regs.get('PC') == 0x6201:
                print("FAIL: could not step clear of the target &6201")
                return False
            ok, resp = ipc.send_command('step to 0x6201')
            if ok or 'stopped-elsewhere' not in resp:
                print(f"FAIL: step to must report a breakpoint that fires "
                      f"before the target as an error, got: {resp}")
                return False
            print("PASS: wait pc and step to agree — own breakpoint at the "
                  "target is OK, an intervening stop is 409")
            return True
    finally:
        os.unlink(bin_path)


def test_wait_pc_aborts_on_shutdown():
    """A wait in flight must not hold the process open for its whole deadline.

    A `wait pc` runs on the IPC server's one command thread, which teardown
    joins, and the teardown before that pause-waits the Z80 the walk keeps
    resuming. The run-to-address walk only watched its own deadline, so
    quitting while a client had `wait pc <unreachable> 60000` outstanding
    could hold the exit for the rest of that minute; harnesses routinely use
    15-30s waits. The walk now takes an abort predicate (unit-tested in
    Z80RunUntilEphemeral.HonoursTheCallersAbortPredicate) and answers
    ERR 503 shutting-down.

    The assertion here is the one this level can make honestly: the process
    exits promptly with the wait outstanding. The 503 itself may not reach the
    client — teardown ends in _exit(), which can beat the reply onto the wire.
    """
    print("Running wait pc shutdown-abort test...")
    with EmulatorRunner() as emu:
        # The emulator ends its own run while the wait below is outstanding.
        if not emu.start('--exit-after=4s'):
            print("FAIL: Could not start emulator")
            return False
        assert emu.process is not None
        port = emu.ipc.port
        reply = {}

        def waiter():
            # &4321 is RAM the firmware never executes, so this wait can only
            # end at its deadline or by the shutdown abort.
            ipc = KoncepcjaIPC(port=port, timeout=90.0)
            reply['result'] = ipc.send_command('wait pc 0x4321 60000')

        thread = threading.Thread(target=waiter, daemon=True)
        thread.start()
        time.sleep(0.5)  # let the wait get in flight before the exit lands
        started = time.monotonic()
        try:
            emu.process.wait(timeout=30)
        except subprocess.TimeoutExpired:
            print("FAIL: the emulator did not exit while a wait pc was in "
                  "flight (its 60s deadline held the process open)")
            return False
        elapsed = time.monotonic() - started
        if elapsed > 15:
            print(f"FAIL: exit took {elapsed:.1f}s — the wait's deadline, "
                  f"not the shutdown, ended it")
            return False
        thread.join(timeout=10)
        ok, resp = reply.get('result', (False, ''))
        if ok:
            print(f"FAIL: the interrupted wait must not answer OK: {resp}")
            return False
        # A peer that _exit()s before the reply lands reads as EOF (an empty
        # reply) on POSIX, but Windows reports it as a reset (WSAECONNRESET).
        # Either way no reply arrived, which the docstring allows.
        if resp and ('10054' in resp or 'reset' in resp.lower()):
            resp = ''
        if resp and 'shutting-down' not in resp:
            print(f"FAIL: the aborted wait answered something other than "
                  f"ERR 503 shutting-down: {resp}")
            return False
        print(f"PASS: wait pc did not hold the exit; it took {elapsed:.1f}s")
        return True


def test_inject_launches_like_run():
    """-i/--inject must hand the program to the firmware the way RUN" does.

    bin_load() used to poke PC at the entry point behind the firmware's back
    (a faked BASIC return stack, no pack re-initialisation), which left a
    program running with the interrupt-driven keyboard scan dead: a game that
    reads keys through KM READ CHAR never saw a keypress (beads-scrl). The
    firmware's own launcher, MC START PROGRAM (&BD16), resets the stack, the
    packs and interrupts, then enters the program — so a 10-byte program that
    loops on KM READ CHAR and stores the character must see a tapped key.

        &6000  CALL &BB09      ; KM READ CHAR: carry set, A = char
        &6003  JR NC,&6000
        &6005  LD (&6100),A
        &6008  JR &6000
    """
    print("Running inject → MC START PROGRAM → firmware keyboard test...")
    program = bytes([0xCD, 0x09, 0xBB, 0x30, 0xFB, 0x32, 0x00, 0x61, 0x18, 0xF6])
    with tempfile.NamedTemporaryFile(suffix='.bin', delete=False) as f:
        f.write(program)
        bin_path = f.name
    try:
        with EmulatorRunner() as emu:
            if not emu.start('-i', bin_path, '-o', '0x6000'):
                print("FAIL: Could not start emulator")
                return False
            ipc = emu.ipc
            ipc.timeout = 20.0  # the server-side waits below run up to 15s
            # Injection happens once boot_time frames have elapsed; the program
            # then spins in &6000-&6009. wait pc pauses on the hit.
            ok, resp = ipc.send_command('wait pc 0x6000 15000')
            if not ok:
                _, regs = ipc.get_regs()
                print(f"FAIL: injected program never reached &6000: {resp} "
                      f"(PC={regs.get('PC', -1):04X})")
                return False
            ok, resp = ipc.read_mem(0x6100, 1)
            if not ok or not resp.endswith('00'):
                print(f"FAIL: &6100 must start clear, got {resp}")
                return False
            # Let the firmware settle first. `wait pc` now stops on the very
            # first instruction after MC START PROGRAM, and a key that is down
            # during the next 5-10 frames is never delivered, however it is
            # pressed (measured with an exact wait: a 5-frame settle misses
            # 2/2, 10 lands 2/2). That window is the program's start-up, not
            # the tap, so settle well clear of it.
            #
            # Exactly 50 frames (`wait vbl 50` now runs the same 50; it used
            # to be a wall-clock sleep that ran ~150, beads-71pg). This test
            # used to lose its key ~2% of the time
            # (beads-27a7): the breakpoint stop above opens the DevTools
            # Registers window, which pushed the registers into the machine
            # on every paused frame and cut the instruction in flight in half
            # (beads-vwwq, fixed in devtools_ui.cpp).
            ok, resp = ipc.send_command('step frame 50')
            if not ok:
                print(f"FAIL: step frame 50: {resp}")
                return False
            ipc.run()
            # The default tap, sent while the machine runs. It used to count
            # the frame already running when it pressed, so the key could be
            # released after a single scannable frame, or before the firmware
            # scanned it at all when the release raced the matrix publish; the
            # hold now counts only frames that began with the key down
            # (beads-cjej). This is that regression check.
            ok, resp = ipc.send_command('input key a')
            if not ok:
                print(f"FAIL: input key a: {resp}")
                return False
            # Only the firmware's ISR (KM SCAN KEYS every frame flyback) can
            # move that key into the buffer KM READ CHAR drains.
            ok, resp = ipc.send_command('wait mem 0x6100 0x61 5000')
            if not ok:
                print(f"FAIL: the key never reached the program through the "
                      f"firmware — interrupt/keyboard path dead after -i: {resp}")
                return False
            print("PASS: injected program received a key via KM READ CHAR")
            return True
    finally:
        os.unlink(bin_path)


def test_wait_bp_drops_a_hit_the_machine_has_run_past():
    """`wait bp` must not report a stop the machine has since been run out of.

    beads-1ew8: a breakpoint stops the machine and nobody collects the hit;
    `run` resumes it; `wait bp` then answered OK with that old hit while the
    machine was running. `bp add; run; wait bp; bp del` (the way tests stop a
    machine at &0038) could so end on a running machine.

    The program passes its breakpoint exactly once, so after the `run` there
    is no second hit that could make a stale answer look true.
    """
    print("Running wait bp vs. a hit from before a run test...")

    def paused(ipc) -> Optional[bool]:
        ok, resp = ipc.send_command('status')
        m = re.search(r'paused=([01])', resp)
        return (m.group(1) == '1') if ok and m else None

    with EmulatorRunner() as emu:
        if not emu.start():
            print("FAIL: Could not start emulator")
            return False
        ipc = emu.ipc
        ipc.pause()
        # 8000 DI : 8001 NOP : 8002 NOP : 8003 JR $
        if not ipc.write_mem(0x8000, 'F3000018FE'):
            print("FAIL: could not write the program")
            return False
        ok, resp = ipc.send_command('reg set PC 0x8000')
        if not ok:
            print(f"FAIL: reg set PC: {resp}")
            return False
        if not ipc.bp_add(0x8002):
            print("FAIL: Could not add breakpoint")
            return False
        ipc.run()
        # See the stop without collecting the hit.
        deadline = time.time() + 5.0
        while paused(ipc) is not True:
            if time.time() > deadline:
                print("FAIL: the breakpoint at 8002 never stopped the machine")
                return False
            time.sleep(0.01)
        ok, pc = ipc.get_reg('PC')
        if not ok or pc != 0x8002:
            print(f"FAIL: stopped at {pc:04X}, expected 8002")
            return False

        ipc.run()
        time.sleep(0.1)
        if paused(ipc) is not False:
            print("FAIL: the machine is not running after run (the breakpoint "
                  "fired twice: this program cannot test the latch)")
            return False

        ok, resp = ipc.wait_bp(timeout_ms=300)
        still_running = paused(ipc) is False
        if ok or '408' not in resp:
            print(f"FAIL: wait bp reported a hit from before the run: {resp!r} "
                  f"(machine running: {still_running})")
            return False
        if not still_running:
            print("FAIL: the machine stopped during a wait bp that timed out")
            return False

        # A hit under the current run is still reported, and is a real stop.
        ipc.pause()
        ipc.send_command('reg set PC 0x8000')
        ipc.run()
        ok, resp = ipc.wait_bp(timeout_ms=5000)
        if not ok or 'PC=8002' not in resp:
            print(f"FAIL: wait bp missed a fresh hit: {resp!r}")
            return False
        if paused(ipc) is not True:
            print("FAIL: wait bp answered OK on a running machine")
            return False

    print("PASS: wait bp drops a hit the machine has run past")
    return True


def test_breakpoint_pause_step_resume():
    """Breakpoint fires, emulator pauses, step in advances PC, resume works.

    In non-headless (threaded) mode this also exercises the deadlock fix:
    the Z80 thread must call signal_ready(true) after cpc_pause() so the
    render thread is not stuck in wait_ready() forever.  The wait_bp timeout
    acts as the deadlock detector — if the emulator is stuck, it times out.
    """
    print("Running breakpoint → pause → step → resume test...")

    with EmulatorRunner() as emu:
        if not emu.start():
            print("FAIL: Could not start emulator")
            return False

        mode = "threaded" if emu.ipc.is_threaded() else "headless"
        print(f"  Running in {mode} mode")

        # 0x0038 = RST 38h / interrupt handler — hit every ~50ms on a running CPC
        if not emu.ipc.bp_add(0x0038):
            print("FAIL: Could not add breakpoint")
            return False

        emu.ipc.run()  # ensure emulation is running

        # wait bp: blocks until breakpoint fires or 5s timeout.
        # A timeout here means the Z80 never reached the BP (or is deadlocked).
        ok, resp = emu.ipc.wait_bp(timeout_ms=5000)
        if not ok:
            print(f"FAIL: wait bp timed out or errored: {resp}")
            return False

        # Emulator should now be paused at the breakpoint
        ok2, pc_before = emu.ipc.get_reg('PC')
        if not ok2:
            print("FAIL: Could not read PC after breakpoint")
            return False
        print(f"  Breakpoint hit at PC=0x{pc_before:04X}")

        # Step one instruction — PC must advance
        ok3, step_resp = emu.ipc.step_in(1)
        if not ok3:
            print(f"FAIL: step in failed: {step_resp}")
            return False

        ok4, pc_after = emu.ipc.get_reg('PC')
        if not ok4:
            print("FAIL: Could not read PC after step")
            return False

        if pc_after == pc_before:
            print(f"FAIL: PC did not advance after step (stuck at 0x{pc_before:04X})")
            return False
        print(f"  After step: PC=0x{pc_after:04X} (+{pc_after - pc_before} bytes)")

        emu.ipc.bp_clear()
        emu.ipc.run()

        # Verify emulator is still alive after resume
        if not emu.ipc.ping():
            print("FAIL: Emulator became unresponsive after resume")
            return False

        print("PASS: Breakpoint → pause → step → resume test")
        return True


def test_snapshot_round_trip():
    """Save snapshot while paused, corrupt memory, load snapshot, verify restored.

    Exercises the pause-lease wait in the IPC server's snapshot save/load paths.
    Without going idle the snapshot might capture a partially-updated Z80 state.
    """
    print("Running snapshot round-trip test...")

    import tempfile, os

    with EmulatorRunner() as emu:
        if not emu.start():
            print("FAIL: Could not start emulator")
            return False

        # Let the ROM boot briefly
        if not emu.ipc.wait_vbl(10, timeout_ms=3000):
            print("FAIL: Could not wait for VBL")
            return False

        emu.ipc.pause()

        # Read original bytes at a known RAM address
        ok, orig_resp = emu.ipc.read_mem(0x4000, 4)
        if not ok:
            print("FAIL: Could not read memory before snapshot")
            return False
        print(f"  Original mem@0x4000: {orig_resp}")

        # Save snapshot to a temp file
        snap = tempfile.mktemp(suffix='.sna')
        try:
            if not emu.ipc.snapshot_save(snap):
                print("FAIL: snapshot save failed")
                return False

            # Overwrite those bytes with a known pattern
            emu.ipc.write_mem(0x4000, 'DEADBEEF')

            ok2, written_resp = emu.ipc.read_mem(0x4000, 4)
            if 'DE' not in written_resp.upper():
                print(f"FAIL: Memory write didn't stick: {written_resp}")
                return False

            # Load the snapshot — must restore the original bytes
            if not emu.ipc.snapshot_load(snap):
                print("FAIL: snapshot load failed")
                return False

            ok3, restored_resp = emu.ipc.read_mem(0x4000, 4)
            if not ok3:
                print("FAIL: Could not read memory after snapshot load")
                return False

            # Strip 'OK ' prefix for comparison
            orig_bytes = orig_resp.replace('OK ', '').strip()
            restored_bytes = restored_resp.replace('OK ', '').strip()

            if orig_bytes != restored_bytes:
                print(f"FAIL: Snapshot did not restore memory: expected {orig_bytes!r}, got {restored_bytes!r}")
                return False

            print(f"  Restored mem@0x4000: {restored_resp}")
            print("PASS: Snapshot round-trip test")
            return True
        finally:
            if os.path.exists(snap):
                os.unlink(snap)


def test_reg_set_halt_is_refused():
    """`reg set HALT` must not answer OK for an edit it cannot make.

    Whether the CPU is halted is where it is in an instruction, not a
    register. No path carries an edit of it into the machine, so the command
    used to answer `OK` while `reg get HALT` went on reading the old value
    (beads-gpx7). It is refused now, in both directions, and changes nothing.
    """
    print("Running reg set HALT test...")

    with EmulatorRunner() as emu:
        if not emu.start():
            print("FAIL: Could not start emulator")
            return False

        emu.ipc.pause()

        # DI : HALT : NOP. With interrupts off the CPU stays halted, so the
        # second half of the test has a machine that really is.
        for cmd in ['mem write 0x6000 F37600', 'reg set PC 0x6000']:
            ok, resp = emu.ipc.send_command(cmd)
            if not ok:
                print(f"FAIL: {cmd}: {resp}")
                return False

        def halt_is(want, when):
            ok, resp = emu.ipc.send_command('reg get HALT')
            if not ok or resp.split()[-1] != want:
                print(f"FAIL: HALT reads {resp!r} {when}, expected {want}")
                return False
            return True

        def refused(cmd):
            ok, resp = emu.ipc.send_command(cmd)
            if ok or not resp.startswith('ERR 400 reg-read-only'):
                print(f"FAIL: {cmd} answered {resp!r}, "
                      "expected ERR 400 reg-read-only")
                return False
            return True

        if not halt_is('00', 'before the HALT'):
            return False
        if not refused('reg set HALT 1'):
            return False
        if not halt_is('00', 'after a refused reg set HALT 1'):
            return False

        ok, resp = emu.ipc.send_command('step 2')
        if not ok:
            print(f"FAIL: step 2: {resp}")
            return False
        if not halt_is('01', 'after executing DI : HALT'):
            return False
        if not refused('reg set HALT 0'):
            return False
        if not halt_is('01', 'after a refused reg set HALT 0'):
            return False

    print("PASS: reg set HALT is refused and changes nothing")
    return True


def test_rapid_pause_resume():
    """20 rapid pause/resume cycles without deadlock.

    In threaded mode, each cpc_resume() wakes the Z80 thread (within 1ms)
    and the next cpc_pause() must not race with signal_ready/wait_consumed.
    A timeout on any command means the emulator deadlocked.
    """
    print("Running rapid pause/resume test...")

    CYCLES = 20

    with EmulatorRunner() as emu:
        if not emu.start():
            print("FAIL: Could not start emulator")
            return False

        for i in range(CYCLES):
            ok_p, _ = emu.ipc.send_command('pause')
            ok_r, _ = emu.ipc.send_command('run')
            if not ok_p or not ok_r:
                print(f"FAIL: pause/resume failed on cycle {i+1}")
                return False

        # Final check — still alive
        if not emu.ipc.ping():
            print("FAIL: Emulator unresponsive after rapid pause/resume")
            return False

        print(f"PASS: {CYCLES} rapid pause/resume cycles without deadlock")
        return True


def test_step_in_accuracy():
    """Pause, plant a known instruction run, step it, verify PC advances.

    Exercises the pause-lease wait in the IPC step-in path: if the Z80 thread
    were still inside z80_execute() when step_in ran, PC would not advance
    predictably.

    Deliberately steps a PLANTED run of NOPs rather than whatever boot code
    happens to be live. The previous version paused at an arbitrary point and
    demanded PC change on every single step, which is simply false for Z80
    block instructions -- LDIR/LDDR/OTIR re-execute at the SAME PC once per
    iteration until BC hits 0. The CPC firmware boots through big LDIR block
    copies, so that test failed on most runs (measured 3 of 4 on master, at
    0x0642 and 0x0B2C -- both `ldir`) while the stepping code was perfectly
    correct.
    """
    print("Running step-in accuracy test...")

    STEPS = 10

    with EmulatorRunner() as emu:
        if not emu.start():
            print("FAIL: Could not start emulator")
            return False

        emu.ipc.pause()

        # 16 NOPs at 0x6000: one byte each, so PC must advance by exactly 1.
        ok, resp = emu.ipc.send_command('mem write 0x6000 ' + '00' * 16)
        if not ok:
            print(f"FAIL: could not plant NOPs: {resp}")
            return False
        if not emu.ipc.send_command('reg set PC 0x6000')[0]:
            print("FAIL: could not set PC")
            return False

        for i in range(STEPS):
            ok_s, _ = emu.ipc.step_in(1)
            if not ok_s:
                print(f"FAIL: step_in failed on step {i+1}")
                return False
            ok_r, cur_pc = emu.ipc.get_reg('PC')
            if not ok_r:
                print(f"FAIL: Could not read PC after step {i+1}")
                return False
            want = 0x6000 + i + 1
            if cur_pc != want:
                # An interrupt vectoring away mid-run is legitimate; a PC that
                # simply failed to move is the bug this test is about.
                if cur_pc == 0x6000 + i:
                    print(f"FAIL: PC stuck at 0x{cur_pc:04X} after step {i+1}")
                    return False
                print(f"  interrupt took PC to 0x{cur_pc:04X}; re-seating")
                emu.ipc.send_command(f'reg set PC 0x{want:04X}')

        print(f"  PC advanced one NOP at a time across {STEPS} steps")
        print("PASS: Step-in accuracy test")
        return True


def test_step_out_nested_call():
    """Step Out finishes the current frame while skipping a nested CALL."""
    print("Running step-out nested-call test...")

    with EmulatorRunner() as emu:
        if not emu.start():
            print("FAIL: Could not start emulator")
            return False

        if not emu.ipc.pause():
            print("FAIL: Could not pause emulator")
            return False

        # 6000: CALL 6006; NOP; RET; padding; 6006: NOP; RET
        setup = [
            'mem write 0x6000 CD066000C90000C9',
            'mem write 0x8000 0070',
            'reg set SP 0x8000',
            'reg set PC 0x6000',
        ]
        for command in setup:
            ok, resp = emu.ipc.send_command(command)
            if not ok:
                print(f"FAIL: {command!r} failed: {resp}")
                return False

        # The walk single-steps through the restart handler, so give the
        # client more than its default 5s -- otherwise a slow-but-correct walk
        # looks like a failure.
        emu.ipc.timeout = 30.0
        ok, resp = emu.ipc.send_command('step out')
        if not ok:
            print(f"FAIL: step out failed: {resp}")
            return False

        ok_pc, pc = emu.ipc.get_reg('PC')
        ok_sp, sp = emu.ipc.get_reg('SP')
        if not ok_pc or not ok_sp or pc != 0x7000 or sp != 0x8002:
            print(
                f"FAIL: expected PC=7000 SP=8002, got "
                f"PC={pc:04X} SP={sp:04X}")
            return False

        print("PASS: Step Out skipped nested CALL and unwound one frame")
        return True


def test_step_out_gated_on_ret_not_sp():
    """Step Out must finish on the RET, not on the first POP that lifts SP.

    The ordinary Z80 subroutine saves a register on entry and restores it
    just before returning:

        PUSH HL / <body> / POP HL / RET

    Issue `step out` inside <body> and the entry SP is the POST-push value,
    so `POP HL` alone raises SP above it. A bare SP-threshold test ends the
    walk there -- one instruction early, PC still on the RET, still inside
    the callee. This pins the RET gate that prevents that.
    """
    print("Running step-out RET-gate test...")

    with EmulatorRunner() as emu:
        if not emu.start():
            print("FAIL: Could not start emulator")
            return False

        if not emu.ipc.pause():
            print("FAIL: Could not pause emulator")
            return False

        # Mid-body of a routine that already did `PUSH HL`:
        #   6000: NOP        <- step out issued here
        #   6001: POP HL     <- raises SP to 8002, ABOVE the 8000 entry SP
        #   6002: RET        <- the real frame exit, to 7000
        # Stack: 8000 = saved HL (1234), 8002 = return address (7000).
        setup = [
            'mem write 0x6000 00E1C9',
            'mem write 0x8000 34120070',
            'reg set SP 0x8000',
            'reg set PC 0x6000',
        ]
        for command in setup:
            ok, resp = emu.ipc.send_command(command)
            if not ok:
                print(f"FAIL: {command!r} failed: {resp}")
                return False

        ok, resp = emu.ipc.send_command('step out')
        if not ok:
            print(f"FAIL: step out failed: {resp}")
            return False

        ok_pc, pc = emu.ipc.get_reg('PC')
        ok_sp, sp = emu.ipc.get_reg('SP')
        if not ok_pc or not ok_sp:
            print("FAIL: could not read back PC/SP")
            return False
        if pc == 0x6002:
            print(
                "FAIL: stopped at the POP, not the RET -- PC=6002 SP="
                f"{sp:04X} (the SP-threshold regression)")
            return False
        if pc != 0x7000 or sp != 0x8004:
            print(
                f"FAIL: expected PC=7000 SP=8004, got PC={pc:04X} SP={sp:04X}")
            return False

        # HL must carry the POP'd value: the walk really executed the body.
        ok_hl, hl = emu.ipc.get_reg('HL')
        if not ok_hl or hl != 0x1234:
            print(f"FAIL: expected HL=1234 after POP, got {hl:04X}")
            return False

        print("PASS: Step Out ran through the POP and finished on the RET")
        return True


def pc_is_moving(ipc, timeout_s=3.0):
    """Poll `reg get PC` until it changes. Returns (moved, first, last).

    A single before/after pair is NOT a liveness test. The firmware idles in
    a short loop, so two samples taken 0.4s apart can land on the same PC
    while the Z80 is running flat out -- the sample interval says nothing
    about where in the loop each read happens. That aliasing failed
    test_profile_load_missing_keeps_running on the macOS CI runner with both
    reads returning 1BC5, while the machine was demonstrably running.

    Polling keeps the original intent -- prove the CPU advances -- and only
    reports frozen after the whole budget has elapsed with no movement, which
    a genuinely paused machine always does.
    """
    ok, first = ipc.send_command('reg get PC')
    if not ok:
        return False, first, first
    last = first
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        time.sleep(0.05)
        ok, cur = ipc.send_command('reg get PC')
        if not ok:
            return False, first, cur
        last = cur
        if cur != first:
            return True, first, cur
    return False, first, last


def make_test_rom(handlers=None):
    """Build a synthetic 32K system ROM and return the directory holding it.

    The CPC maps the lower ROM over 0x0000-0x3FFF, so the eight Z80 restart
    vectors (&00-&38) are covered by firmware and `mem write` there lands in
    RAM the CPU never executes -- a handler simply cannot be planted at a
    restart vector on a normally-booted machine. Supplying our own system ROM
    sidesteps that entirely: no unmapping, no new IPC command, just a ROM with
    no firmware in it. `rom_path` is only a directory (CPC.rom_path + "/" +
    chROMFile[model]), so pointing it at a temp dir is enough.

    `handlers` maps a ROM offset to the bytes to place there, e.g.
    {0x0030: bytes([0xD1, 0xC3, 0x04, 0x60])} for a restart that discards its
    return address and jumps away. Filler is 0xFF (rst 38h) so stray execution
    is obvious rather than silently sliding through NOPs.

    Caller owns the returned directory; shutil.rmtree it when done.
    """
    rom = bytearray(b'\xFF' * 32768)      # OS half + BASIC half
    # Park at a NON-ZERO address: EmulatorRunner treats PC==0 as "not ready
    # yet", so a loop at 0x0000 would sit exactly on that sentinel.
    rom[0x0000:0x0003] = bytes([0xC3, 0x00, 0x01])  # 0000: JP 0100
    rom[0x0100:0x0103] = bytes([0xC3, 0x00, 0x01])  # 0100: JP 0100 (park)
    # &FF filler decodes as `rst 38h`, so 0x0038 would otherwise recurse into
    # itself and run the stack down. A bare RET keeps stray execution bounded.
    rom[0x0038] = 0xC9
    # Overwriting any of these silently removes the scaffolding: clobber the
    # park loop and the machine never reaches a steady PC, so start() waits
    # out its full timeout and then blames the ROM for not loading.
    reserved = set(range(0x0000, 0x0003)) | {0x0038} | set(
        range(0x0100, 0x0103))
    for offset, code in (handlers or {}).items():
        assert 0 <= offset and offset + len(code) <= len(rom), (
            f"handler at {offset:#06x} ({len(code)} bytes) does not fit a 32K "
            f"ROM")
        clash = reserved & set(range(offset, offset + len(code)))
        assert not clash, (
            f"handler at {offset:#06x} ({len(code)} bytes) overlaps ROM "
            f"scaffolding at {sorted(hex(a) for a in clash)} -- the reset "
            f"vector, the park loop at 0x0100 or the 0x0038 guard")
        rom[offset:offset + len(code)] = code
    assert len(rom) == 32768, "a slice assignment resized the ROM"
    d = tempfile.mkdtemp(prefix='koncpc_testrom_')
    try:
        with open(os.path.join(d, 'cpc6128.rom'), 'wb') as f:
            f.write(rom)
    except OSError:
        shutil.rmtree(d, ignore_errors=True)
        raise
    return d


def test_step_out_tail_jumping_restart():
    """A restart that discards its return address must not strand the walk.

    The CPC's LOW JUMP (&08) and FIRM JUMP (&28) push a return address, then
    the handler POPs it, reads its inline operand and JUMPS away -- the return
    never happens. Counting entered frames cannot express that: the restart's
    entry is never balanced, so step out finishes a frame too high. Tracking
    each entered frame by its return SLOT does, because the entry retires when
    the stack rises past it however that happens.

    Runs against a synthetic ROM so the restart handler is OURS. An earlier
    attempt at this test planted the handler with `mem write` and passed for
    entirely the wrong reason -- the firmware's own handler happened to return.
    """
    print("Running step-out tail-jumping-restart test...")

    romdir = make_test_rom({
        # 0030: POP DE (discard the pushed return address) / JP 6004
        0x0030: bytes([0xD1, 0xC3, 0x04, 0x60]),
    })
    try:
        with EmulatorRunner() as emu:
            if not emu.start('-O', f'rom.rom_path={romdir}',
                             '-O', 'system.model=2'):
                print("FAIL: emulator would not start with the test ROM")
                return False
            emu.ipc.timeout = 30.0
            if not emu.ipc.pause():
                print("FAIL: Could not pause emulator")
                return False

            # Prove the CPU really sees our handler, not firmware. Without this
            # the test can pass for the wrong reason, which is exactly how the
            # previous version of it fooled us.
            ok, dis = emu.ipc.send_command('disasm 0x0030 2')
            low = dis.lower()
            if not ok or 'pop de' not in low or 'jp $6004' not in low:
                print(f"FAIL: 0x0030 is not our handler: {dis.strip()!r}")
                return False

            #   6000: RST 30h   -> pushes 6001, vectors to our handler
            #   6004: RET       -> returns to 7000 via OUR frame's slot
            # Stack: 8000 = 7000 (this frame's return address)
            setup = [
                'mem write 0x6000 F7000000C9',
                'mem write 0x8000 0070',
                'reg set SP 0x8000',
                'reg set PC 0x6000',
            ]
            for command in setup:
                ok, resp = emu.ipc.send_command(command)
                if not ok:
                    print(f"FAIL: {command!r} failed: {resp}")
                    return False

            ok, resp = emu.ipc.send_command('step out')
            if not ok:
                print(f"FAIL: step out failed: {resp}")
                return False

            ok_pc, pc = emu.ipc.get_reg('PC')
            ok_sp, sp = emu.ipc.get_reg('SP')
            if not ok_pc or not ok_sp or pc != 0x7000 or sp != 0x8002:
                print(f"FAIL: expected PC=7000 SP=8002, "
                      f"got PC={pc:04X} SP={sp:04X}")
                return False

            # The handler's POP DE lifted the pushed return address into DE.
            # Without this, nothing distinguishes "the handler ran and threw
            # its return address away" from "the walk reached the right PC by
            # some other route".
            ok_de, de = emu.ipc.get_reg('DE')
            if not ok_de or de != 0x6001:
                print(f"FAIL: handler's POP DE did not run "
                      f"(DE={de:04X}, expected 6001)")
                return False

            print("PASS: tail-jumping restart did not strand the walk")
            return True
    finally:
        shutil.rmtree(romdir, ignore_errors=True)


def test_step_out_nested_restarts():
    """Two restarts deep, entered mid-frame, must both unwind.

    This is the test that requires entered frames to be a STACK of slots
    rather than one slot. Its partner, test_step_out_tail_jumping_restart,
    covers the other half; between them both wrong shapes die. Verified by
    mutating src/z80_view.cpp and re-running:

        mutation                              nested      tail-jumping
        keep only the newest entered slot     FAIL        (passes)
        pre-PR depth counter, no pruning      (passes)    FAIL (times out)

    So this test alone does NOT rule out the counter -- do not read it as a
    regression guard for that bug. What it does rule out is dropping an outer
    frame when an inner one is entered.

    The mid-frame entry is what gives it teeth. Entered at the top of the
    frame, it stays green under every mutation above, because each restart's
    RET lands exactly at entry_sp and the unwound() conjunct rejects it
    without consulting the slots at all. The leading POP puts entry_sp BELOW
    both restart slots, so a dropped slot yields a RET that does satisfy
    unwound() and the walk stops early at 0x6002.
    """
    print("Running step-out nested-restarts test...")

    # TWO DIFFERENT restarts, not one calling itself -- RST 30h at 0x0030
    # would recurse forever and run the stack into the ground.
    romdir = make_test_rom({
        0x0028: bytes([0xC9]),        # 0028: RET
        0x0030: bytes([0xEF, 0xC9]),  # 0030: RST 28h, then RET
    })
    try:
        with EmulatorRunner() as emu:
            if not emu.start('-O', f'rom.rom_path={romdir}',
                             '-O', 'system.model=2'):
                print("FAIL: emulator would not start with the test ROM")
                return False
            emu.ipc.timeout = 30.0
            if not emu.ipc.pause():
                print("FAIL: Could not pause emulator")
                return False

            # Both vectors must be OURS. &28 in particular is a real firmware
            # restart (FIRM JUMP), so if the ROM override were silently
            # ignored this test would run Amstrad's code and could pass for
            # the wrong reason -- the exact failure that got an earlier
            # version of the sibling test deleted.
            # Exact mnemonics, not substrings: the 0xFF filler disassembles
            # as `rst 38h`, which would satisfy a bare 'rst' check and let an
            # ignored ROM override pass for the wrong reason.
            for addr, want in ((0x0028, 'ret'), (0x0030, 'rst 28h')):
                ok, dis = emu.ipc.send_command(f'disasm 0x{addr:04X} 1')
                if not ok or want not in dis.lower():
                    print(f"FAIL: 0x{addr:04X} is not our handler: "
                          f"{dis.strip()!r}")
                    return False

            # Enter MID-frame, like a real step out does. Without the POP
            # the whole test passes even with frame tracking deleted, because
            # the stack-level conjunct carries it: the outer handler's RET
            # lands exactly at entry_sp and is rejected anyway. With the POP,
            # entry_sp sits BELOW the restart slots, so losing a slot makes
            # the walk finish early at 0x6002 instead of the caller.
            #   6000: POP HL   -> SP 8000 -> 8002
            #   6001: RST 30h  -> 0030 RST 28h -> 0028 RET -> 0031 RET
            #   6002: RET      -> the frame exit, to 7000
            for command in ['mem write 0x6000 E1F7C9',
                            'mem write 0x8000 34120070',
                            'reg set SP 0x8000', 'reg set PC 0x6000']:
                ok, resp = emu.ipc.send_command(command)
                if not ok:
                    print(f"FAIL: {command!r} failed: {resp}")
                    return False

            ok, resp = emu.ipc.send_command('step out')
            if not ok:
                print(f"FAIL: step out failed: {resp}")
                return False

            ok_pc, pc = emu.ipc.get_reg('PC')
            ok_sp, sp = emu.ipc.get_reg('SP')
            if not ok_pc or not ok_sp:
                print("FAIL: could not read back PC/SP")
                return False
            if pc == 0x6002:
                print("FAIL: finished inside the frame -- a restart slot was "
                      "lost")
                return False
            if pc != 0x7000 or sp != 0x8004:
                print(f"FAIL: expected PC=7000 SP=8004, "
                      f"got PC={pc:04X} SP={sp:04X}")
                return False

            print("PASS: nested restarts unwound to the caller")
            return True
    finally:
        shutil.rmtree(romdir, ignore_errors=True)


def _load_frame(emu, code_hex, stack_hex="34120070", extra=None):
    """Plant a routine at 0x6000 inside a frame whose return address is 0x7000.

    Stack: 8000 = a saved register pair, 8002 = the return address. SP starts
    at 8000, i.e. mid-frame, AFTER the routine's entry PUSH -- the position
    that breaks a naive stack-pointer threshold.
    """
    for command in ['mem write 0x6000 ' + code_hex,
                    'mem write 0x8000 ' + stack_hex,
                    'reg set SP 0x8000',
                    'reg set PC 0x6000'] + (extra or []):
        ok, resp = emu.ipc.send_command(command)
        if not ok:
            print(f"FAIL: {command!r} failed: {resp}")
            return False
    return True


def test_step_out_untaken_conditional_ret():
    """An untaken RET cc must NOT end the walk.

    This is the case that defeated the first RET gate. That gate asked two
    independent questions -- "is this a RET-class opcode?" and "is SP above
    where we started?" -- and an untaken `RET NZ` answers yes to both the
    moment an earlier POP has lifted SP. Reproduced then: step out stopped at
    0x6002, on the untaken RET NZ, still inside the frame.
    """
    print("Running step-out untaken-RET-cc test...")

    with EmulatorRunner() as emu:
        if not emu.start():
            print("FAIL: Could not start emulator")
            return False
        if not emu.ipc.pause():
            print("FAIL: Could not pause emulator")
            return False

        #   6000: POP HL   -> SP 8000 -> 8002, ABOVE the entry SP
        #   6001: RET NZ   -> NOT taken (Z set): moves no stack at all
        #   6002: RET      -> the real frame exit, to 7000
        if not _load_frame(emu, 'E1C0C9', extra=['reg set F 0x40']):
            return False

        ok, resp = emu.ipc.send_command('step out')
        if not ok:
            print(f"FAIL: step out failed: {resp}")
            return False

        ok_pc, pc = emu.ipc.get_reg('PC')
        ok_sp, sp = emu.ipc.get_reg('SP')
        if not ok_pc or not ok_sp:
            print("FAIL: could not read back PC/SP")
            return False
        if pc == 0x6002:
            print("FAIL: stopped on the untaken RET NZ, still inside the frame")
            return False
        if pc != 0x7000 or sp != 0x8004:
            print(f"FAIL: expected PC=7000 SP=8004, got PC={pc:04X} SP={sp:04X}")
            return False

        print("PASS: untaken RET cc did not end the walk")
        return True


def test_step_out_pop_then_call():
    """A POP before a CALL must not end the walk when the callee is skipped.

    The walk once carried a `depth == 0 && unwound()` backstop after a
    callee skip, for a hypothetical callee that destroys the stack. A plain
    POP earlier in the frame lifts SP above the entry level, so the very next
    CALL-skip satisfied it and step out reported OK at the RET -- inside the
    frame. Third variant of the same bug: any exit that fires without seeing a
    taken return is it.
    """
    print("Running step-out POP-then-CALL test...")

    with EmulatorRunner() as emu:
        if not emu.start():
            print("FAIL: Could not start emulator")
            return False
        if not emu.ipc.pause():
            print("FAIL: Could not pause emulator")
            return False

        #   6000: POP HL     -> SP 8000 -> 8002, above the entry SP
        #   6001: CALL 6005  -> skipped at full speed
        #   6004: RET        -> the real frame exit, to 7000
        #   6005: RET        -> the callee
        if not _load_frame(emu, 'E1CD0560C9C9'):
            return False

        ok, resp = emu.ipc.send_command('step out')
        if not ok:
            print(f"FAIL: step out failed: {resp}")
            return False

        ok_pc, pc = emu.ipc.get_reg('PC')
        ok_sp, sp = emu.ipc.get_reg('SP')
        if not ok_pc or not ok_sp:
            print("FAIL: could not read back PC/SP")
            return False
        if pc == 0x6004:
            print("FAIL: backstop fired after the CALL skip, still in frame")
            return False
        if pc != 0x7000 or sp != 0x8004:
            print(f"FAIL: expected PC=7000 SP=8004, got PC={pc:04X} SP={sp:04X}")
            return False

        print("PASS: POP before CALL did not end the walk early")
        return True


def test_step_out_computed_return():
    """`POP HL : JP (HL)` is a return, and must end the walk."""
    print("Running step-out computed-return test...")

    with EmulatorRunner() as emu:
        if not emu.start():
            print("FAIL: Could not start emulator")
            return False
        if not emu.ipc.pause():
            print("FAIL: Could not pause emulator")
            return False

        #   6000: POP HL   -> HL = 7000 (the return address), SP 8000 -> 8002
        #   6001: JP (HL)  -> jumps to the word just popped: a return
        # Stack: 8000 = 7000 so the POP lifts the return address into HL.
        if not _load_frame(emu, 'E1E9', stack_hex='00700070'):
            return False

        ok, resp = emu.ipc.send_command('step out')
        if not ok:
            print(f"FAIL: step out failed: {resp}")
            return False

        ok_pc, pc = emu.ipc.get_reg('PC')
        if not ok_pc or pc != 0x7000:
            print(f"FAIL: expected PC=7000 after POP HL:JP (HL), got {pc:04X}")
            return False

        print("PASS: computed return ended the walk at the caller")
        return True


def test_step_out_never_returns_times_out_honestly():
    """A frame that can never return must report a plain 408, not a false OK.

    Deciding whether a frame WILL return is undecidable, so step out does not
    guess: it runs its deadline and says it timed out. What it must never do is
    report success at some arbitrary address it happened to stop at.
    """
    print("Running step-out never-returns test...")

    with EmulatorRunner() as emu:
        if not emu.start():
            print("FAIL: Could not start emulator")
            return False
        if not emu.ipc.pause():
            print("FAIL: Could not pause emulator")
            return False

        # 6000: DI / 6001: JP $6001 -- spins forever, never returns, never
        # calls. Interrupts are disabled so the walk is testing THIS frame and
        # not the firmware's interrupt handler, whose frames interleave with
        # ours in a way that is timing-dependent (see beads: interrupt frames
        # serviced invisibly during a full-speed CALL skip).
        if not _load_frame(emu, 'F3C30160'):
            return False

        emu.ipc.timeout = 30.0  # the walk runs its full deadline by design
        ok, resp = emu.ipc.send_command('step out')
        if 'ERR 408' not in resp:
            print(f"FAIL: expected ERR 408 timeout, got: {resp.strip()}")
            return False

        print("PASS: non-returning frame timed out honestly")
        return True


def test_step_out_stops_at_breakpoint_inside_own_frame():
    """A breakpoint in the frame being stepped out of must stop the walk.

    The stepped path is probe-blind, so without an explicit check a breakpoint
    here is walked straight through -- while the identical breakpoint inside a
    skipped callee stops the command.
    """
    print("Running step-out breakpoint-inside-frame test...")

    with EmulatorRunner() as emu:
        if not emu.start():
            print("FAIL: Could not start emulator")
            return False
        if not emu.ipc.pause():
            print("FAIL: Could not pause emulator")
            return False

        # 6000: NOP / 6001: NOP / 6002: RET, breakpoint on the second NOP.
        if not _load_frame(emu, '000000C9'):
            return False
        emu.ipc.send_command('bp clear')
        ok, resp = emu.ipc.send_command('bp add 0x6001')
        if not ok:
            print(f"FAIL: bp add failed: {resp}")
            return False

        ok, resp = emu.ipc.send_command('step out')
        emu.ipc.send_command('bp clear')
        if 'breakpoint-hit' not in resp:
            print(f"FAIL: expected breakpoint-hit, got: {resp.strip()}")
            return False

        ok_pc, pc = emu.ipc.get_reg('PC')
        if not ok_pc or pc != 0x6001:
            print(f"FAIL: expected to stop at PC=6001, got {pc:04X}")
            return False

        print("PASS: breakpoint inside the stepped frame stopped the walk")
        return True


def test_step_out_breakpoint_stop_is_one_arrival():
    """A breakpoint the step-out walk steps onto is hit once (beads-w88g).

    The walk stopped the CPU before that opcode's fetch, where a probe stop
    leaves it after the fetch, so the next run fetched it again and stopped
    at the same breakpoint a second time.
    """
    print("Running step-out breakpoint-arrives-once test...")

    with EmulatorRunner() as emu:
        if not emu.start():
            print("FAIL: Could not start emulator")
            return False
        if not emu.ipc.pause():
            print("FAIL: Could not pause emulator")
            return False

        # 6003: JR $ / 6006: NOP / 6007: NOP / 6008: RET, returning to 6003.
        setup = ['mem write 0x6003 18FE00000000C9',
                 'mem write 0x8000 0360',
                 'reg set SP 0x8000',
                 'reg set PC 0x6006',
                 'bp clear',
                 'bp add 0x6007']
        for command in setup:
            ok, resp = emu.ipc.send_command(command)
            if not ok:
                print(f"FAIL: {command!r} failed: {resp}")
                emu.ipc.send_command('bp clear')
                return False

        ok, resp = emu.ipc.send_command('step out')
        if 'breakpoint-hit' not in resp:
            emu.ipc.send_command('bp clear')
            print(f"FAIL: expected breakpoint-hit, got: {resp.strip()}")
            return False
        ok, resp = emu.ipc.send_command('step frame 2')
        ok_pc, pc = emu.ipc.get_reg('PC')
        emu.ipc.send_command('bp clear')
        if not ok or not ok_pc or not 0x6003 <= pc <= 0x6005:
            print(f"FAIL: the run from the stop should reach the JR $ at "
                  f"6003, got {resp.strip()!r} PC={pc:04X}")
            return False

        print("PASS: the run from a stepped-onto breakpoint carries on")
        return True


def test_step_out_stops_at_real_breakpoint_on_landing_address():
    """A real breakpoint sitting on Step Out's ephemeral landing address
    must win and be reported as a breakpoint hit, not silently absorbed as
    a clean Step Out landing.

    z80_probe_exec_should_break() checks NORMAL breakpoints before
    EPHEMERAL ones at the same address and flags user_breakpoint_fired so
    process_probe_hit() records that a real breakpoint -- not the
    ephemeral -- caused the stop. The existing unit test for that predicate
    (z80_probe_filter_test.cpp) calls it directly, bypassing the ephemeral
    lifecycle entirely; this exercises the real `step out` path end to end.
    """
    print("Running step-out vs real-breakpoint collision test...")

    with EmulatorRunner() as emu:
        if not emu.start():
            print("FAIL: Could not start emulator")
            return False

        if not emu.ipc.pause():
            print("FAIL: Could not pause emulator")
            return False

        # 6000: CALL 6006; NOP; RET; padding; 6006: NOP; RET
        setup = [
            'mem write 0x6000 CD066000C90000C9',
            'mem write 0x8000 0070',
            'reg set SP 0x8000',
            'reg set PC 0x6000',
            'bp add 0x6003',  # exactly Step Out's ephemeral landing address
        ]
        for command in setup:
            ok, resp = emu.ipc.send_command(command)
            if not ok:
                print(f"FAIL: {command!r} failed: {resp}")
                emu.ipc.send_command('bp clear')
                return False

        ok, resp = emu.ipc.send_command('step out')
        if not ok or 'breakpoint-hit' not in resp:
            print(
                "FAIL: expected a reported breakpoint hit when a real "
                f"breakpoint sits on the landing address, got {resp.strip()!r}")
            emu.ipc.send_command('bp clear')
            return False

        ok_pc, pc = emu.ipc.get_reg('PC')
        emu.ipc.send_command('bp clear')
        if not ok_pc or pc != 0x6003:
            print(f"FAIL: expected PC=6003 (the real breakpoint), got "
                  f"PC={pc:04X}" if ok_pc else "FAIL: could not read PC")
            return False

        print("PASS: real breakpoint at the landing address wins over Step Out")
        return True


def test_mouse_input():
    """IPC mouse input: device gating + full command surface.

    Verifies the 'input mouse' command layer:
      - rejected when no mouse device is enabled,
      - move/button/buttons accepted once the AMX mouse is enabled,
      - malformed sub-commands are rejected.
    Actual pointer motion is exercised by the SDL path; this asserts the IPC
    contract (which is all the server is responsible for).
    """
    print("Running mouse input test...")

    # 1. No mouse device -> commands rejected.
    with EmulatorRunner() as emu:
        if not emu.start():
            print("FAIL: Could not start emulator")
            return False
        ok, resp = emu.ipc.send_command('input mouse move 5 5')
        if ok:
            print(f"FAIL: expected ERR with no mouse device, got OK: {resp!r}")
            return False
        print(f"  no-device rejected as expected: {resp.strip()}")

    # 2. AMX mouse enabled -> full surface works, bad args rejected.
    with EmulatorRunner() as emu:
        if not emu.start('-O', 'input.amx_mouse=1'):
            print("FAIL: Could not start emulator with AMX mouse")
            return False

        checks = [
            ('input mouse move 10 -4', True),
            ('input mouse button L down', True),
            ('input mouse button R up', True),
            ('input mouse buttons 0', True),
            ('input mouse button X down', False),  # bad button
            ('input mouse wiggle', False),         # bad sub-command
            ('input mouse move 1', False),         # missing dy
        ]
        for cmd, want_ok in checks:
            ok, resp = emu.ipc.send_command(cmd)
            if ok != want_ok:
                print(f"FAIL: {cmd!r} -> ok={ok} (wanted {want_ok}): {resp.strip()}")
                return False
            print(f"  {cmd!r} -> {resp.strip()}")

        print("PASS: mouse input test")
        return True


def test_gun_input():
    """IPC light-gun input: device gating + full command surface.

    Verifies the 'input gun' command layer (IPC Phase 2, beads-vrsr):
      - rejected when no phazer type is enabled,
      - move/trigger accepted once a phazer is enabled (input.lightgun),
      - malformed sub-commands are rejected.
    The aim mapping and LPEN latch live in the light_gun Device (covered by the
    LightGun gtest suite); this asserts the IPC command contract end-to-end.
    """
    print("Running gun input test...")

    # 1. No phazer type -> commands rejected.
    with EmulatorRunner() as emu:
        if not emu.start():
            print("FAIL: Could not start emulator")
            return False
        ok, resp = emu.ipc.send_command('input gun move 100 100')
        if ok:
            print(f"FAIL: expected ERR with no light gun, got OK: {resp!r}")
            return False
        print(f"  no-gun rejected as expected: {resp.strip()}")

    # 2. Phazer enabled -> full surface works, bad args rejected.
    with EmulatorRunner() as emu:
        if not emu.start('-O', 'input.lightgun=1'):
            print("FAIL: Could not start emulator with light gun")
            return False

        checks = [
            ('input gun move 100 60', True),
            ('input gun trigger down', True),
            ('input gun trigger up', True),
            ('input gun trigger sideways', False),  # bad trigger state
            ('input gun wiggle', False),            # bad sub-command
            ('input gun move 1', False),            # missing y
            ('input gun', False),                   # missing sub-command
        ]
        for cmd, want_ok in checks:
            ok, resp = emu.ipc.send_command(cmd)
            if ok != want_ok:
                print(f"FAIL: {cmd!r} -> ok={ok} (wanted {want_ok}): {resp.strip()}")
                return False
            print(f"  {cmd!r} -> {resp.strip()}")

        print("PASS: gun input test")
        return True


def test_chord_hold_input():
    """IPC key hold + chord: 'input key [hold=N]' and 'input chord' (Phase 3).

    Verifies the hold=<frames> modifier on 'input key' and the atomic modified
    tap of 'input chord' (beads-nz0n):
      - default and custom hold accepted, hold<1 / bad numbers / stray args rejected,
      - chord with modifiers accepted, modifier-only / unknown-modifier / empty
        chords rejected.
    The atomicity (all rows down in one write) is asserted by the gtest; this
    checks the IPC command contract end-to-end.
    """
    print("Running chord/hold input test...")

    with EmulatorRunner() as emu:
        if not emu.start():
            print("FAIL: Could not start emulator")
            return False

        checks = [
            ('input key RETURN', True),           # default 2-frame tap
            ('input key RETURN hold=5', True),    # custom hold
            ('input key RETURN hold=1', True),    # minimum hold
            ('input key RETURN hold=0', False),   # hold must be >= 1
            ('input key RETURN hold=xyz', False), # not a number
            ('input key RETURN foo=5', False),    # unknown extra arg
            ('input chord CTRL+SHIFT+ESC', True),
            ('input chord SHIFT+A hold=3', True),
            ('input chord ESC', True),            # degenerate chord = plain tap
            ('input chord CTRL+SHIFT', False),    # ends in a modifier
            ('input chord CTRL+FOO+ESC', False),  # unknown modifier
            ('input chord', False),               # missing chord
        ]
        for cmd, want_ok in checks:
            ok, resp = emu.ipc.send_command(cmd)
            if ok != want_ok:
                print(f"FAIL: {cmd!r} -> ok={ok} (wanted {want_ok}): {resp.strip()}")
                return False
            print(f"  {cmd!r} -> {resp.strip()}")

        print("PASS: chord/hold input test")
        return True


def test_type_input():
    """IPC type: 'input type' routed through the AutoTypeQueue (Phase 4).

    Verifies that 'input type' now goes through g_autotype_queue (beads-c8fn),
    so WinAPE ~KEY~ tokens and newlines work exactly as with 'autotype':
      - plain text and ~KEY~ tokens accepted,
      - missing text rejected.
    That ~KEY~ parses to a KEY action (not literal characters) is asserted by
    the IpcServerTest gtest, which inspects the queue directly.
    """
    print("Running type input test...")

    with EmulatorRunner() as emu:
        if not emu.start():
            print("FAIL: Could not start emulator")
            return False

        checks = [
            ('autotype clear', True),            # ensure the queue is idle
            ('input type "hello world"', True),  # plain text
            ('input type "~RETURN~"', True),     # ~KEY~ token (now via the queue)
            ('input type "~ENTER~"', True),      # ~KEY~ alias
            ('input type', False),               # missing text
        ]
        for cmd, want_ok in checks:
            ok, resp = emu.ipc.send_command(cmd)
            if ok != want_ok:
                print(f"FAIL: {cmd!r} -> ok={ok} (wanted {want_ok}): {resp.strip()}")
                return False
            print(f"  {cmd!r} -> {resp.strip()}")

        print("PASS: type input test")
        return True


def test_input_state():
    """IPC input state readback (Phase 5, beads-bej2).

    Verifies 'input state' reports held keys from keyboard_matrix:
      - empty when nothing is held,
      - SHIFT reported held after 'input keydown SHIFT' (also catches the
        modifier-self release regression where keydown SHIFT never latched),
      - a row view reports the raw byte + the name,
      - released after keyup / after a chord tap,
      - out-of-range / non-numeric rows rejected.
    """
    print("Running input state readback test...")

    with EmulatorRunner() as emu:
        if not emu.start():
            print("FAIL: Could not start emulator")
            return False

        ok, resp = emu.ipc.send_command('input state')
        if not ok or '(none)' not in resp:
            print(f"FAIL: expected held=(none) initially, got {resp!r}")
            return False
        print(f"  initial: {resp.strip()}")

        emu.ipc.send_command('input keydown SHIFT')
        ok, resp = emu.ipc.send_command('input state')
        if not ok or 'SHIFT' not in resp:
            print(f"FAIL: expected SHIFT held after keydown, got {resp!r}")
            return False
        print(f"  after keydown SHIFT: {resp.strip()}")

        # SHIFT is row 2 bit 5 (0x25); the row view shows the raw byte + name.
        ok, resp = emu.ipc.send_command('input state 2')
        if not ok or 'row2=' not in resp or 'SHIFT' not in resp:
            print(f"FAIL: expected row2 byte + SHIFT, got {resp!r}")
            return False
        print(f"  row 2: {resp.strip()}")

        emu.ipc.send_command('input keyup SHIFT')
        ok, resp = emu.ipc.send_command('input state')
        if not ok or '(none)' not in resp:
            print(f"FAIL: expected released after keyup, got {resp!r}")
            return False
        print(f"  after keyup SHIFT: {resp.strip()}")

        # A chord tap presses then releases, so nothing stays held.
        emu.ipc.send_command('input chord SHIFT+A')
        ok, resp = emu.ipc.send_command('input state')
        if not ok or '(none)' not in resp:
            print(f"FAIL: expected nothing held after chord tap, got {resp!r}")
            return False
        print(f"  after chord tap: {resp.strip()}")

        for cmd in ('input state 99', 'input state xyz'):
            ok, resp = emu.ipc.send_command(cmd)
            if ok:
                print(f"FAIL: expected ERR for {cmd!r}, got OK: {resp!r}")
                return False
            print(f"  {cmd!r} -> {resp.strip()}")

        print("PASS: input state readback test")
        return True


def test_joystick_input():
    """IPC joystick input: the 'input joy' command surface.

    The device-level behaviour (J0 -> matrix row 9, J1 -> row 6, press toggles
    the right bit) is covered by the JoystickInputTest gtest suite; this asserts
    the IPC command contract end-to-end.
    """
    print("Running joystick input test...")

    with EmulatorRunner() as emu:
        if not emu.start():
            print("FAIL: Could not start emulator")
            return False

        checks = [
            ('input joy 0 U', True),       # joystick 0 up
            ('input joy 0 -U', True),      # release up
            ('input joy 0 F1', True),      # fire 1
            ('input joy 1 RIGHT', True),   # joystick 1 right
            ('input joy 0 0', True),       # release all directions
            ('input joy 0 SIDEWAYS', False),  # bad direction
            ('input joy', False),          # missing args
        ]
        for cmd, want_ok in checks:
            ok, resp = emu.ipc.send_command(cmd)
            if ok != want_ok:
                print(f"FAIL: {cmd!r} -> ok={ok} (wanted {want_ok}): {resp.strip()}")
                return False
            print(f"  {cmd!r} -> {resp.strip()}")

        print("PASS: joystick input test")
        return True


def test_load_accepts_flux_disk_formats():
    """IPC `load` routes flux disk images to drive A (beads-wxy6).

    The dispatcher hard-coded ".dsk", so `load game.hfe` returned
    ERR 415 unsupported even though the loader handles HFE — a front door
    that had drifted from what slotshandler actually accepts.

    Discriminates at the DOOR, not the loader: a deliberately-invalid file
    with an accepted extension must get past extension dispatch and fail in
    the loader (ERR 500), while an unknown extension is still rejected up
    front (ERR 415). That distinction is exactly what this fix changed, and
    it needs no real disk fixture — the repo has no flux images checked in.
    """
    print("Running IPC flux-format load routing test...")

    import tempfile
    import zipfile

    with EmulatorRunner() as emu:
        if not emu.start():
            print("FAIL: Could not start emulator")
            return False

        with tempfile.TemporaryDirectory() as td:
            # Accepted extensions: past the door, rejected by the loader.
            for ext in ('.hfe', '.scp', '.a2r', '.ipf', '.raw', '.dsk'):
                path = os.path.join(td, 'probe' + ext)
                with open(path, 'wb') as f:
                    f.write(b'not a real disk image')
                ok, resp = emu.ipc.send_command('load ' + path)
                if '415' in resp:
                    print(f"FAIL: {ext} rejected at the door: {resp.strip()!r}")
                    return False
                print(f"  {ext}: accepted by dispatch -> {resp.strip()}")

            # A ZIP is classified by its first supported member, just like
            # command-line and drag/drop loading.
            zip_path = os.path.join(td, 'probe.zip')
            with zipfile.ZipFile(zip_path, 'w') as archive:
                archive.writestr('inside.dsk', b'not a real disk image')
            ok, resp = emu.ipc.send_command('load ' + zip_path)
            if ok or not resp.startswith('ERR 500'):
                print(
                    "FAIL: .zip did not reach the inner DSK loader: "
                    f"{resp.strip()!r}")
                return False
            print(f"  .zip: classified by inner media -> {resp.strip()}")

            # A zip with no member the loader recognises must be refused
            # with the dedicated no-supported-media error, not routed to
            # any loader.
            no_media_zip = os.path.join(td, 'no_media.zip')
            with zipfile.ZipFile(no_media_zip, 'w') as archive:
                archive.writestr('readme.txt', b'not a disk or tape image')
            ok, resp = emu.ipc.send_command('load ' + no_media_zip)
            if ok or 'no-supported-media-in-zip' not in resp:
                print(
                    "FAIL: zip with no supported media should be "
                    f"ERR 415 no-supported-media-in-zip, got {resp.strip()!r}")
                return False
            print(f"  .zip with no supported media: correctly refused -> "
                  f"{resp.strip()}")

            # A corrupt/malformed zip must fail the same way, not crash or
            # hang the IPC command.
            corrupt_zip = os.path.join(td, 'corrupt.zip')
            with open(corrupt_zip, 'wb') as f:
                f.write(b'PK\x03\x04not actually a zip file')
            ok, resp = emu.ipc.send_command('load ' + corrupt_zip)
            if ok or 'no-supported-media-in-zip' not in resp:
                print(
                    "FAIL: corrupt zip should be "
                    f"ERR 415 no-supported-media-in-zip, got {resp.strip()!r}")
                return False
            print(f"  corrupt .zip: correctly refused -> {resp.strip()}")

            # Unknown extension must still be refused up front.
            path = os.path.join(td, 'probe.xyz')
            with open(path, 'wb') as f:
                f.write(b'nope')
            ok, resp = emu.ipc.send_command('load ' + path)
            if '415' not in resp:
                print(f"FAIL: unknown ext should be ERR 415, got {resp.strip()!r}")
                return False
            print(f"  .xyz: correctly refused -> {resp.strip()}")

        # The help text must advertise what the dispatcher accepts.
        ok, resp = emu.ipc.send_command('help load')
        if '.hfe' not in resp or '.zip' not in resp:
            print(f"FAIL: 'help load' omits accepted formats: {resp!r}")
            return False
        print("  help load advertises the flux formats")

    print("PASS: IPC load accepts flux disk formats")
    return True


def read_lower_rom_signature(ipc: KoncepcjaIPC, addr: int = 0x02E0,
                             length: int = 16,
                             max_steps: int = 20000) -> Optional[str]:
    """Read `length` bytes of the LOWER ROM at `addr`, or None if not found.

    A plain `mem read` is not a ROM read. It returns what the Z80 would see
    right now, and the firmware switches the lower ROM in and out all the
    time (it sits disabled at the BASIC prompt). On a running machine a read
    at 0x02E0 got ROM ~93% of the time on a 6128 and ~50% on a 6128+; the
    rest were the zeros of the RAM underneath, or a byte-by-byte mix of both.
    That is beads-idt4: the reference signatures read as all zeros on a
    loaded CI Mac.

    So: pause, and walk forward with `step in` (which also waits for the Z80
    thread to go idle -- `pause` alone only raises a flag) until the window
    read as the CPU sees it differs from the same window read with
    --view=ram. Only then did the bytes come from a ROM overlay. Stepping
    rather than run/pause retries keeps this off the wall clock: a pause
    lands at the same point in the frame each time, where the ROM may well
    be off. The comparison is the direct proof; the `rom:LO` flag in the
    context line is only the Gate Array's latch. (Before beads-szc0 that flag
    also went stale while paused: a step refreshed the registers but not the
    chip views, so it kept the pre-pause ROM state.)
    The machine is left running, as EmulatorRunner.start() hands it over.
    """
    stride = 7  # odd, so the walk does not lock onto a loop's period
    ipc.pause()
    try:
        ok, _ = ipc.step_in(1)  # the first read must not race a running Z80
        if not ok:
            return None
        for _ in range(0, max_steps, stride):
            ok_cpu, cpu = ipc.read_mem(addr, length)
            ok_ram, ram = ipc.send_command(
                f'mem read 0x{addr:04X} {length} --view=ram')
            if not (ok_cpu and ok_ram):
                return None
            cpu = cpu.replace('OK ', '').strip().upper()
            ram = ram.replace('OK ', '').strip().upper()
            if cpu != ram:
                return cpu
            ok, _ = ipc.step_in(stride)
            if not ok:
                return None
        return None
    finally:
        ipc.run()


def test_cartridge_hotload_reattaches():
    """A cartridge loaded into a running 6128+ is the one the board reads.

    beads-lrdy: the board holds the cartridge image by a raw pointer, and
    loading a cartridge frees that image and allocates another. `load x.cpr`
    never told the board, which went on reading released memory; it looked
    fine because the allocator usually hands the same block back, so the
    proof that counts is this test against an AddressSanitizer build. A file
    that was refused left the board with no image at all.

    The second cartridge is the system one with one byte of the character
    set changed, so the machine still boots and the lower ROM says which
    cartridge the board is reading.
    """
    print("Running cartridge hot-load test...")
    window, patch = 0x3C78, 0x3C7A

    system = PROJECT_ROOT / 'rom' / 'system.cpr'
    image = bytearray(system.read_bytes())
    bank0 = image.find(b'cb00') + 8  # chunk id, 32-bit length, then the bank
    if bank0 < 8:
        print("FAIL: rom/system.cpr has no cb00 chunk")
        return False
    original = bytes(image[bank0 + window:bank0 + window + 8]).hex().upper()
    image[bank0 + patch] ^= 0xA5
    patched = bytes(image[bank0 + window:bank0 + window + 8]).hex().upper()

    def lower_rom(ipc: KoncepcjaIPC) -> Optional[str]:
        sig = read_lower_rom_signature(ipc, window, 8)
        return None if sig is None else sig.replace(' ', '')

    tmp = Path(tempfile.mkdtemp(prefix='koncpc_cart_'))
    try:
        other = tmp / 'other.cpr'
        other.write_bytes(image)
        refused = tmp / 'refused.cpr'
        refused.write_bytes(b'RIFF\x04\x00\x00\x00nope')

        with EmulatorRunner() as emu:
            if not emu.start('-O', 'system.model=3'):
                print("FAIL: Could not start 6128+")
                return False
            ipc = emu.ipc
            if lower_rom(ipc) != original:
                print("FAIL: the 6128+ did not boot the system cartridge")
                return False

            steps = (
                (other, True, patched, 'the new cartridge'),
                (refused, False, patched, 'the cartridge it had (refused file)'),
                (system, True, original, 'the system cartridge again'),
            )
            for path, accepted, expected, what in steps:
                ok, resp = ipc.send_command(f'load {path}')
                if ok != accepted:
                    print(f"FAIL: load {path.name}: {resp}")
                    return False
                # Frames of the machine, on whatever image the board holds.
                if not ipc.wait_vbl(25):
                    print(f"FAIL: no frames after load {path.name}")
                    return False
                ipc.run()
                seen = lower_rom(ipc)
                if seen != expected:
                    print(f"FAIL: after load {path.name} the lower ROM reads "
                          f"{seen}, expected {expected} ({what})")
                    return False
                if accepted:
                    continue
                # A rebuild loads the cartridge the config names, and gives
                # up on the whole emulator when that fails: the refused file
                # must not have stayed behind as the configured cartridge.
                ok, resp = ipc.send_command('config apply')
                if not ok or lower_rom(ipc) != expected:
                    print(f"FAIL: rebuild after a refused cartridge: {resp}")
                    return False

        print("PASS: Cartridge hot-load test")
        return True
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def test_model_change_rebuild():
    """Model changes must rebuild the board, not just mutate config state.

    Repro for beads-o0iq item 3: switching 6128 -> 6128+ through the live
    settings path used to update CPC.model, but the running board kept the old
    model's boot ROM because koncpc_rebuild_machine() never restarted the
    bridge. The proof is the visible ROM signature at 0x02E0: a rebuilt Plus
    machine must match a fresh 6128+ boot, not the pre-change 6128 boot.
    """
    print("Running model-change rebuild test...")

    # 0x02E5 differs between cpc6128.rom and the 6128+ system cartridge; low
    # vectors at 0x0000 are identical, so sample a window that actually changes.
    rom_signature = read_lower_rom_signature

    def model_value(ipc: KoncepcjaIPC) -> Optional[int]:
        ok, resp = ipc.send_command('config get model')
        if not ok:
            return None
        try:
            return int(resp.replace('OK', '').strip())
        except ValueError:
            return None

    with EmulatorRunner() as ref_6128:
        if not ref_6128.start('-O', 'system.model=2'):
            print("FAIL: Could not start 6128 reference machine")
            return False
        sig_6128 = rom_signature(ref_6128.ipc)
        if sig_6128 is None:
            print("FAIL: Could not read 6128 ROM signature")
            return False

    with EmulatorRunner() as ref_plus:
        if not ref_plus.start('-O', 'system.model=3'):
            print("FAIL: Could not start 6128+ reference machine")
            return False
        sig_plus = rom_signature(ref_plus.ipc)
        if sig_plus is None:
            print("FAIL: Could not read 6128+ ROM signature")
            return False

    if sig_6128 == sig_plus:
        print(f"FAIL: Reference ROM signatures are identical: {sig_6128}")
        return False

    with EmulatorRunner() as emu:
        if not emu.start('-O', 'system.model=2'):
            print("FAIL: Could not start emulator under test")
            return False

        before_model = model_value(emu.ipc)
        if before_model != 2:
            print(f"FAIL: Expected initial model 2, got {before_model!r}")
            return False

        before_sig = rom_signature(emu.ipc)
        if before_sig != sig_6128:
            print(f"FAIL: Initial ROM signature {before_sig!r} != 6128 ref {sig_6128!r}")
            return False

        ok, resp = emu.ipc.send_command('config set model 3')
        if not ok:
            print(f"FAIL: config set model 3 failed: {resp}")
            return False
        ok, resp = emu.ipc.send_command('config apply')
        if not ok:
            print(f"FAIL: config apply failed: {resp}")
            return False

        after_model = model_value(emu.ipc)
        if after_model != 3:
            print(f"FAIL: Expected rebuilt model 3, got {after_model!r}")
            return False

        after_sig = rom_signature(emu.ipc)
        if after_sig != sig_plus:
            print(f"FAIL: Rebuilt ROM signature {after_sig!r} != 6128+ ref {sig_plus!r}")
            return False

        if after_sig == before_sig:
            print(f"FAIL: ROM signature stayed on the old model: {after_sig}")
            return False

        print(f"  6128 signature   : {sig_6128}")
        print(f"  6128+ signature  : {sig_plus}")
        print(f"  rebuilt signature: {after_sig}")
        print("PASS: model change rebuilt the board and boot ROM")
        return True


def test_profile_load_rebuilds_machine():
    """profile load must idle + rebuild when model/ram_size change.

    Repro for beads-x3ka: ConfigProfileManager::load() wrote CPC.model straight
    into the global struct with no pause and no emulator_init(), so IPC
    `profile load 6128plus` left banks/ASIC/ROMs on the old machine while
    config reported Plus. The proof matches test_model_change_rebuild: the
    ROM signature at 0x02E0 must match a fresh 6128+ boot after the load.
    Soft-only profile fields are covered by the unit suite; this guards the
    runtime caller contract.
    """
    print("Running profile-load rebuild test...")

    rom_signature = read_lower_rom_signature

    def model_value(ipc: KoncepcjaIPC) -> Optional[int]:
        ok, resp = ipc.send_command('config get model')
        if not ok:
            return None
        try:
            # Strip a possible ` pending=<n>` suffix — profile load clears it,
            # but tolerate the config get format.
            token = resp.replace('OK', '').strip().split()[0]
            return int(token)
        except (ValueError, IndexError):
            return None

    with EmulatorRunner() as ref_plus:
        if not ref_plus.start('-O', 'system.model=3'):
            print("FAIL: Could not start 6128+ reference machine")
            return False
        sig_plus = rom_signature(ref_plus.ipc)
        if sig_plus is None:
            print("FAIL: Could not read 6128+ ROM signature")
            return False

    with EmulatorRunner() as emu:
        if not emu.start('-O', 'system.model=2'):
            print("FAIL: Could not start emulator under test")
            return False

        before_model = model_value(emu.ipc)
        if before_model != 2:
            print(f"FAIL: Expected initial model 2, got {before_model!r}")
            return False

        before_sig = rom_signature(emu.ipc)
        if before_sig is None:
            print("FAIL: Could not read initial ROM signature")
            return False
        if before_sig == sig_plus:
            print(f"FAIL: 6128 boot already matches Plus ref: {before_sig}")
            return False

        # Built-in profile — no host .kpf required.
        ok, resp = emu.ipc.send_command('profile load 6128plus')
        if not ok:
            print(f"FAIL: profile load 6128plus failed: {resp}")
            return False

        after_model = model_value(emu.ipc)
        if after_model != 3:
            print(f"FAIL: Expected model 3 after profile load, got {after_model!r}")
            return False

        ok, cur = emu.ipc.send_command('profile current')
        if not ok or '6128plus' not in cur:
            print(f"FAIL: profile current after load: {cur!r}")
            return False

        after_sig = rom_signature(emu.ipc)
        if after_sig != sig_plus:
            print(f"FAIL: Profile-load ROM signature {after_sig!r} != "
                  f"6128+ ref {sig_plus!r}")
            return False

        if after_sig == before_sig:
            print(f"FAIL: ROM signature stayed on the old model: {after_sig}")
            return False

        # Soft re-load of the same identity must stay OK without a second
        # identity change (still rebuilds only when model/ram differ).
        ok, resp = emu.ipc.send_command('profile load 6128plus')
        if not ok:
            print(f"FAIL: second profile load 6128plus failed: {resp}")
            return False
        if model_value(emu.ipc) != 3:
            print("FAIL: model drifted after same-profile reload")
            return False

        print(f"  before (6128)    : {before_sig}")
        print(f"  6128+ reference  : {sig_plus}")
        print(f"  after profile load: {after_sig}")
        print("PASS: profile load rebuilt the board under pause lease")
        return True


def test_disk_live_put_cat():
    """Live FDC is authoritative for IPC disk put/cat (beads-csl7.1 / lly6).

    Unit tests only cover pull/push when the bridge is inactive. This starts a
    real board, formats drive A, writes a host file onto the live medium, and
    reads it back. A put that returns OK but a cat that cannot see the bytes
    is a stale host-view bug, not a generic command failure.
    """
    print("Running live-board disk put/cat test...")

    with EmulatorRunner() as emu:
        if not emu.start():
            print("FAIL: Could not start emulator")
            return False

        ok, resp = emu.ipc.send_command('disk format A data')
        if not ok:
            print(f"FAIL: disk format command ERR (not stale-view): {resp}")
            return False

        with tempfile.TemporaryDirectory() as td:
            host = os.path.join(td, 'hello.bin')
            with open(host, 'wb') as f:
                f.write(b'HI')

            ok, resp = emu.ipc.send_command(f'disk put A {host} HELLO.BIN')
            if not ok:
                print(f"FAIL: disk put command ERR (not stale-view): {resp}")
                return False

            ok, ls_resp = emu.ipc.send_command('disk ls A')
            if not ok or 'HELLO.BIN' not in ls_resp:
                print(f"FAIL: stale host view after OK put; ls={ls_resp!r}")
                return False

            ok, cat_resp = emu.ipc.send_command('disk cat A HELLO.BIN')
            if not ok:
                print(f"FAIL: stale host view after OK put; cat={cat_resp!r}")
                return False
            compact = cat_resp.replace(' ', '').upper()
            if '48' not in compact or '49' not in compact:
                print(f"FAIL: cat payload mismatch (stale or truncated): "
                      f"{cat_resp!r}")
                return False

        print(f"  ls: {ls_resp.strip()}")
        print(f"  cat: {cat_resp.strip()}")
        print("PASS: live-board disk put/cat round-trip")
        return True


def test_disk_put_basic_listing_runs():
    """A BASIC listing pushed with `disk put` must RUN (beads-otjf).

    `disk put` gave every file a BINARY header with load/exec address 0, so
    RUN" of a pushed .bas executed its text as Z80 code at &0000 and rebooted
    the machine. A plain-text .bas is now written the way SAVE"x",A writes one
    -- no header, CR LF lines, a ^Z at the end -- and BASIC tokenises it on
    load. The program's POKE is the proof it ran.
    """
    print("Running disk put BASIC listing → RUN\" test...")

    with EmulatorRunner() as emu:
        if not emu.start():
            print("FAIL: Could not start emulator")
            return False
        ipc = emu.ipc
        ipc.timeout = 25.0
        with tempfile.TemporaryDirectory() as td:
            host = os.path.join(td, 'prog.bas')
            with open(host, 'w', newline='\n') as f:
                f.write('10 POKE &6100,&61\n')
            for cmd in ('disk format A data', f'disk put A {host}'):
                ok, resp = ipc.send_command(cmd)
                if not ok:
                    print(f"FAIL: {cmd}: {resp}")
                    return False
        # A plain listing is stored headerless; BINARY here is the old bug.
        ok, resp = ipc.send_command('disk info A PROG.BAS')
        if ok or 'no valid AMSDOS header' not in resp:
            print(f"FAIL: a listing must be stored without a header: {resp}")
            return False
        # Boot to the prompt and let it settle before typing (see
        # test_m4_cat_lists_the_sd_card: keys typed at once get eaten).
        for cmd in ('wait vbl 150 30000', 'mem write 0x6100 00'):
            ok, resp = ipc.send_command(cmd)
            if not ok:
                print(f"FAIL: {cmd}: {resp}")
                return False
        ipc.run()
        # NOT quoted: autotype types everything after the first space.
        ok, resp = ipc.send_command('autotype run"prog~RETURN~')
        if not ok:
            print(f"FAIL: autotype: {resp}")
            return False
        ok, resp = ipc.send_command('wait mem 0x6100 0x61 20000')
        if not ok:
            print(f"FAIL: RUN\"prog never executed the listing: {resp}")
            return False
        print("PASS: a disk-put BASIC listing loads and RUNs")
        return True


def test_frames_dump_stops_at_a_breakpoint():
    """`frames dump` must end, not wedge, when a breakpoint fires mid-step.

    Each recorded frame is a one-frame step; a breakpoint pauses the machine
    before the frame completes, so the step never counted down and the old
    loop spun on it forever -- the socket, and every command after it, hung.
    RST &38 runs on every interrupt (300/s), so a breakpoint there fires in
    the first frame.
    """
    print("Running frames dump vs breakpoint test...")
    with EmulatorRunner() as emu:
        if not emu.start():
            print("FAIL: Could not start emulator")
            return False
        ipc = emu.ipc
        ipc.timeout = 15.0
        with tempfile.TemporaryDirectory() as td:
            gif = os.path.join(td, 'rec.gif')
            for cmd in ('wait vbl 10', 'bp add 0x0038'):
                ok, resp = ipc.send_command(cmd)
                if not ok:
                    print(f"FAIL: {cmd}: {resp}")
                    return False
            ok, resp = ipc.send_command(f'frames dump {gif} 20')
            if not ok or not resp.startswith('OK frames='):
                print(f"FAIL: frames dump under a breakpoint: {resp!r}")
                return False
            recorded = int(resp.split('=')[1].split()[0])
            if recorded >= 20:
                print(f"FAIL: recorded all {recorded} frames through a "
                      f"breakpoint at &0038")
                return False
            ok, resp = ipc.send_command('ping')
            if not ok:
                print(f"FAIL: connection unusable afterwards: {resp}")
                return False
        ipc.send_command('bp clear')
        print(f"PASS: recording ended at the breakpoint ({recorded} frames)")
        return True


def _hash_frame_after(snapshot: str, *args) -> Tuple[Optional[str], str]:
    """Start a core, load `snapshot`, run 3 frames, and hash its frame.

    Returns (crc, the whole reply), or (None, why).
    """
    with EmulatorRunner() as emu:
        if not emu.start(*args):
            return None, "could not start emulator"
        ipc = emu.ipc
        for cmd in ('pause', f'snapshot load {snapshot}', 'wait vbl 3'):
            ok, resp = ipc.send_command(cmd)
            if not ok:
                return None, f"{cmd}: {resp}"
        ok, resp = ipc.send_command('hash frame')
        if not ok or not resp.startswith('OK crc32='):
            return None, f"hash frame: {resp!r}"
        ok, vram = ipc.send_command('hash vram')
        crc = resp.split()[1]
        if not ok or vram.strip() != f'OK {crc}':
            return None, f"hash vram {vram!r} is not hash frame's {crc}"
        return crc, resp.strip()


def test_hash_frame_is_the_machine_frame():
    """`hash frame` hashes the machine's own frame, whoever presents it.

    beads-0ir.1: `hash vram` hashed the host surface the frame runner blits
    the frame into, scaled to the window and in the video plugin's pixel
    format, read from the IPC thread beside the runner. The same machine state
    hashed differently under the GUI build and -H. Both now hash the
    canonical frame (768x272 RGB24), copied between two frames: the same
    snapshot, run the same 3 frames, hashes the same in the threaded loop and
    the -H loop. The animated GIF of `frames dump` is that frame too, each
    row shown twice (768x544), in either loop.
    """
    print("Running hash frame test...")
    with tempfile.TemporaryDirectory() as td:
        sna = os.path.join(td, 'state.sna')
        gif = os.path.join(td, 'rec.gif')
        with EmulatorRunner() as emu:
            if not emu.start():
                print("FAIL: Could not start emulator")
                return False
            ipc = emu.ipc
            for cmd in ('wait vbl 100', f'snapshot save {sna}',
                        f'frames dump {gif} 2'):
                ok, resp = ipc.send_command(cmd)
                if not ok:
                    print(f"FAIL: {cmd}: {resp}")
                    return False
            ok, resp = ipc.send_command('hash frame')
            fields = dict(f.split('=', 1) for f in resp.split()[1:] if '=' in f)
            want = {'width': '768', 'height': '272', 'stride': '2304',
                    'format': 'rgb24', 'paused': '1'}
            if not ok or any(fields.get(k) != v for k, v in want.items()):
                print(f"FAIL: hash frame reply {resp!r}, want {want}")
                return False
        with open(gif, 'rb') as f:
            head = f.read(10)
        size = (int.from_bytes(head[6:8], 'little'),
                int.from_bytes(head[8:10], 'little'))
        if head[:3] != b'GIF' or size != (768, 544):
            print(f"FAIL: frames dump GIF is {head[:6]!r} {size}, want "
                  f"GIF (768, 544)")
            return False
        # The loop under test, then the other one.
        threaded, why = _hash_frame_after(sna)
        if threaded is None:
            print(f"FAIL: first core: {why}")
            return False
        other_args = ('--headless',)
        if os.environ.get('KONCPC_E2E_HEADLESS') == '1':
            # Both -H: still a fresh core, at another window scale.
            other_args = ('--headless', '-O', 'video.scr_scale=1')
        other, why = _hash_frame_after(sna, *other_args)
        if other is None:
            print(f"FAIL: second core: {why}")
            return False
        if threaded != other:
            print(f"FAIL: the same frame hashed {threaded} in one loop and "
                  f"{other} in the other")
            return False
    print(f"PASS: hash frame agrees across loops ({threaded})")
    return True


def test_disk_status_save_eject():
    """File-menu Save Disk / Eject Disk over IPC (live FDC, not host t_drive).

    After put, disk save must persist the CPC file into a loadable image.
    Reload after eject is the proof the bytes came from the live medium.
    Drive B flux save is 409 because flux is A-only.
    """
    print("Running live-board disk status/save/eject test...")

    with EmulatorRunner() as emu:
        if not emu.start():
            print("FAIL: Could not start emulator")
            return False

        ok, status = emu.ipc.send_command('disk eject A')
        if not ok:
            print(f"FAIL: eject empty A: {status}")
            return False
        ok, status = emu.ipc.send_command('disk status A')
        if not ok or 'present=0' not in status or 'backing=empty' not in status:
            print(f"FAIL: empty status: {status!r}")
            return False

        ok, resp = emu.ipc.send_command('disk format A data')
        if not ok:
            print(f"FAIL: disk format: {resp}")
            return False
        ok, status = emu.ipc.send_command('disk status A')
        if not ok or 'present=1' not in status or 'can_dsk=1' not in status:
            print(f"FAIL: formatted status: {status!r}")
            return False
        if 'can_scp=1' in status:
            print(f"FAIL: sector disc reported flux caps: {status!r}")
            return False

        with tempfile.TemporaryDirectory() as td:
            host = os.path.join(td, 'hello.bin')
            with open(host, 'wb') as f:
                f.write(b'HI')
            ok, resp = emu.ipc.send_command(f'disk put A {host} HELLO.BIN')
            if not ok:
                print(f"FAIL: put: {resp}")
                return False

            saved = os.path.join(td, 'saved.dsk')
            ok, resp = emu.ipc.send_command(f'disk save A {saved} dsk')
            if not ok:
                print(f"FAIL: save dsk: {resp}")
                return False
            if not os.path.isfile(saved) or os.path.getsize(saved) < 64:
                print(f"FAIL: saved image missing or tiny: {saved}")
                return False

            ok, resp = emu.ipc.send_command('disk eject A')
            if not ok:
                print(f"FAIL: eject: {resp}")
                return False
            ok, ls = emu.ipc.send_command('disk ls A')
            if ok and 'HELLO.BIN' in ls:
                print(f"FAIL: HELLO.BIN still listed after eject: {ls!r}")
                return False

            ok, resp = emu.ipc.send_command(f'load {saved}')
            if not ok:
                print(f"FAIL: reload saved image: {resp}")
                return False
            ok, ls = emu.ipc.send_command('disk ls A')
            if not ok or 'HELLO.BIN' not in ls:
                print(f"FAIL: saved image dropped live write; ls={ls!r}")
                return False

            flux_path = os.path.join(td, 'blank.scp')
            ok, resp = emu.ipc.send_command(
                f'disk new {flux_path} data flux')
            if not ok:
                print(f"FAIL: disk new flux: {resp}")
                return False
            ok, resp = emu.ipc.send_command(f'load {flux_path}')
            if not ok:
                print(f"FAIL: load flux: {resp}")
                return False
            ok, status = emu.ipc.send_command('disk status A')
            if not ok or 'backing=flux' not in status or 'can_scp=1' not in status:
                print(f"FAIL: flux status: {status!r}")
                return False
            out_scp = os.path.join(td, 'out.scp')
            ok, resp = emu.ipc.send_command(f'disk save A {out_scp} scp')
            if not ok:
                print(f"FAIL: save scp on A: {resp}")
                return False
            ok, resp = emu.ipc.send_command(
                f'disk save B {os.path.join(td, "b.scp")} scp')
            if ok or '409' not in resp:
                print(f"FAIL: expected 409 saving scp on B, got {resp!r}")
                return False

        print(f"  status after format: sector, can_dsk")
        print(f"  save/reload kept HELLO.BIN; A scp save OK; B scp 409")
        print("PASS: live-board disk status/save/eject")
        return True


def test_disk_eject_flushes_dirty_writes():
    """`disk eject` must persist dirty writes even without an explicit save.

    Regression for the eject/flush ordering bug: subcycle_bridge_apply_pending_media()
    (which flushes dirty sectors back to CPC.driveA/B.file) must run BEFORE that
    path is cleared, or the flush silently no-ops and the write is lost.
    """
    print("Running disk eject dirty-write-flush test...")

    with EmulatorRunner() as emu:
        if not emu.start():
            print("FAIL: Could not start emulator")
            return False

        with tempfile.TemporaryDirectory() as td:
            disk_path = os.path.join(td, 'dirty.dsk')
            ok, resp = emu.ipc.send_command(f'disk new {disk_path} data sector')
            if not ok:
                print(f"FAIL: disk new: {resp}")
                return False

            ok, resp = emu.ipc.send_command(f'load {disk_path}')
            if not ok:
                print(f"FAIL: load: {resp}")
                return False

            host = os.path.join(td, 'dirty.bin')
            with open(host, 'wb') as f:
                f.write(b'UNSAVED')
            ok, resp = emu.ipc.send_command(f'disk put A {host} DIRTY.BIN')
            if not ok:
                print(f"FAIL: put: {resp}")
                return False

            # Eject WITHOUT an explicit `disk save` first — the flush-on-eject
            # path is the only thing that can persist this write.
            ok, resp = emu.ipc.send_command('disk eject A')
            if not ok:
                print(f"FAIL: eject: {resp}")
                return False

            ok, resp = emu.ipc.send_command(f'load {disk_path}')
            if not ok:
                print(f"FAIL: reload after eject: {resp}")
                return False
            ok, ls = emu.ipc.send_command('disk ls A')
            if not ok or 'DIRTY.BIN' not in ls:
                print(f"FAIL: eject discarded dirty write; ls={ls!r}")
                return False

        print("PASS: disk eject flushes dirty writes without an explicit save")
        return True


def _dirty_disc_in_drive_a(emu, td):
    """Create a.dsk in `td`, load it into drive A and write a file to it that
    is never saved: the write lives only in the FDC's copy of the disc.
    Returns (path of a.dsk, the marker the unsaved file holds) or None."""
    a_path = os.path.join(td, 'a.dsk')
    ok, resp = emu.ipc.send_command(f'disk new {a_path} data sector')
    if not ok:
        print(f"FAIL: disk new a: {resp}")
        return None
    ok, resp = emu.ipc.send_command(f'load {a_path}')
    if not ok:
        print(f"FAIL: load a: {resp}")
        return None
    marker = b'UNSAVED-WRITE-TO-DISC-A'
    host = os.path.join(td, 'hello.bin')
    with open(host, 'wb') as f:
        f.write(marker)
    ok, resp = emu.ipc.send_command(f'disk put A {host} HELLO.BIN')
    if not ok:
        print(f"FAIL: put: {resp}")
        return None
    return a_path, marker


def _read_bytes(path):
    with open(path, 'rb') as f:
        return f.read()


def test_disk_load_over_dirty_disc_flushes_to_its_own_file():
    """Loading a disc over one with unsaved writes must write those back to
    the OUTGOING disc's file (beads-11us).

    Every load path names the new file in CPC.driveA.file before the swap is
    applied, and the write-back used that field: the old disc's image landed
    in the NEW file and the old file never got its write.
    """
    print("Running load-over-dirty-disc test...")

    with EmulatorRunner() as emu:
        if not emu.start():
            print("FAIL: Could not start emulator")
            return False

        with tempfile.TemporaryDirectory() as td:
            b_path = os.path.join(td, 'b.dsk')
            ok, resp = emu.ipc.send_command(f'disk new {b_path} data sector')
            if not ok:
                print(f"FAIL: disk new b: {resp}")
                return False
            b_before = _read_bytes(b_path)

            dirty = _dirty_disc_in_drive_a(emu, td)
            if dirty is None:
                return False
            a_path, marker = dirty

            ok, resp = emu.ipc.send_command(f'load {b_path}')
            if not ok:
                print(f"FAIL: load b over dirty a: {resp}")
                return False
            # The swap is queued for the next frame boundary; a listing
            # applies it now, so the files below are read after the flush.
            ok, ls = emu.ipc.send_command('disk ls A')
            if not ok or 'HELLO.BIN' in ls:
                print(f"FAIL: drive A does not hold the blank b.dsk; ls={ls!r}")
                return False

            if _read_bytes(b_path) != b_before:
                print("FAIL: b.dsk was overwritten by the load that read it")
                return False
            if marker not in _read_bytes(a_path):
                print("FAIL: a.dsk lost its unsaved write")
                return False

        print("PASS: the outgoing disc's writes went back to its own file")
        return True


def test_disk_failed_load_does_not_redirect_write_back():
    """A load that fails leaves the old disc in the drive; its unsaved writes
    still belong to its own file, not to the file that failed to load
    (beads-11us: the failed path stayed in CPC.driveA.file)."""
    print("Running failed-load write-back test...")

    with EmulatorRunner() as emu:
        if not emu.start():
            print("FAIL: Could not start emulator")
            return False

        with tempfile.TemporaryDirectory() as td:
            dirty = _dirty_disc_in_drive_a(emu, td)
            if dirty is None:
                return False
            a_path, marker = dirty

            junk_path = os.path.join(td, 'junk.dsk')
            junk = b'this is not a disc image\n' * 8
            with open(junk_path, 'wb') as f:
                f.write(junk)
            ok, resp = emu.ipc.send_command(f'load {junk_path}')
            if ok:
                print(f"FAIL: a junk file loaded as a disc: {resp}")
                return False
            # A refused file changes nothing: the old disc, unsaved write
            # and all, is still in the drive (KCPC MEDIA_INSERT agrees:
            # test_kcpc.py, test_media_requests_are_checked).
            ok, ls = emu.ipc.send_command('disk ls A')
            if not ok or 'HELLO.BIN' not in ls:
                print(f"FAIL: the refused load emptied drive A; ls={ls!r}")
                return False

            ok, resp = emu.ipc.send_command('disk eject A')
            if not ok:
                print(f"FAIL: eject: {resp}")
                return False

            if _read_bytes(junk_path) != junk:
                print("FAIL: the file that failed to load was overwritten")
                return False
            if marker not in _read_bytes(a_path):
                print("FAIL: a.dsk lost its unsaved write")
                return False

        print("PASS: a failed load leaves the write-back pointed at the disc")
        return True


def _dirty_zipped_disc_in_drive_a(emu, td, marker):
    """Put Game.DSK (and a readme) in game.zip in `td`, load the archive into
    drive A and write a file holding `marker` to the disc, unsaved.
    Returns (path of game.zip, its bytes) or None."""
    import zipfile

    src = os.path.join(td, 'blank-for-zip.dsk')
    zip_path = os.path.join(td, 'game.zip')
    if not os.path.exists(zip_path):
        ok, resp = emu.ipc.send_command(f'disk new {src} data sector')
        if not ok:
            print(f"FAIL: disk new: {resp}")
            return None
        with zipfile.ZipFile(zip_path, 'w') as archive:
            archive.writestr('readme.txt', b'the other entry of the archive')
            archive.writestr('Game.DSK', _read_bytes(src))
        os.remove(src)
    zip_before = _read_bytes(zip_path)
    ok, resp = emu.ipc.send_command(f'load {zip_path}')
    if not ok:
        print(f"FAIL: load zip: {resp}")
        return None
    host = os.path.join(td, 'hello.bin')
    with open(host, 'wb') as f:
        f.write(marker)
    ok, resp = emu.ipc.send_command(f'disk put A {host} HELLO.BIN')
    if not ok:
        print(f"FAIL: put: {resp}")
        return None
    return zip_path, zip_before


def test_zipped_disc_write_back_goes_beside_the_archive():
    """Unsaved writes to a disc loaded from a .zip are written as a DSK next
    to the archive; the archive itself is never written (beads-lels: it was
    overwritten with the raw DSK, destroying its other entries). A .dsk
    already there is not overwritten either: the next free name is taken."""
    print("Running zipped-disc write-back test...")

    with EmulatorRunner() as emu:
        if not emu.start():
            print("FAIL: Could not start emulator")
            return False

        with tempfile.TemporaryDirectory() as td:
            first = b'FIRST-UNSAVED-WRITE-TO-THE-ZIPPED-DISC'
            dirty = _dirty_zipped_disc_in_drive_a(emu, td, first)
            if dirty is None:
                return False
            zip_path, zip_before = dirty
            ok, resp = emu.ipc.send_command('disk eject A')
            if not ok:
                print(f"FAIL: eject: {resp}")
                return False

            if _read_bytes(zip_path) != zip_before:
                print("FAIL: the archive was written to; it now starts with "
                      f"{_read_bytes(zip_path)[:8]!r}")
                return False
            dsk_path = os.path.join(td, 'Game.dsk')
            if not os.path.exists(dsk_path):
                print(f"FAIL: no Game.dsk beside the archive: {os.listdir(td)}")
                return False
            dsk_first = _read_bytes(dsk_path)
            if first not in dsk_first:
                print("FAIL: Game.dsk does not hold the unsaved write")
                return False
            ok, resp = emu.ipc.send_command(f'load {dsk_path}')
            ok2, ls = emu.ipc.send_command('disk ls A')
            if not ok or not ok2 or 'HELLO.BIN' not in ls:
                print(f"FAIL: Game.dsk is not a loadable disc: {resp!r} {ls!r}")
                return False

            # Again, with Game.dsk now in the way: it holds somebody's work.
            second = b'SECOND-UNSAVED-WRITE-TO-THE-ZIPPED-DISC'
            if _dirty_zipped_disc_in_drive_a(emu, td, second) is None:
                return False
            ok, resp = emu.ipc.send_command('disk eject A')
            if not ok:
                print(f"FAIL: second eject: {resp}")
                return False
            if _read_bytes(zip_path) != zip_before:
                print("FAIL: the archive was written to on the second eject")
                return False
            if _read_bytes(dsk_path) != dsk_first:
                print("FAIL: the existing Game.dsk was overwritten")
                return False
            dsk2_path = os.path.join(td, 'Game-2.dsk')
            if not os.path.exists(dsk2_path) or \
                    second not in _read_bytes(dsk2_path):
                print("FAIL: the second write is not in Game-2.dsk: "
                      f"{os.listdir(td)}")
                return False

        print("PASS: a zipped disc's writes land beside the archive")
        return True


def test_zipped_disc_keeps_its_writes_over_a_machine_rebuild():
    """A machine rebuild writes the discs back and reads the drives' files
    again. For a disc from a .zip the write goes beside the archive, so the
    drive must be pointed at that file: re-reading the archive would bring
    the unwritten disc back (beads-lels)."""
    print("Running zipped-disc machine-rebuild test...")

    with EmulatorRunner() as emu:
        if not emu.start():
            print("FAIL: Could not start emulator")
            return False

        with tempfile.TemporaryDirectory() as td:
            marker = b'UNSAVED-WRITE-KEPT-OVER-A-REBUILD'
            dirty = _dirty_zipped_disc_in_drive_a(emu, td, marker)
            if dirty is None:
                return False
            zip_path, zip_before = dirty

            ok, resp = emu.ipc.send_command('config apply')
            if not ok:
                print(f"FAIL: config apply: {resp}")
                return False

            if _read_bytes(zip_path) != zip_before:
                print("FAIL: the archive was written to by the rebuild")
                return False
            dsk_path = os.path.join(td, 'Game.dsk')
            if not os.path.exists(dsk_path) or \
                    marker not in _read_bytes(dsk_path):
                print(f"FAIL: the write is not in Game.dsk: {os.listdir(td)}")
                return False
            ok, status = emu.ipc.send_command('status')
            if not ok or 'image=Game.dsk' not in status:
                print(f"FAIL: drive A is not pointed at Game.dsk: {status!r}")
                return False
            ok, ls = emu.ipc.send_command('disk ls A')
            if not ok or 'HELLO.BIN' not in ls:
                print(f"FAIL: the rebuilt machine lost the write; ls={ls!r}")
                return False
            # The drive holds a plain DSK now: later writes go to that file.
            ok, resp = emu.ipc.send_command('disk eject A')
            if not ok:
                print(f"FAIL: eject: {resp}")
                return False
            if sorted(os.listdir(td)) != ['Game.dsk', 'game.zip', 'hello.bin']:
                print(f"FAIL: unexpected files: {sorted(os.listdir(td))}")
                return False

        print("PASS: a rebuild points the drive at the DSK beside the archive")
        return True


def test_zipped_disc_from_the_command_line_writes_beside_the_archive():
    """A .zip named on the command line is attached when the machine is
    built, not through a load: that path must know the disc came out of an
    archive too (beads-lels)."""
    print("Running zipped-disc command-line test...")
    import zipfile

    with tempfile.TemporaryDirectory() as td:
        src = os.path.join(td, 'blank.dsk')
        zip_path = os.path.join(td, 'boot.zip')
        with EmulatorRunner() as emu:
            if not emu.start():
                print("FAIL: Could not start emulator")
                return False
            ok, resp = emu.ipc.send_command(f'disk new {src} data sector')
            if not ok:
                print(f"FAIL: disk new: {resp}")
                return False
        with zipfile.ZipFile(zip_path, 'w') as archive:
            archive.writestr('Boot.dsk', _read_bytes(src))
        os.remove(src)
        zip_before = _read_bytes(zip_path)

        marker = b'UNSAVED-WRITE-TO-THE-DISC-FROM-ARGV'
        with EmulatorRunner() as emu:
            if not emu.start(zip_path):
                print("FAIL: Could not start emulator with the archive")
                return False
            host = os.path.join(td, 'hello.bin')
            with open(host, 'wb') as f:
                f.write(marker)
            ok, resp = emu.ipc.send_command(f'disk put A {host} HELLO.BIN')
            if not ok:
                print(f"FAIL: put: {resp}")
                return False
            ok, resp = emu.ipc.send_command('disk eject A')
            if not ok:
                print(f"FAIL: eject: {resp}")
                return False

        if _read_bytes(zip_path) != zip_before:
            print("FAIL: the archive was written to; it now starts with "
                  f"{_read_bytes(zip_path)[:8]!r}")
            return False
        dsk_path = os.path.join(td, 'Boot.dsk')
        if not os.path.exists(dsk_path) or marker not in _read_bytes(dsk_path):
            print(f"FAIL: the write is not in Boot.dsk: {os.listdir(td)}")
            return False

    print("PASS: a zipped disc from the command line is written beside it")
    return True


def test_zipped_disc_unwritable_directory_leaves_the_archive_alone():
    """When the DSK cannot be created beside the archive the write-back
    fails; it must not fall back to writing the archive (beads-lels)."""
    print("Running zipped-disc unwritable-directory test...")
    if os.name == 'nt' or os.geteuid() == 0:
        print("SKIP: needs a directory the process cannot write to")
        return True

    with EmulatorRunner() as emu:
        if not emu.start():
            print("FAIL: Could not start emulator")
            return False

        with tempfile.TemporaryDirectory() as td:
            dirty = _dirty_zipped_disc_in_drive_a(emu, td, b'NOWHERE-TO-GO')
            if dirty is None:
                return False
            zip_path, zip_before = dirty
            listing = sorted(os.listdir(td))
            os.chmod(td, 0o555)
            try:
                ok, resp = emu.ipc.send_command('disk eject A')
            finally:
                os.chmod(td, 0o755)
            if not ok:
                print(f"FAIL: eject: {resp}")
                return False
            if _read_bytes(zip_path) != zip_before:
                print("FAIL: the archive was written to")
                return False
            if sorted(os.listdir(td)) != listing:
                print(f"FAIL: unexpected files: {sorted(os.listdir(td))}")
                return False

        print("PASS: an unwritable directory never costs the archive")
        return True


def test_scripted_exit_writes_back_unsaved_disc_writes():
    """An exit nobody is asked about writes a disc's unsaved writes back, as
    an eject does (beads-lgul). doCleanUp() only queued an eject the stopped
    machine never applied, so IPC `quit`, a signal and --exit-after dropped
    them. A zipped disc's writes go beside the archive, as on an eject."""
    print("Running scripted-exit write-back test...")
    # --exit-after leaves the test this long to dirty the disc first.
    exit_after = '--exit-after=10s'
    cases = [('ipc quit', 'quit', ()), ('sigterm', 'sigterm', ()),
             ('exit-after', None, (exit_after,)),
             ('ipc quit, zipped disc', 'quit', ())]
    if os.name != 'posix':
        # Popen.send_signal(SIGTERM) on Windows is TerminateProcess(h, 1): no
        # signal reaches the emulator, so nothing could be written back.
        print("  (not POSIX: the signal case is skipped)")
        cases = [c for c in cases if c[1] != 'sigterm']
    for name, how, args in cases:
        with EmulatorRunner() as emu, tempfile.TemporaryDirectory() as td:
            if not emu.start(*args):
                print(f"FAIL: could not start emulator ({name})")
                return False
            proc = emu.process
            marker = b'UNSAVED-WRITE-AT-EXIT'
            if 'zipped' in name:
                dirty = _dirty_zipped_disc_in_drive_a(emu, td, marker)
                if dirty is None:
                    return False
                zip_path, zip_before = dirty
                written = os.path.join(td, 'Game.dsk')
            else:
                dirty = _dirty_disc_in_drive_a(emu, td)
                if dirty is None:
                    return False
                written, marker = dirty
            if os.path.exists(written) and marker in _read_bytes(written):
                print(f"FAIL: {name}: the write reached the file before the "
                      "exit; the test proves nothing")
                return False
            if how == 'quit':
                emu.ipc.send_command('quit')
            elif how == 'sigterm':
                proc.send_signal(signal.SIGTERM)
            try:
                proc.wait(timeout=30)
            except subprocess.TimeoutExpired:
                print(f"FAIL: {name}: the emulator did not exit")
                return False
            if not os.path.exists(written) or \
                    marker not in _read_bytes(written):
                print(f"FAIL: {name}: the disc's unsaved write was dropped "
                      f"at exit: {sorted(os.listdir(td))}")
                return False
            if 'zipped' in name and _read_bytes(zip_path) != zip_before:
                print(f"FAIL: {name}: the archive was written to")
                return False
    print("PASS: IPC quit, SIGTERM and --exit-after write a disc's unsaved "
          "writes back (a zipped disc's beside the archive)")
    return True


def test_exit_puts_the_zipped_disc_written_back_on_recent():
    """A GUI exit with code 0 saves the config. A zipped disc's writes go to
    a DSK beside the archive, and that DSK goes on the Recent list, as on an
    eject; at exit the write-back ran after the config was saved, so the
    entry never reached the file (beads-8c1o)."""
    print("Running exit Recent-list test...")
    with EmulatorRunner() as emu, tempfile.TemporaryDirectory() as td:
        if not emu.start():
            print("FAIL: could not start emulator")
            return False
        if not emu.ipc.is_threaded():
            # Only a GUI exit saves the config: -H never rewrites it.
            print("SKIP: only a GUI exit saves the config")
            return True
        dirty = _dirty_zipped_disc_in_drive_a(emu, td, b'ON-RECENT-AT-EXIT')
        if dirty is None:
            return False
        written = os.path.join(td, 'Game.dsk')
        if os.path.exists(written):
            print("FAIL: Game.dsk exists before the exit; the test proves "
                  "nothing")
            return False
        emu.ipc.send_command('quit')
        try:
            rc = emu.process.wait(timeout=30)
        except subprocess.TimeoutExpired:
            print("FAIL: the emulator did not exit")
            return False
        if rc != 0 or not os.path.exists(written):
            print(f"FAIL: exit {rc}, files {sorted(os.listdir(td))}")
            return False
        recent = None
        for line in Path(emu._cfg_tmp.name).read_text().splitlines():
            if line.startswith('mru_disk_00='):
                recent = line.split('=', 1)[1].strip()
        if recent is None or \
                os.path.realpath(recent) != os.path.realpath(written):
            print(f"FAIL: Recent starts with {recent!r}, not {written}")
            return False
    print("PASS: the DSK a zipped disc was written to at exit is on Recent")
    return True


# Writes sector 0 of the Symbiface II IDE master (ports &FD08-&FD0F, the ATA
# register file) full of 0xA5, then parks at IDE_WRITE_PARK.
IDE_WRITE_SECTOR_0 = bytes([
    0xF3,                    # di
    0x01, 0x0A, 0xFD,        # ld bc,&FD0A
    0x3E, 0x01,              # ld a,1
    0xED, 0x79,              # out (c),a      sector count = 1
    0xAF,                    # xor a
    0x0C, 0xED, 0x79,        # LBA low = 0
    0x0C, 0xED, 0x79,        # LBA mid = 0
    0x0C, 0xED, 0x79,        # LBA high = 0
    0x0C, 0x3E, 0xE0, 0xED, 0x79,  # drive/head: master, LBA
    0x0C, 0x3E, 0x30, 0xED, 0x79,  # command: WRITE SECTORS
    0x0E, 0x08,              # ld c,&08       data register
    0x3E, 0xA5,              # ld a,&A5
    0x16, 0x02,              # ld d,2
    0x1E, 0x00,              # ld e,0         2 x 256 bytes
    0xED, 0x79,              # loop: out (c),a
    0x1D,                    # dec e
    0x20, 0xFB,              # jr nz,loop
    0x15,                    # dec d
    0x20, 0xF8,              # jr nz,loop
    0x18, 0xFE,              # park: jr park
])
IDE_WRITE_PARK = 0x4000 + len(IDE_WRITE_SECTOR_0) - 2


def test_scripted_exit_writes_back_unsaved_ide_writes():
    """An exit nobody is asked about writes the Symbiface IDE image's unsaved
    writes back, as it does a disc's (beads-ssqd). The board works on a copy
    of the image that only a machine rebuild wrote back, so IPC `quit` and a
    signal dropped every sector the CPC had written."""
    print("Running scripted-exit IDE write-back test...")
    cases = ['quit', 'sigterm'] if os.name == 'posix' else ['quit']
    for how in cases:
        with EmulatorRunner() as emu, tempfile.TemporaryDirectory() as td:
            img = os.path.join(td, 'ide.img')
            with open(img, 'wb') as f:
                f.write(bytes(64 * 1024))
            if not emu.start('-O', 'peripheral.symbiface=1',
                             '-O', f'peripheral.ide_master={img}'):
                print(f"FAIL: could not start emulator ({how})")
                return False
            proc = emu.process
            emu.ipc.pause()
            ok, resp = emu.ipc.send_command(
                'mem write 0x4000 ' + IDE_WRITE_SECTOR_0.hex().upper())
            if not ok:
                print(f"FAIL: {how}: mem write: {resp}")
                return False
            ok, resp = emu.ipc.send_command('reg set PC 0x4000')
            if not ok:
                print(f"FAIL: {how}: reg set PC: {resp}")
                return False
            if not emu.ipc.wait_pc(IDE_WRITE_PARK, 5000):
                print(f"FAIL: {how}: the IDE write never finished")
                return False
            if _read_bytes(img)[:512] != bytes(512):
                print(f"FAIL: {how}: the write reached the file before the "
                      "exit; the test proves nothing")
                return False
            if how == 'quit':
                emu.ipc.send_command('quit')
            else:
                proc.send_signal(signal.SIGTERM)
            try:
                proc.wait(timeout=30)
            except subprocess.TimeoutExpired:
                print(f"FAIL: {how}: the emulator did not exit")
                return False
            data = _read_bytes(img)
            if data[:512] != b'\xA5' * 512:
                print(f"FAIL: {how}: the IDE image's unsaved write was "
                      f"dropped at exit; sector 0 starts {data[:8].hex()}")
                return False
            if len(data) != 64 * 1024 or data[512:] != bytes(len(data) - 512):
                print(f"FAIL: {how}: the rest of the IDE image changed")
                return False
    print("PASS: IPC quit and SIGTERM write the IDE image's unsaved writes "
          "back")
    return True


def test_profile_load_missing_keeps_running():
    """profile load ERR must restore a running machine (beads-csl7.2).

    CpcPauseLease destructor only drops the lease count; 13db3b7c added
    restore_run_state on load failure. A missing profile must return ERR and
    leave the Z80 advancing — wait vbl resumes the machine itself and would
    pass even if left paused, so this asserts PC motion via pc_is_moving(),
    which polls rather than comparing one before/after pair (see that helper
    for why).
    """
    print("Running profile-load missing-name resume test...")

    with EmulatorRunner() as emu:
        if not emu.start():
            print("FAIL: Could not start emulator")
            return False

        moved, pc1, pc2 = pc_is_moving(emu.ipc)
        if not moved:
            print(f"FAIL: PC already frozen before load ({pc1} / {pc2})")
            return False

        ok, resp = emu.ipc.send_command('profile load no-such-profile-csl7')
        if ok:
            print(f"FAIL: missing profile unexpectedly succeeded: {resp}")
            return False
        if not resp.startswith('ERR'):
            print(f"FAIL: expected ERR for missing profile, got {resp!r}")
            return False

        moved, pc3, pc4 = pc_is_moving(emu.ipc)
        if not moved:
            print(f"FAIL: machine left paused after profile load ERR "
                  f"({pc3} / {pc4}); resp={resp!r}")
            return False

        print(f"  load ERR: {resp.strip()}")
        print(f"  PC still moves: {pc3.strip()} -> {pc4.strip()}")
        print("PASS: profile load ERR left the machine running")
        return True


def test_boots_to_basic_with_peripherals():
    """The CPC must reach the BASIC prompt with peripherals configured.

    Nothing else in this suite boots the configured machine and checks that it
    arrives. That gap let a real crashloop ship: anything attached into an
    expansion ROM slot gets initialised by the firmware's ROM scan at boot, so
    fitting a peripheral's ROM whose Device is not wired up runs its boot code
    against absent hardware and the machine resets forever, never reaching
    BASIC. Every unit test still passed, because the defect was in which slots
    get fitted, not in the fitting.

    The assertion is the BASIC prompt itself, read off the telnet console —
    model-independent, and exactly the thing the user loses when this breaks.
    """
    with EmulatorRunner() as emu:
        # M4 on: it owns a ROM slot and auto-loads an image into it, which is
        # the configuration that broke.
        if not emu.start('-O', 'peripheral.m4board=1'):
            print("  Failed to start emulator")
            return False

        port = emu.await_logged_port('Telnet console', timeout=10.0)
        if port is None:
            print("  SKIP: emulator logged no telnet port")
            return True

        banner = b''
        try:
            with socket.create_connection(('127.0.0.1', port), timeout=5.0) as sock:
                sock.settimeout(1.0)
                deadline = time.monotonic() + 15.0
                while time.monotonic() < deadline and b'Ready' not in banner:
                    try:
                        chunk = sock.recv(4096)
                    except socket.timeout:
                        continue
                    if not chunk:
                        break
                    banner += chunk
        except OSError as e:
            print(f"  SKIP: could not reach the telnet console: {e}")
            return True

        if b'Ready' not in banner:
            print(f"  CPC never reached the BASIC prompt. Console said: "
                  f"{banner[:200]!r}")
            return False
        print("  CPC reached the BASIC prompt")
        return True


def test_serial_ipc_drives_the_live_card():
    """`serial send` must reach the CPU through the card the board clocks.

    `serial send`, `send_string` and `status` used to talk to a DART object
    the CPU never addressed: the bytes queued where no IN instruction could
    read them, and `status` reported that object's permanently empty FIFO.
    Here a Z80 program polls the live card's RR0 and reads its data port, so
    the bytes count only if they really arrive -- and it echoes each one back
    out of the data port into a File backend, the CPC-to-host direction.

    The program waits on a flag first, so `status` can be read while the
    bytes sit in the card: three in its RX FIFO, the rest queued host-side
    until there is room (the FIFO is three deep; pushing more overruns it).
    """
    print("Running serial-on-the-live-card test...")
    program = bytes([
        0xF3,              # 6000 DI
        0xAF,              # 6001 XOR A
        0x32, 0xFF, 0x7F,  # 6002 LD (&7FFF),A   clear the go flag
        0x3A, 0xFF, 0x7F,  # 6005 wait: LD A,(&7FFF)
        0xB7,              # 6008 OR A
        0x28, 0xFA,        # 6009 JR Z,wait
        0x21, 0x00, 0x70,  # 600B LD HL,&7000
        0x1E, 0x05,        # 600E LD E,5
        0x01, 0xDE, 0xFA,  # 6010 rx: LD BC,&FADE  DART ch A control
        0xED, 0x78,        # 6013 IN A,(C)         RR0
        0xE6, 0x01,        # 6015 AND 1            RX character available
        0x28, 0xF7,        # 6017 JR Z,rx
        0x01, 0xDC, 0xFA,  # 6019 LD BC,&FADC      DART ch A data
        0xED, 0x78,        # 601C IN A,(C)
        0x77,              # 601E LD (HL),A
        0x23,              # 601F INC HL
        0xED, 0x79,        # 6020 OUT (C),A        echo it back out
        0x1D,              # 6022 DEC E
        0x20, 0xEB,        # 6023 JR NZ,rx
        0x18, 0xFE,        # 6025 done: JR done
    ])
    done = 0x6025
    workdir = tempfile.mkdtemp(prefix='koncpc_serial_')
    bin_path = os.path.join(workdir, 'serial.bin')
    out_path = os.path.join(workdir, 'serial_out.bin')
    with open(bin_path, 'wb') as f:
        f.write(program)

    def status_fields(resp: str) -> dict:
        return dict(p.split('=', 1) for p in resp.split()[1:] if '=' in p)

    try:
        with EmulatorRunner() as emu:
            if not emu.start('-O', 'peripheral.serial_enabled=1',
                             '-O', 'peripheral.serial_backend=1',  # File
                             '-O', f'peripheral.serial_output_file={out_path}',
                             '-i', bin_path, '-o', '0x6000'):
                print("FAIL: Could not start emulator")
                return False
            ipc = emu.ipc
            ipc.timeout = 20.0
            ok, resp = ipc.send_command('wait pc 0x6005 15000')
            if not ok:
                print(f"FAIL: program never started: {resp}")
                return False
            ipc.run()

            ok, resp = ipc.send_command('serial send_string HELL')
            if not ok or 'sent 4 bytes' not in resp:
                print(f"FAIL: send_string: {resp}")
                return False
            ok, resp = ipc.send_command('serial send 0x4F')  # 'O'
            if not ok:
                print(f"FAIL: send: {resp}")
                return False
            ipc.wait_vbl(10)

            ok, resp = ipc.send_command('serial status')
            st = status_fields(resp)
            print(f"  before the CPU reads: {resp}")
            if not ok or st.get('rx_available') != '1' \
                    or st.get('rx_fifo') != '3' or st.get('rx_pending') != '2' \
                    or st.get('rx_overrun') != '0':
                print("FAIL: status should show 3 bytes in the card's FIFO, "
                      "2 queued, no overrun")
                return False

            ipc.send_command('mem write 0x7FFF 01')
            ok, resp = ipc.send_command(f'wait pc 0x{done:04X} 10000')
            if not ok:
                print(f"FAIL: the CPU never received all five bytes: {resp}")
                return False

            ok, resp = ipc.read_mem(0x7000, 5)
            if '48454C4C4F' not in resp.upper().replace(' ', ''):
                print(f"FAIL: the CPU read the wrong bytes: {resp}")
                return False

            ok, resp = ipc.send_command('serial status')
            st = status_fields(resp)
            print(f"  after: {resp}")
            if st.get('rx_available') != '0' or st.get('rx_pending') != '0' \
                    or st.get('tx_dropped') != '0':
                print("FAIL: status should show the FIFO and queue drained")
                return False

            with open(out_path, 'rb') as f:
                echoed = f.read()
            if echoed != b'HELLO':
                print(f"FAIL: the CPC's echo reached the backend as {echoed!r}")
                return False
            print("PASS: the CPU read HELLO off the live card and echoed it "
                  "to the backend")
            return True
    finally:
        shutil.rmtree(workdir, ignore_errors=True)


def test_debugger_stop_contract():
    """When the debugger says it stopped, it must actually have stopped there.

    Three claims that were each broken at some point and are cheap to pin:
      * `input key` must not change whether the emulator is running. The tap
        rides a frame step that ends in cpc_pause(), so without an explicit
        restore a tap STOPS a running machine and the firmware never scans the
        key.
      * After `wait bp`, `reg get PC` must equal the address armed. The hit
        identity is published by process_probe_hit() and then overwritten by
        subcycle_bridge_sync_regs_view(), so it has to be re-applied.
      * `wait bp` must not report a hit left over from a PREVIOUS arming --
        that is how a test can pass while measuring nothing.
    """
    print("Running debugger stop-contract test...")

    with EmulatorRunner() as emu:
        if not emu.start('-O', 'system.run_tier=4'):
            print("  Failed to start emulator")
            return False
        time.sleep(5)

        def paused() -> Optional[str]:
            ok, resp = emu.ipc.send_command('status')
            if not ok:
                return None
            parts = [t for t in resp.split() if t.startswith('paused=')]
            return parts[0] if parts else None

        emu.ipc.send_command('bp clear')
        emu.ipc.send_command('wp clear')
        emu.ipc.send_command('run')
        time.sleep(0.5)

        # 1. a tap leaves a running machine running
        emu.ipc.send_command('input key a')
        state = paused()
        if state != 'paused=0':
            print(f"  FAIL: 'input key' left the machine {state} (want paused=0)")
            return False
        print("  tap leaves a running machine running: OK")

        # 2. the reported PC is the breakpoint address, not the fetch past it
        emu.ipc.send_command('bp clear')
        emu.ipc.send_command('run')
        time.sleep(0.3)
        emu.ipc.send_command('bp add 0x1BD9')
        ok, resp = emu.ipc.send_command('wait bp 4000')
        if not ok:
            print(f"  FAIL: plain bp at the idle poll never fired: {resp!r}")
            return False
        ok, regs = emu.ipc.send_command('regs')
        pc = next((t.split('=')[1] for t in regs.split()
                   if t.startswith('PC=')), None)
        if pc is None or int(pc, 16) != 0x1BD9:
            print(f"  FAIL: after wait bp, reg PC={pc} (want 1BD9)")
            return False
        print("  breakpoint PC survives the register sync: OK")

        # 3. a hit from a previous arming must not answer the next wait
        emu.ipc.send_command('bp clear')   # drops the arming the hit came from
        emu.ipc.send_command('run')
        time.sleep(0.3)
        emu.ipc.send_command('bp add 0x0000')  # never executed at the prompt
        ok, _ = emu.ipc.send_command('wait bp 1500')
        if ok:
            print("  FAIL: wait bp reported a stale hit from a previous arming")
            return False
        print("  stale hits are not reported as fresh: OK")

        # 4. a stop staged before a timeout must not overtake the Run that
        # follows it. A zero-budget wait at the hot idle poll forces both
        # orderings over repeated attempts.
        saw_timeout = False
        for _ in range(50):
            emu.ipc.send_command('bp clear')
            emu.ipc.send_command('run')
            emu.ipc.send_command('bp add 0x1BD9')
            hit, _ = emu.ipc.send_command('wait bp 0')
            if hit:
                emu.ipc.send_command('run')
                continue
            saw_timeout = True
            emu.ipc.send_command('bp clear')
            emu.ipc.send_command('run')
            time.sleep(0.1)
            state = paused()
            if state != 'paused=0':
                print(
                    "  FAIL: an expired breakpoint stop overtook the "
                    f"following run ({state})")
                return False
            break
        if not saw_timeout:
            print("  FAIL: could not exercise wait-bp timeout ordering")
            return False
        print("  expired stop cannot overtake a later run: OK")

        emu.ipc.send_command('bp add 0x1BD9')
        ok, _ = emu.ipc.send_command('wait bp 4000')
        if not ok or paused() != 'paused=1':
            print("  FAIL: committed breakpoint was not observably paused")
            return False
        print("  committed hit is published only after pause: OK")

        emu.ipc.send_command('bp clear')
        emu.ipc.send_command('run')

    print("PASS: debugger stop-contract test")
    return True


def test_conditional_debug_matrix():
    """Conditional breakpoints and watchpoints must judge hits honestly.

    beads-tib2 pinned four lies in one sweep: `if carry` armed cleanly as an
    unknown identifier and never fired; `if pc == <addr>` was false at its
    own breakpoint (the view's PC was mid-fetch at post-filter time); EVERY
    armed watchpoint's hits were silently resumed (the filter returned
    watchpoints.empty()); and clears issued at a pause looked like they
    leaked into later arms. This matrix drives all four through the live IPC
    against the firmware's own activity: 0x1BD9 is the BASIC idle loop's
    char-poll (constantly executed), 0xB8B4 is the firmware TIME counter
    (written ~300/s at idle).
    """
    with EmulatorRunner() as emu:
        if not emu.start('-O', 'system.run_tier=4'):
            print("  Failed to start emulator")
            return False
        # Every check below asks the server to wait up to 5-6s, and this runs
        # at the slowest tier. With the 5.0s default socket timeout the client
        # abandons the connection first, so a server-side budget of 5000ms or
        # more can never be observed: a slow-but-correct hit and a breakpoint
        # that never fires produce the identical "silent". The checks passed
        # only because hits normally land in well under a second — on a loaded
        # runner they do not, and the last one reported "plain bp did not
        # fire" for what may only have been the client giving up. Give the
        # socket room so every budget here is observable and a real miss is
        # reported as a real miss.
        emu.ipc.timeout = 20.0
        time.sleep(5)  # let the firmware reach its idle loop

        def fires(arm_cmd, timeout_ms=5000):
            ok, resp = emu.ipc.send_command(arm_cmd)
            if not ok:
                return 'ERR'
            ok, resp = emu.ipc.send_command(f'wait bp {timeout_ms}')
            return 'FIRES' if ok else 'silent'

        def reset_state():
            emu.ipc.send_command('bp clear')
            emu.ipc.send_command('wp clear')
            emu.ipc.send_command('run')
            time.sleep(0.5)
            # Establish the precondition instead of assuming it. A hit that
            # lands just AFTER a `wait bp` times out still pauses the machine,
            # and that pause is applied by the main loop strictly later than
            # the resume we just sent -- so `run` can be overtaken and the next
            # check then arms against a stopped machine and sees nothing. That
            # product-level race is beads-6561; here we simply refuse to start
            # a check until the emulator is really running.
            deadline = time.monotonic() + 5.0
            while time.monotonic() < deadline:
                ok, resp = emu.ipc.send_command('status')
                if ok and 'paused=0' in resp:
                    return
                emu.ipc.send_command('run')
                time.sleep(0.1)
            print("  WARNING: emulator would not resume before the next check")

        checks = [
            ('bp add 0x1BD9 if pc == 0x1BD9', 'FIRES',
             "true condition at its own breakpoint"),
            ('bp add 0x1BD9 if pc != 0x1BD9', 'silent',
             "false condition stays silent"),
            ('bp add 0x1BD9 if carrry', 'ERR',
             "unknown identifier refused at arm time"),
            ('wp add 0xB8B4 1 w', 'FIRES',
             "a plain watchpoint fires on the TIME counter"),
            ('wp add 0xB8B4 1 w if value < 256', 'FIRES',
             "watch condition sees the hit value"),
            ('wp add 0xB8B4 1 w if 0', 'silent',
             "false watch condition stays silent"),
        ]
        for arm, want, label in checks:
            got = fires(arm, 2500 if want == 'silent' else 6000)
            reset_state()
            if got != want:
                print(f"  FAIL {label}: {arm} -> {got} (want {want})")
                return False
            print(f"  {label}: {got}")

        # The original beads-tib2 repro, live: `if carry` at the idle loop's
        # char-poll return (0x1BD9). Carry is set there exactly when KM READ
        # CHAR hands back a fetched key, so pressing one guarantees a
        # carry-true hit -- on the pre-fix tree this armed OK and never fired.
        #
        # This assertion was briefly rewritten to claim the opposite (that
        # carry is NEVER set here), because a regression made the condition
        # evaluator read the PREVIOUS frame's flags: hits judged in
        # subcycle_bridge_frame()'s continue loop were evaluated before
        # subcycle_bridge_sync_regs_view() refreshed z80.AF. With the sync
        # restored, `if carry` fires on a delivered key again. Do not weaken
        # this assertion to match a green run -- it is the canary for exactly
        # that class of stale-mirror bug (beads-6561).
        #
        # 4000ms is plenty for a delivered key; the socket timeout raised at
        # the top of this test is what makes any budget here observable at all.
        ok, _ = emu.ipc.send_command('bp add 0x1BD9 if carry')
        if not ok:
            print("  FAIL: 'if carry' refused at arm time")
            return False
        emu.ipc.send_command('input key a')
        ok, _ = emu.ipc.send_command('wait bp 4000')
        reset_state()
        if not ok:
            print("  FAIL: 'if carry' never fired on a delivered key")
            return False
        print("  carry condition fires on a delivered key: FIRES")

        # Clear-promptness: a cleared breakpoint must not fire after resume.
        got = fires('bp add 0x1BD9')
        if got != 'FIRES':
            print("  FAIL: plain bp did not fire")
            return False
        reset_state()  # clear at the pause, then resume
        ok, _ = emu.ipc.send_command('wait bp 2500')
        if ok:
            print("  FAIL: a cleared breakpoint fired after resume")
            return False
        print("  cleared breakpoint stays cleared after resume")
        return True


def test_wait_bp_reports_an_io_stop_as_one():
    """`wait bp` says an I/O breakpoint stopped the machine, and where.

    beads-llnn: the probe's I/O hit went out through the watchpoint fields, so
    `wait bp` answered `OK PC=7F89 WATCH=1 WP_ADDR=7F89 ...` for the firmware's
    Gate Array write: a watchpoint that nobody set, and a PC that was the port
    (B=7F, C=89), not an address the CPU was executing. The reply must name the
    I/O breakpoint (`IO=1 PORT= DIR=OUT VAL=`), and its PC must be the one the
    registers show, just past the OUT that made the access.
    """
    with EmulatorRunner() as emu:
        if not emu.start():
            print("  Failed to start emulator")
            return False
        emu.ipc.timeout = 20.0
        time.sleep(3)  # let the firmware reach its idle loop

        ok, resp = emu.ipc.send_command('iobp add 0x7F00 0xFF00 out')
        if not ok:
            print(f"  FAIL: iobp add out: {resp}")
            return False
        ok, resp = emu.ipc.send_command('wait bp 5000')
        emu.ipc.send_command('iobp clear')
        if not ok:
            print(f"  FAIL: the Gate Array write never stopped: {resp}")
            return False
        fields = dict(t.split('=', 1) for t in resp.split()[1:] if '=' in t)
        print(f"  wait bp: {resp.strip()}")
        problems = []
        if fields.get('WATCH') != '0':
            problems.append("an I/O stop reported as a watchpoint")
        if fields.get('IO') != '1' or fields.get('DIR') != 'OUT':
            problems.append("no IO=1 DIR=OUT")
        port = int(fields.get('PORT', '0'), 16)
        if port & 0xFF00 != 0x7F00:
            problems.append(f"PORT={port:04X} is not the Gate Array's")
        if 'VAL' not in fields:
            problems.append("an OUT without the byte it wrote")
        pc = int(fields.get('PC', '0'), 16)
        ok, reg = emu.ipc.send_command('reg get PC')
        if not ok or int(reg.split()[1], 16) != pc:
            problems.append(f"PC={pc:04X} but the registers show {reg!r}")
        # The OUT is the instruction just behind PC: OUT (C),r / OUTI / OTIR
        # (ED xx) or OUT (n),A (D3 nn), both two bytes long.
        ok, mem = emu.ipc.send_command(f'mem read {(pc - 2) & 0xFFFF:#06x} 2')
        opcode = mem.split()[1][:2].upper() if ok else ''
        if opcode not in ('ED', 'D3'):
            problems.append(f"no OUT at PC-2 ({mem!r}): PC is not the code's")
        emu.ipc.send_command('run')
        if problems:
            print(f"  FAIL: {'; '.join(problems)}")
            return False
        return True


def test_iobp_second_entry_on_same_port_fires():
    """Two I/O breakpoints on one port and mask, one per direction, both arm.

    beads-lj1t: `iobp add 0x7F00 0xFF00 in` then `iobp add 0x7F00 0xFF00 out`
    are two entries in the list, and the probe has one comparator for the
    pair. It kept the first entry's direction and dropped the second, so the
    OUT breakpoint was listed and never fired. The firmware writes the Gate
    Array (&7Fxx) on every interrupt and never reads it, so the IN entry alone
    must stay silent and the pair must stop on a write.
    """
    with EmulatorRunner() as emu:
        if not emu.start():
            print("  Failed to start emulator")
            return False
        emu.ipc.timeout = 20.0
        time.sleep(3)  # let the firmware reach its idle loop

        ok, resp = emu.ipc.send_command('iobp add 0x7F00 0xFF00 in')
        if not ok:
            print(f"  FAIL: iobp add in: {resp}")
            return False
        ok, resp = emu.ipc.send_command('wait bp 1500')
        if ok:
            print(f"  FAIL: nothing reads the Gate Array, yet IN fired: {resp}")
            return False

        ok, resp = emu.ipc.send_command('iobp add 0x7F00 0xFF00 out')
        if not ok:
            print(f"  FAIL: iobp add out: {resp}")
            return False
        ok, resp = emu.ipc.send_command('iobp list')
        if not ok or 'count=2' not in resp or '/in' not in resp \
                or '/out' not in resp:
            print(f"  FAIL: expected the two entries listed: {resp}")
            return False
        ok, resp = emu.ipc.send_command('wait bp 5000')
        if not ok:
            print(f"  FAIL: the OUT breakpoint on the shared port never "
                  f"fired: {resp}")
            return False
        print(f"  second entry on the same port fires: {resp.strip()}")

        # Deleting the OUT entry disarms its direction again: the comparator
        # is rebuilt from the list, not left with the merged directions.
        emu.ipc.send_command('iobp del 1')
        emu.ipc.send_command('run')
        ok, resp = emu.ipc.send_command('wait bp 1500')
        if ok:
            print(f"  FAIL: OUT still fires after its entry was deleted: "
                  f"{resp}")
            return False
        emu.ipc.send_command('iobp clear')
        emu.ipc.send_command('run')
        return True


def test_pc_and_mem_events_fire():
    """`event on|once pc=` / `mem=` must run their command (beads-uj1c).

    The handlers existed but nothing called them, so both triggers armed with
    `OK id=N` and never fired. The planted loop at 0x6000 is
        6000 LD A,0x42   6002 LD (0x5000),A   6005 JR 0x6000
    and each event's command is a `mem write` this test reads back. It runs
    on the default tier: an event must fire on the Fast tier too, without
    turning into a stop -- `wait vbl` answers ERR 409 if an event-only hit
    paused the machine like a breakpoint.
    """
    with EmulatorRunner() as emu:
        if not emu.start():
            print("  Failed to start emulator")
            return False
        emu.ipc.timeout = 20.0
        emu.ipc.pause()
        for command in ['mem write 0x6000 3E42320050 18F9',
                        'mem write 0x5000 00',
                        'mem write 0x6100 00000000',
                        'reg set PC 0x6000']:
            ok, resp = emu.ipc.send_command(command)
            if not ok:
                print(f"  FAIL: {command!r}: {resp}")
                return False
        arms = [
            # id 1: one-shot on the store's opcode fetch.
            'event once pc=0x6002 mem write 0x6100 AA',
            # id 2: persistent, value-matched write.
            'event on mem=0x5000:0x42 mem write 0x6101 BB',
            # id 3: value mismatch -- must never fire.
            'event once mem=0x5000:0x43 mem write 0x6102 CC',
            # id 4: removes itself from inside its own command (the event
            # lock must not be held while a command runs).
            'event on pc=0x6005 event off 4',
        ]
        for command in arms:
            ok, resp = emu.ipc.send_command(command)
            if not ok:
                print(f"  FAIL: {command!r}: {resp}")
                return False
        ok, resp = emu.ipc.send_command('wait vbl 10 10000')
        if not ok:
            print(f"  FAIL: an event-only hit stopped the machine: {resp}")
            return False
        ok, mem = emu.ipc.read_mem(0x6100, 3)
        if not ok:
            print(f"  FAIL: mem read: {mem}")
            return False
        got = mem.replace(' ', '').upper()
        if 'AABB00' not in got:
            print(f"  FAIL: event side effects at 0x6100 = {mem!r}, "
                  "want AA BB 00 (pc= fired, mem=:42 fired, mem=:43 did not)")
            return False
        ok, listing = emu.ipc.send_command('event list')
        if not ok or 'count=2' not in listing or 'id=2 ' not in listing \
                or 'id=3 ' not in listing:
            print(f"  FAIL: one-shots must go, the rest must stay: {listing!r}")
            return False
        print("  pc= and mem= events fired; one-shots removed; no stop")
        return True


class TelnetOracle:
    """Reader for the emulator's telnet console (IPC port + 1).

    The console mirrors firmware OUTPUT only. The line editor's echo of the
    characters you inject does NOT route through TXT_OUTPUT (&BB5A), so nothing
    you send with `autotype` ever appears on this stream. Searching it for your
    own keystrokes therefore fails no matter how well typing works -- that
    mistake is what made beads-qgxr look like a keyboard defect for three
    sessions. `type_and_expect` refuses such a search outright.

    Assert on what the program PRINTS. To check input actually landed, read the
    screen back (`screenshot`, with an idle control) or the firmware key-state
    map at B635.
    """

    def __init__(self, sock):
        self.sock = sock
        self.sock.settimeout(1.0)
        self.data = b''

    def read_until(self, marker: bytes, budget: float) -> bool:
        deadline = time.monotonic() + budget
        while time.monotonic() < deadline:
            if marker in self.data:
                return True
            try:
                chunk = self.sock.recv(4096)
            except socket.timeout:
                continue
            if not chunk:
                return marker in self.data
            self.data += chunk
        return marker in self.data

    def mark(self) -> int:
        """Offset to slice from, so assertions ignore earlier boot output."""
        return len(self.data)

    def read_until_since(self, mark: int, marker: bytes,
                         budget: float) -> bool:
        """Like read_until, but only output produced after `mark` counts.

        read_until() searches the whole accumulated buffer, so a marker that
        already appeared during boot (`Ready` is the obvious one) returns
        instantly and the caller sees an empty slice -- a green that measured
        nothing. Anything waiting on repeated output must use this.
        """
        deadline = time.monotonic() + budget
        while time.monotonic() < deadline:
            if marker in self.data[mark:]:
                return True
            try:
                chunk = self.sock.recv(4096)
            except socket.timeout:
                continue
            if not chunk:
                return marker in self.data[mark:]
            self.data += chunk
        return marker in self.data[mark:]

    def since(self, mark: int) -> bytes:
        return self.data[mark:]

    def type_and_expect(self, ipc, text: str, marker: bytes,
                        budget: float = 20.0):
        """Type `text`, then wait for `marker` in output produced after it.

        Returns (found, mark) so the caller can slice the response with
        `since(mark)` instead of searching for the typed command, which is
        never echoed here.
        """
        typed = text.encode() if isinstance(text, str) else text
        if marker and marker in typed:
            raise AssertionError(
                f"marker {marker!r} occurs in the typed text {typed!r}. The "
                "telnet console mirrors firmware OUTPUT only and never echoes "
                "injected keystrokes, so this search can only ever fail. "
                "Assert on something the program prints instead.")
        start = self.mark()
        ok, resp = ipc.send_command(f"autotype {text}")
        if not ok:
            print(f"  autotype was refused: {resp}")
            return False, start
        return self.read_until_since(start, marker, budget), start


def test_m4_cat_lists_the_sd_card():
    """`cat` over the M4 must list the SD card's actual contents.

    The one test that would have caught the whole M4 saga end-to-end. The M4
    shipped through four separate defects — a ROM filename the project never
    shipped, an unpatched ROM image, command frames spliced in the mailbox,
    and a ready-poll defeated by frame-latency answers — and 100+ unit tests
    passed through all of them, because each tested its component in
    isolation. The CPC-visible truth is this: a two-entry SD card, `cat`, and
    the two entries on the console, spelled right, exactly once.
    """
    # The M4 ROM is user-supplied and gitignored (rom/ or resources/roms/), so
    # a clean checkout -- and every CI job -- simply has none. Without it the
    # board never attaches, `cat` falls through to AMSDOS and the CPC answers
    # "Drive A: disc missing". That is a missing asset, not a defect, and the
    # unit-level M4 tests already GTEST_SKIP on exactly this condition
    # (test/m4_rom_fitting_test.cpp:56,101). Match them rather than hard-fail:
    # this test used to report False while printing nothing at all.
    # Same filenames and same search order as m4board_find_rom() in
    # src/m4board.cpp: rom/ first, then resources/roms/.
    repo_root = Path(__file__).parent.parent.parent
    have_rom = any((repo_root / d / name).exists()
                   for d in ('rom', 'resources/roms')
                   for name in ('m4board.rom', 'M4ROM.BIN'))
    if not have_rom:
        print("  SKIP: no M4 ROM present (user-supplied, gitignored)")
        return True

    sd = tempfile.mkdtemp(prefix='koncpc_m4sd_')
    try:
        with open(os.path.join(sd, 'readme.txt'), 'w') as f:
            f.write('hello from the harness\n')
        os.mkdir(os.path.join(sd, 'games'))

        with EmulatorRunner() as emu:
            if not emu.start('-O', 'peripheral.m4board=1',
                             '-O', f'peripheral.m4_sd_path={sd}'):
                print("  Failed to start emulator")
                return False

            tport = emu.await_logged_port('Telnet console', timeout=10.0)
            if tport is None:
                print("  SKIP: emulator logged no telnet port")
                return True

            try:
                with socket.create_connection(('127.0.0.1', tport),
                                              timeout=5.0) as sock:
                    con = TelnetOracle(sock)

                    if not con.read_until(b'Ready', 20.0):
                        print(f"  CPC never reached BASIC: {con.data[:200]!r}")
                        return False

                    # Let the firmware settle at the prompt before typing.
                    # Immediately after 'Ready' the keyboard handler can still
                    # eat leading characters (the `un3hello` class): typed too
                    # early, 'cat' arrives as a bare RETURN.
                    time.sleep(3.0)
                    # NOT quoted: `autotype` types everything after the
                    # first space literally (see `help autotype`). Wrapping the
                    # text in apostrophes types them too, and a leading ' is a
                    # BASIC comment marker -- the line then silently does
                    # nothing, which is what made this test look like a
                    # keyboard bug (beads-qgxr).
                    # 'free' is the tail of the M4's catalogue footer -- an
                    # assertion on what the M4 PRINTS. type_and_expect refuses
                    # a marker taken from the typed text, because the console
                    # never echoes injected keystrokes (beads-gosg).
                    found, listing_from = con.type_and_expect(
                        emu.ipc, 'cat~RETURN~', b'free', 20.0)
                    if not found:
                        print(f"  cat never completed: {con.data[-300:]!r}")
                        return False
            except OSError as e:
                print(f"  SKIP: could not reach the telnet console: {e}")
                return True

            listing = con.since(listing_from)
            problems = []
            if b'README  .TXT' not in listing:
                problems.append("README  .TXT missing")
            if b'GAMES' not in listing:
                problems.append("GAMES missing")
            if b'<DIR>' not in listing:
                problems.append("<DIR> marker missing")
            if listing.count(b'README') != 1:
                problems.append(
                    f"README listed {listing.count(b'README')} times "
                    "(stale-response duplication)")
            if b'READM ' in listing or b'READM\t' in listing:
                problems.append("torn entry 'READM' present (window tear)")
            if listing.count(b'+') > 10:
                problems.append(
                    f"{listing.count(b'+')} '+' glyphs (poll-timeout flood)")
            if problems:
                print(f"  M4 catalogue corrupt: {'; '.join(problems)}")
                print(f"  console: {listing[:400]!r}")
                return False
            print("  M4 cat listed the SD card correctly")
            return True
    finally:
        shutil.rmtree(sd, ignore_errors=True)


def main():
    """Run all IPC tests."""
    print("=" * 50)
    print("konCePCja IPC Test Harness")
    print("=" * 50)

    tests = [
        test_wait_vbl_counts_real_frames,
        test_hash_frame_is_the_machine_frame,
        test_wait_vbl_timeout_ends_stopped,
        test_pause_replies_once_stopped,
        test_config_set_machine_keys_on_a_running_machine,
        test_sdisc_commands_reach_the_running_machine,
        test_sdisc_enabled_at_run_time_is_fitted,
        test_wait_pc_timeout_ends_stopped,
        test_wait_pc_is_exact,
        test_wait_pc_aborts_on_shutdown,
        test_run_to_address_breakpoint_polarity,
        test_inject_launches_like_run,
        test_boots_to_basic_with_peripherals,
        test_serial_ipc_drives_the_live_card,
        test_conditional_debug_matrix,
        test_iobp_second_entry_on_same_port_fires,
        test_wait_bp_reports_an_io_stop_as_one,
        test_pc_and_mem_events_fire,
        test_debugger_stop_contract,
        test_m4_cat_lists_the_sd_card,
        test_model_change_rebuild,
        test_cartridge_hotload_reattaches,
        test_profile_load_rebuilds_machine,
        test_profile_load_missing_keeps_running,
        test_disk_live_put_cat,
        test_disk_put_basic_listing_runs,
        test_frames_dump_stops_at_a_breakpoint,
        test_disk_status_save_eject,
        test_disk_eject_flushes_dirty_writes,
        test_disk_load_over_dirty_disc_flushes_to_its_own_file,
        test_disk_failed_load_does_not_redirect_write_back,
        test_zipped_disc_write_back_goes_beside_the_archive,
        test_zipped_disc_keeps_its_writes_over_a_machine_rebuild,
        test_zipped_disc_from_the_command_line_writes_beside_the_archive,
        test_zipped_disc_unwritable_directory_leaves_the_archive_alone,
        test_scripted_exit_writes_back_unsaved_disc_writes,
        test_exit_puts_the_zipped_disc_written_back_on_recent,
        test_scripted_exit_writes_back_unsaved_ide_writes,
        test_headless_runs_subcycle_engine,
        test_headless_honours_the_speed_limiter,
        test_programmatic_quit_exit_codes,
        test_double_signal_terminates,
        test_engine1_bp_clear_resume,
        test_z80_basic,
        test_memory_rw,
        test_mem_ram_view_under_rom_overlay,
        test_breakpoint,
        # Thread-split correctness tests (work in both headless and threaded mode)
        test_breakpoint_pause_step_resume,
        test_wait_bp_drops_a_hit_the_machine_has_run_past,
        test_snapshot_round_trip,
        test_rapid_pause_resume,
        test_reg_set_halt_is_refused,
        test_step_in_accuracy,
        test_step_out_nested_call,
        test_step_out_gated_on_ret_not_sp,
        test_step_out_untaken_conditional_ret,
        test_step_out_pop_then_call,
        test_step_out_tail_jumping_restart,
        test_step_out_nested_restarts,
        test_step_out_computed_return,
        test_step_out_never_returns_times_out_honestly,
        test_step_out_stops_at_breakpoint_inside_own_frame,
        test_step_out_breakpoint_stop_is_one_arrival,
        test_step_out_stops_at_real_breakpoint_on_landing_address,
        test_mouse_input,
        test_gun_input,
        test_chord_hold_input,
        test_type_input,
        test_input_state,
        test_joystick_input,
        test_load_accepts_flux_disk_formats,
    ]

    passed = 0
    failed = 0

    # The sub-cycle board is the only engine (Gate C Wave 1 deleted the
    # legacy core), so the old dual-engine loop is gone.
    EmulatorRunner.test_engine = 1
    # The runner reports the name and outcome of every test itself. Relying on
    # each test to announce itself meant three of them printed nothing at all,
    # so a test could return False and the only trace was the arithmetic in the
    # summary -- a silent failure in a suite that is meant to be a gate.
    failures = []
    for test in tests:
        name = test.__name__
        try:
            if test():
                passed += 1
                print(f"  -> {name}: PASS")
            else:
                failed += 1
                failures.append(name)
                print(f"  -> {name}: FAIL")
        except Exception as e:
            print(f"FAIL: {name} raised {e}")
            failed += 1
            failures.append(name)
            print(f"  -> {name}: FAIL")
        print()

    print("=" * 50)
    print(f"Results: {passed} passed, {failed} failed")
    for name in failures:
        print(f"  FAILED: {name}")
    print("=" * 50)

    return 0 if failed == 0 else 1


if __name__ == '__main__':
    sys.exit(main())
