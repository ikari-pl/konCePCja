#!/usr/bin/env python3
"""
IPC Test Harness for konCePCja Emulator

Connects to the emulator's IPC server (port 6543) to run automated tests.
"""

import queue
import re
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

        Note: The server closes the connection after each command,
        so we create a new connection for each request.
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
        """Returns True if the emulator is running in non-headless (threaded) mode.

        'devtools' is a no-op in headless mode (returns ERR) but succeeds in GUI mode.
        The Z80/render thread split is only active in non-headless mode.
        """
        ok, _ = self.send_command('devtools')
        return ok


class EmulatorRunner:
    """Manages emulator process lifecycle."""

    test_engine: Optional[int] = None

    def __init__(self, exe_path: Optional[str] = None):
        if exe_path is None:
            # Find the executable relative to this script
            script_dir = Path(__file__).parent
            project_root = script_dir.parent.parent
            exe_path = str(project_root / 'koncepcja')
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
        'pause' hangs. ('wait vbl' is no use here — the server implements it as
        a fixed 20ms-per-count sleep, not a real blank wait.) A non-zero Z80 PC
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
        if headless:
            env['SDL_VIDEODRIVER'] = 'dummy'
            env['SDL_AUDIODRIVER'] = 'dummy'

        cmd = [self.exe_path] + list(args)
        # Pin the config unless the caller chose one. Started from the repo
        # root the emulator would otherwise pick up $CWD/koncepcja.cfg — the
        # maintainer's live config, which makes runs depend on whoever is
        # sitting at the machine. A throwaway copy of the shipped example also
        # keeps a mid-run save from editing the example itself.
        if not any(a == '-c' or a.startswith('-c') or a == '--cfg_file'
                   or a.startswith('--cfg_file=') for a in args):
            example = Path(self.exe_path).parent / 'koncepcja.cfg.example'
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
        ok1, pc1 = ipc.send_command('reg get PC')
        time.sleep(0.4)
        ok2, pc2 = ipc.send_command('reg get PC')
        if not (ok1 and ok2 and pc1 != pc2):
            print(f"FAIL: PC frozen headless ({pc1} / {pc2})")
            return False
        print("PASS: headless runs the sub-cycle engine (tier OK, PC moves)")
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

    Exercises cpc_pause_and_wait() in the IPC server's snapshot save/load paths.
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

    Exercises cpc_pause_and_wait() in the IPC step-in path: if the Z80 thread
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
    def rom_signature(ipc: KoncepcjaIPC) -> Optional[str]:
        ok, resp = ipc.read_mem(0x02E0, 16)
        if not ok:
            return None
        return resp.replace('OK ', '').strip().upper()

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

    def rom_signature(ipc: KoncepcjaIPC) -> Optional[str]:
        ok, resp = ipc.read_mem(0x02E0, 16)
        if not ok:
            return None
        return resp.replace('OK ', '').strip().upper()

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


def test_profile_load_missing_keeps_running():
    """profile load ERR must restore a running machine (beads-csl7.2).

    CpcPauseLease destructor only drops the lease count; 13db3b7c added
    restore_run_state on load failure. A missing profile must return ERR and
    leave the Z80 advancing — wait vbl is a fixed sleep and would pass even
    if paused, so this asserts PC motion the way the headless-engine test does.
    """
    print("Running profile-load missing-name resume test...")

    with EmulatorRunner() as emu:
        if not emu.start():
            print("FAIL: Could not start emulator")
            return False

        ok1, pc1 = emu.ipc.send_command('reg get PC')
        time.sleep(0.4)
        ok2, pc2 = emu.ipc.send_command('reg get PC')
        if not (ok1 and ok2 and pc1 != pc2):
            print(f"FAIL: PC already frozen before load ({pc1} / {pc2})")
            return False

        ok, resp = emu.ipc.send_command('profile load no-such-profile-csl7')
        if ok:
            print(f"FAIL: missing profile unexpectedly succeeded: {resp}")
            return False
        if not resp.startswith('ERR'):
            print(f"FAIL: expected ERR for missing profile, got {resp!r}")
            return False

        ok3, pc3 = emu.ipc.send_command('reg get PC')
        time.sleep(0.4)
        ok4, pc4 = emu.ipc.send_command('reg get PC')
        if not (ok3 and ok4 and pc3 != pc4):
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
        # 4000ms, not 6000: KoncepcjaIPC's own socket timeout is 5.0s, so a
        # longer server-side budget can never be observed by the client.
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
        test_boots_to_basic_with_peripherals,
        test_conditional_debug_matrix,
        test_debugger_stop_contract,
        test_m4_cat_lists_the_sd_card,
        test_model_change_rebuild,
        test_profile_load_rebuilds_machine,
        test_profile_load_missing_keeps_running,
        test_disk_live_put_cat,
        test_disk_status_save_eject,
        test_disk_eject_flushes_dirty_writes,
        test_headless_runs_subcycle_engine,
        test_engine1_bp_clear_resume,
        test_z80_basic,
        test_memory_rw,
        test_mem_ram_view_under_rom_overlay,
        test_breakpoint,
        # Thread-split correctness tests (work in both headless and threaded mode)
        test_breakpoint_pause_step_resume,
        test_snapshot_round_trip,
        test_rapid_pause_resume,
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
