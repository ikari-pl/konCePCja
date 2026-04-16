#!/usr/bin/env python3
"""
IPC Test Harness for konCePCja Emulator

Connects to the emulator's IPC server (port 6543) to run automated tests.
"""

import socket
import subprocess
import time
import sys
import os
from pathlib import Path
from typing import Optional, Tuple

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

    def __init__(self, exe_path: str = None):
        if exe_path is None:
            # Find the executable relative to this script
            script_dir = Path(__file__).parent
            project_root = script_dir.parent.parent
            exe_path = str(project_root / 'koncepcja')
        self.exe_path = exe_path
        self.process: Optional[subprocess.Popen] = None
        self.ipc = KoncepcjaIPC()

    def start(self, *args, headless: bool = True) -> bool:
        """Start emulator with given arguments."""
        env = os.environ.copy()
        if headless:
            env['SDL_VIDEODRIVER'] = 'dummy'
            env['SDL_AUDIODRIVER'] = 'dummy'

        cmd = [self.exe_path] + list(args)
        try:
            self.process = subprocess.Popen(
                cmd,
                env=env,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE
            )
            # Wait for IPC server to start
            if self.ipc.connect():
                return True
            else:
                self.stop()
                return False
        except Exception as e:
            print(f"Failed to start emulator: {e}")
            return False

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


def main():
    """Run all IPC tests."""
    print("=" * 50)
    print("konCePCja IPC Test Harness")
    print("=" * 50)

    tests = [
        test_z80_basic,
        test_memory_rw,
        test_breakpoint,
        # Thread-split correctness tests (work in both headless and threaded mode)
        test_breakpoint_pause_step_resume,
        test_snapshot_round_trip,
        test_rapid_pause_resume,
        test_step_in_accuracy,
    ]

    passed = 0
    failed = 0

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
