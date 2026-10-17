#!/usr/bin/env python3
"""The exit criterion of phase 3 (beads-3o1), as one test.

docs/protocol/kcpc-binary-v1.md, section 16, "Exit criterion of phase 3".
One core, and with no polling loop and no sleep-and-retry anywhere:

1. two connections, a controller and an observer;
2. the controller sets a breakpoint and runs, and BOTH connections are
   pushed EVT_STOPPED with reason BREAKPOINT;
3. the observer reads 65536 bytes of the CPU space in ONE response frame,
   byte for byte what the text IPC's `mem read` of the same range returns.

Every wait is a blocking read of a socket or a pipe. Nothing sleeps: a core
that hangs is killed by a watchdog timer, which turns the hang into a failed
read instead of a stuck job.

Usage: test_exit_criterion.py [--ui-free | --gui] [--repeat N]
                              [path/to/koncepcja]
  (default)  the binary is started with -H
  --ui-free  the binary is a KONCPC_MODERN_UI=0 build, headless by
             construction, started without -H
  --gui      the GUI build under SDL's dummy drivers (the threaded loop)
  --repeat N run the test N times, each against a core of its own
The binary defaults to ./koncepcja, or $KONCPC_EXE when set.
Exit status 0 when every run passed.
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
import traceback
from pathlib import Path
from typing import Dict, List, Optional

sys.path.insert(0, str(Path(__file__).resolve().parent))
import kcpc_client as kc  # noqa: E402
from kcpc_client import KcpcClient  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent.parent.parent

# The whole run, start to exit. A core that is still there after it is
# killed, and every read it was serving fails.
WATCHDOG_S = 60.0
IO_TIMEOUT_S = 10.0
INTERRUPT_VECTOR = 0x0038   # RST &38: the firmware's 300 Hz interrupt entry


class Core:
    """One core and the ports its startup manifest printed."""

    def __init__(self, exe: Path, mode: str):
        self.tmpdir = tempfile.mkdtemp(prefix='koncpc-exit-')
        cfg = Path(self.tmpdir) / 'koncepcja.cfg'
        shutil.copyfile(ROOT / 'koncepcja.cfg.example', cfg)
        env = os.environ.copy()
        env['KONCPC_NO_DIALOGS'] = '1'
        env['KONCPC_TEST_WINDOW'] = '1'
        cmd = [str(exe)]
        if mode == 'headless':
            cmd.append('-H')
        elif mode == 'gui':
            env['SDL_VIDEODRIVER'] = 'dummy'
            env['SDL_AUDIODRIVER'] = 'dummy'
        cmd += ['-c', str(cfg)]
        self.proc = subprocess.Popen(cmd, cwd=ROOT, env=env,
                                     stdout=subprocess.PIPE,
                                     stderr=subprocess.PIPE, text=True)
        self.watchdog = threading.Timer(WATCHDOG_S, self.proc.kill)
        self.watchdog.daemon = True
        self.watchdog.start()
        self.stderr_lines: List[str] = []
        threading.Thread(target=self._drain, args=(self.proc.stderr,
                                                   self.stderr_lines),
                         daemon=True).start()
        self.ports = self._read_manifest()
        # The rest of stdout, so the core never blocks on a full pipe.
        threading.Thread(target=self._drain, args=(self.proc.stdout, []),
                         daemon=True).start()

    @staticmethod
    def _drain(stream, sink: List[str]) -> None:
        for line in stream:
            sink.append(line)

    def _read_manifest(self) -> Dict[str, int]:
        """`ports:` of the YAML block between '--- # koncepcja' and '...',
        read line by line in blocking reads of the pipe."""
        ports: Dict[str, int] = {}
        in_block = in_ports = False
        for line in self.proc.stdout:
            line = line.rstrip('\n')
            if not in_block:
                in_block = line.startswith('--- # koncepcja')
                continue
            if line == '...':
                return ports
            m = re.match(r'^(\s*)([A-Za-z0-9_]+):\s*(.*)$', line)
            if not m:
                continue
            indent, key, value = m.groups()
            value = value.split(' #')[0].strip()
            if not indent:
                in_ports = key == 'ports'
            elif in_ports and value.isdigit():
                ports[key] = int(value)
        raise AssertionError(
            f'the core exited ({self.proc.wait()}) before its manifest was '
            f'complete:\n{"".join(self.stderr_lines[-20:])}')

    def client(self) -> KcpcClient:
        assert 'binary' in self.ports, f'no binary port: {self.ports}'
        return KcpcClient(self.ports['binary'], timeout=IO_TIMEOUT_S)

    def ipc(self, command: str) -> str:
        """One text IPC command, its one-line answer read in blocking
        reads."""
        with socket.create_connection(('127.0.0.1', self.ports['ipc']),
                                      timeout=IO_TIMEOUT_S) as sock:
            sock.sendall((command + '\n').encode())
            buf = b''
            while not buf.endswith(b'\n'):
                chunk = sock.recv(1 << 20)
                assert chunk, f'text IPC closed during {command!r}'
                buf += chunk
        return buf.decode().strip()

    def quit(self) -> int:
        """Ask the core to quit and wait for it: the watchdog bounds the
        wait."""
        try:
            self.ipc('quit')
        except (OSError, AssertionError):
            pass   # it may exit before it answers
        code = self.proc.wait()
        self.watchdog.cancel()
        return code

    def stop(self) -> None:
        if self.proc.poll() is None:
            self.proc.kill()
            self.proc.wait()
        self.watchdog.cancel()
        shutil.rmtree(self.tmpdir, ignore_errors=True)


def run_event(c: KcpcClient, who: str) -> kc.Event:
    """The next run-state event on `c`, in a blocking read. A change of
    control is the only other event a RUN subscription is pushed."""
    while True:
        event = c.next_event(IO_TIMEOUT_S)
        if event.opcode in (kc.EVT_STOPPED, kc.EVT_RESUMED, kc.EVT_RESET):
            return event
        assert event.opcode == kc.EVT_CONTROL_CHANGED, f'{who}: {event}'


def expect(c: KcpcClient, who: str, opcode: int,
           reason: Optional[int] = None) -> kc.Event:
    event = run_event(c, who)
    assert event.opcode == opcode, (
        f'{who}: {event.name}, expected {kc.EVENT_NAMES[opcode]}: {event}')
    if reason is not None:
        assert event.reason == reason, f'{who}: reason {event.reason}: {event}'
    return event


def exit_criterion(exe: Path, mode: str) -> str:
    core = Core(exe, mode)
    try:
        # 1. Two connections: a controller and an observer.
        with core.client() as ctl, core.client() as obs:
            hello = ctl.hello('exit-criterion controller',
                              role=kc.ROLE_CONTROLLER)
            assert hello.role == kc.ROLE_CONTROLLER, hello
            needed = (kc.CAP_MACHINE_READ | kc.CAP_MACHINE_WRITE
                      | kc.CAP_RUN_CONTROL | kc.CAP_EVENTS
                      | kc.CAP_BREAKPOINTS)
            assert hello.caps & needed == needed, f'caps {hello.caps:#x}'
            seen = obs.hello('exit-criterion observer')
            assert seen.role == kc.ROLE_OBSERVER, seen
            assert seen.client_id != hello.client_id
            for c in (ctl, obs):
                assert c.subscribe(kc.CLASS_RUN) == (kc.CLASS_RUN, 0)

            # 2. The controller sets a breakpoint and runs. It pauses first,
            # so that the run is a change both connections are told of.
            ctl.pause()
            for who, c in (('controller', ctl), ('observer', obs)):
                expect(c, who, kc.EVT_STOPPED, kc.STOP_PAUSED)
            ctl.bp_add(kc.BpEntry(kc.BP_EXEC, INTERRUPT_VECTOR))
            began = time.monotonic()
            ctl.run()
            stops = {}
            for who, c in (('controller', ctl), ('observer', obs)):
                expect(c, who, kc.EVT_RESUMED)
                stop = expect(c, who, kc.EVT_STOPPED, kc.STOP_BREAKPOINT)
                assert (stop.pc, stop.addr) == (
                    INTERRUPT_VECTOR, INTERRUPT_VECTOR), f'{who}: {stop}'
                assert (stop.cause_client, stop.cause_request) == (0, 0), (
                    f'{who}: a breakpoint stop is nobody\'s request: {stop}')
                stops[who] = stop
            pushed_ms = (time.monotonic() - began) * 1000
            assert stops['controller'].frame == stops['observer'].frame, stops

            # 3. The observer reads the whole CPU space: the next frame on
            # its connection is the one response, and it holds every byte.
            request = obs.send_request(
                kc.OP_MEM_READ,
                kc.mem_read_payload(kc.SPACE_CPU, 0, kc.CPU_SPACE_SIZE))
            frame = obs.read_frame()
            assert frame.kind == kc.KIND_RESPONSE, frame.kind
            assert (frame.opcode, frame.id) == (kc.OP_MEM_READ, request)
            assert frame.status == kc.OK, (
                kc.status_name(frame.status), frame.message)
            assert len(frame.payload) == (kc.MEM_READ_REPLY_OVERHEAD
                                          + kc.CPU_SPACE_SIZE), (
                len(frame.payload))
            fields = kc.Fields(frame.payload)
            fields.skip(4)
            stamp = kc.Stamp.read(fields)
            cpu = fields.rest()
            assert stamp.paused and stamp.frame == stops['observer'].frame, (
                f'the read is not of the stopped machine: {stamp}')

            text = core.ipc('mem read 0 65536')
            assert text.startswith('OK '), text[:80]
            assert cpu == bytes.fromhex(text[3:]), (
                'MEM_READ of the CPU space differs from the text IPC')
            assert any(cpu), 'the CPU space is all zeros: nothing was read'
            ctl.bp_clear()
        code = core.quit()
        assert code == 0, f'the core exited with {code}'
        return (f'pushed to both in {pushed_ms:.0f} ms, frame '
                f'{stops["observer"].frame}; 65536 bytes in one '
                f'{len(frame.payload) + kc.HEADER_SIZE}-byte frame')
    except Exception:
        sys.stderr.write(''.join(core.stderr_lines[-30:]))
        raise
    finally:
        core.stop()


def main() -> int:
    argv = sys.argv[1:]
    mode = 'headless'
    repeat = 1
    if '--repeat' in argv:
        index = argv.index('--repeat')
        repeat = int(argv[index + 1])
        del argv[index:index + 2]
    if '--ui-free' in argv:
        mode = 'ui-free'
    elif '--gui' in argv:
        mode = 'gui'
    argv = [a for a in argv if a not in ('--ui-free', '--gui')]
    if argv:
        exe = Path(argv[0]).resolve()
    elif os.environ.get('KONCPC_EXE'):
        exe = Path(os.environ['KONCPC_EXE']).resolve()
    else:
        exe = ROOT / 'koncepcja'
    if not exe.exists():
        print(f'FAIL: {exe} does not exist; build it first')
        return 1

    print(f'KCPC exit criterion against {exe} ({mode})')
    failed = 0
    for run in range(1, repeat + 1):
        begin = time.monotonic()
        try:
            detail = exit_criterion(exe, mode)
        except Exception:  # pylint: disable=broad-except
            failed += 1
            print(f'  FAIL run {run}')
            traceback.print_exc()
            continue
        print(f'  ok   run {run} ({time.monotonic() - begin:.1f}s): {detail}')
    print(f'Results: {repeat - failed} passed, {failed} failed')
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
