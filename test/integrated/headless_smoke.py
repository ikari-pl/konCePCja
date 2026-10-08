#!/usr/bin/env python3
"""Smoke test for the -H/--headless loop: start, drive over IPC, quit.

This is the cheapest end-to-end proof that `koncepcja -H` is a usable
backend for an out-of-process frontend (beads-cv2): it boots with no window
and no audio, says where it is listening through the startup manifest on
stdout, answers IPC over one persistent connection, pauses and resumes for
real, and exits cleanly when told to.

Everything is discovered the way a frontend would discover it: the IPC port
comes from the manifest, never from a hardcoded 6543 (the server probes
forward past a busy port, so a guess can silently address another instance).

Usage: headless_smoke.py [path/to/koncepcja]   (default: ./koncepcja)
Exit status 0 on success, 1 on the first failed check.
"""

import os
import re
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path
from typing import Dict, List, Optional

MANIFEST_TIMEOUT_S = 30.0
COMMAND_TIMEOUT_S = 10.0
EXIT_TIMEOUT_S = 10.0


def fail(msg: str) -> None:
    print(f"FAIL: {msg}")
    sys.exit(1)


def read_manifest(proc: subprocess.Popen,
                  stdout_lines: List[str]) -> Dict[str, str]:
    """Collect the YAML manifest block ('--- # koncepcja' .. '...').

    Returns a flat dict keyed by dotted path ('ports.ipc', 'mode'). The
    manifest is deliberately simple (scalars, one level of nesting), so a
    line parser is enough and keeps this script free of a YAML dependency.
    """
    deadline = time.monotonic() + MANIFEST_TIMEOUT_S
    in_block = False
    section = ''
    fields: Dict[str, str] = {}
    seen = 0
    while time.monotonic() < deadline:
        if seen >= len(stdout_lines):
            if proc.poll() is not None:
                fail(f"emulator exited (code {proc.returncode}) before "
                     "finishing the startup manifest")
            time.sleep(0.05)
            continue
        line = stdout_lines[seen].rstrip('\n')
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
        if not indent:
            section = key if value == '' else ''
            if value != '':
                fields[key] = value.strip("'")
        elif section:
            fields[f'{section}.{key}'] = value.strip("'")
    fail(f"no complete startup manifest on stdout within "
         f"{MANIFEST_TIMEOUT_S:.0f}s")
    return {}  # unreachable


class Connection:
    """One persistent IPC connection, line-oriented, like a real frontend."""

    def __init__(self, port: int):
        self.sock = socket.create_connection(('127.0.0.1', port),
                                             timeout=COMMAND_TIMEOUT_S)
        self.buf = b''

    def command(self, cmd: str) -> str:
        self.sock.sendall((cmd + '\n').encode())
        while b'\n' not in self.buf:
            chunk = self.sock.recv(65536)
            if not chunk:
                fail(f"connection closed while waiting for '{cmd}'")
            self.buf += chunk
        line, self.buf = self.buf.split(b'\n', 1)
        return line.decode().strip()

    def expect_ok(self, cmd: str) -> str:
        resp = self.command(cmd)
        if not resp.startswith('OK'):
            fail(f"'{cmd}' -> {resp!r}")
        print(f"  {cmd} -> {resp[:72]}")
        return resp

    def pc(self) -> int:
        resp = self.expect_ok('reg get PC')
        return int(resp.split()[1], 16)


