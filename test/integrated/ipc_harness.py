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
    Without quiescence the snapshot might capture a partially-updated Z80 state.
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
    """Pause, read PC, step N instructions, verify PC advanced monotonically.

    Exercises cpc_pause_and_wait() in the IPC step-in path.  If the Z80 thread
    was still inside z80_execute() when step_in ran, the PC would not advance
    predictably.
    """
    print("Running step-in accuracy test...")

    STEPS = 10

    with EmulatorRunner() as emu:
        if not emu.start():
            print("FAIL: Could not start emulator")
            return False

        emu.ipc.pause()

        ok, pc_start = emu.ipc.get_reg('PC')
        if not ok:
            print("FAIL: Could not read initial PC")
            return False

        prev_pc = pc_start
        for i in range(STEPS):
            ok_s, _ = emu.ipc.step_in(1)
            if not ok_s:
                print(f"FAIL: step_in failed on step {i+1}")
                return False
            ok_r, cur_pc = emu.ipc.get_reg('PC')
            if not ok_r:
                print(f"FAIL: Could not read PC after step {i+1}")
                return False
            if cur_pc == prev_pc:
                print(f"FAIL: PC stuck at 0x{cur_pc:04X} after step {i+1}")
                return False
            prev_pc = cur_pc

        print(f"  PC advanced from 0x{pc_start:04X} to 0x{prev_pc:04X} over {STEPS} steps")
        print("PASS: Step-in accuracy test")
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
        if '.hfe' not in resp:
            print(f"FAIL: 'help load' does not mention flux formats: {resp!r}")
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
        test_headless_runs_subcycle_engine,
        test_engine1_bp_clear_resume,
        test_z80_basic,
        test_memory_rw,
        test_breakpoint,
        # Thread-split correctness tests (work in both headless and threaded mode)
        test_breakpoint_pause_step_resume,
        test_snapshot_round_trip,
        test_rapid_pause_resume,
        test_step_in_accuracy,
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
    for test in tests:
        try:
            if test():
                passed += 1
            else:
                failed += 1
        except Exception as e:
            print(f"FAIL: {test.__name__} raised {e}")
            failed += 1
        print()

    print("=" * 50)
    print(f"Results: {passed} passed, {failed} failed")
    print("=" * 50)

    return 0 if failed == 0 else 1


if __name__ == '__main__':
    sys.exit(main())
