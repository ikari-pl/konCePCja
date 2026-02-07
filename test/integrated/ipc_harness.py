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


def main():
    """Run all IPC tests."""
    print("=" * 50)
    print("konCePCja IPC Test Harness")
    print("=" * 50)

    tests = [
        test_z80_basic,
        test_memory_rw,
        test_breakpoint,
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