def main() -> None:
    root = Path(__file__).resolve().parent.parent.parent
    exe = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else root / 'koncepcja'
    if not exe.exists():
        fail(f"{exe} does not exist; build it first")

    # A throwaway copy of the shipped example config: never whoever's
    # koncepcja.cfg sits in the working directory, and a save on exit edits
    # the copy, not the example.
    tmpdir = tempfile.mkdtemp(prefix='koncpc-hsmoke-')
    cfg = Path(tmpdir) / 'koncepcja.cfg'
    shutil.copyfile(root / 'koncepcja.cfg.example', cfg)

    env = os.environ.copy()
    env['KONCPC_NO_DIALOGS'] = '1'
    cmd = [str(exe), '-H', '-c', str(cfg)]
    print(f"Starting: {' '.join(cmd)}")
    proc = subprocess.Popen(cmd, cwd=root, env=env, stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE, text=True)
    stdout_lines: List[str] = []
    stderr_lines: List[str] = []

    def pump(stream, sink: List[str]) -> None:
        for line in stream:
            sink.append(line)

    for stream, sink in ((proc.stdout, stdout_lines),
                         (proc.stderr, stderr_lines)):
        threading.Thread(target=pump, args=(stream, sink), daemon=True).start()

    exited_cleanly = False
    try:
        manifest = read_manifest(proc, stdout_lines)
        mode = manifest.get('mode')
        if mode != 'headless':
            fail(f"manifest mode is {mode!r}, expected 'headless'")
        port_text = manifest.get('ports.ipc', 'null')
        if not port_text.isdigit():
            fail(f"manifest has no IPC port (ports.ipc: {port_text})")
        port = int(port_text)
        print(f"  manifest: mode={mode} ipc={port} "
              f"telnet={manifest.get('ports.telnet')} "
              f"tier={manifest.get('machine.effective_tier')}")

        conn = Connection(port)
        if conn.expect_ok('ping') != 'OK pong':
            fail("ping did not answer 'OK pong'")
        if 'gui=0' not in conn.expect_ok('gui'):
            fail("'gui' does not report the headless loop (gui=0)")

        # The core is up once PC leaves 0 (the IPC server binds before the
        # board finishes initialising).
        deadline = time.monotonic() + COMMAND_TIMEOUT_S
        while conn.pc() == 0:
            if time.monotonic() > deadline:
                fail("PC never left 0: the headless core is not running")
            time.sleep(0.05)

        regs = conn.expect_ok('regs')
        for reg in ('A=', 'PC=', 'SP='):
            if reg not in regs:
                fail(f"'regs' reply lacks {reg}: {regs!r}")

        # 'pause' is asynchronous by contract (docs/ipc-protocol.md): it stops
        # the machine at the end of the frame in flight, and register reads
        # are frame-boundary snapshots, so the PC read right after it can
        # still be the pre-frame one. Let the stop land, then hold it to it.
        conn.expect_ok('pause')
        time.sleep(0.5)
        pc1 = conn.pc()
        time.sleep(0.3)
        pc2 = conn.pc()
        if pc1 != pc2:
            fail(f"PC moved while paused ({pc1:04X} -> {pc2:04X})")

        # Running means running: 'wait vbl' counts emulated frames, so it
        # cannot pass on a frozen core, and it returns only once the machine
        # is paused at a frame boundary -- synchronously, unlike 'pause'.
        conn.expect_ok('run')
        conn.expect_ok('wait vbl 25 10000')
        pc1 = conn.pc()
        time.sleep(0.3)
        pc2 = conn.pc()
        if pc1 != pc2:
            fail(f"PC moved after 'wait vbl' paused ({pc1:04X} -> {pc2:04X})")

        # And 'run' really resumes the paused loop.
        conn.expect_ok('run')
        deadline = time.monotonic() + COMMAND_TIMEOUT_S
        while conn.pc() == pc2:
            if time.monotonic() > deadline:
                fail(f"PC stuck at {pc2:04X} after 'run'")
            time.sleep(0.05)

        conn.sock.sendall(b'quit\n')
        try:
            code = proc.wait(timeout=EXIT_TIMEOUT_S)
        except subprocess.TimeoutExpired:
            fail(f"'quit' did not end the process within {EXIT_TIMEOUT_S:.0f}s")
        if code != 0:
            fail(f"'quit' exited with code {code}, expected 0")
        exited_cleanly = True
        print("PASS: -H boots, publishes its ports, serves IPC, pauses, "
              "runs and quits cleanly")
    finally:
        if not exited_cleanly:
            if proc.poll() is None:
                proc.kill()
                proc.wait()
            tail = ''.join(stderr_lines[-30:])
            if tail:
                print("--- emulator stderr (tail) ---")
                print(tail.rstrip())
        shutil.rmtree(tmpdir, ignore_errors=True)


if __name__ == '__main__':
    main()
