#!/usr/bin/env python3
"""The exit criterion of phase 4 (beads-0ir), as one test.

From a running core, another process reads the machine's frame out of the
shared-memory ring the core publishes (docs/protocol/fb-ring-v1.md), and it
is the frame the core itself hashes:

1. HELLO announces CAP_FB_SHM, and FB_INFO names the ring (transport 1, the
   OS name for the core's pid) with the sequence number of its latest frame;
2. EVT_FRAME carries each frame's sequence number in the ring, one more for
   each frame;
3. on the paused machine, the text IPC's `hash frame`, the reference
   client's ring reader (kcpc_client.FbRing) and tools/fb_dump (a C program
   that knows the ring only from the document) give the same CRC32 of the
   same frame, and FB_INFO's fb_seq is that frame's;
4. the picture fb_dump writes is that frame (each row twice, as the core's
   own `screenshot`), and a recognisable CPC screen: the firmware's blue
   paper with its yellow text.

Like test_exit_criterion.py: every wait is a blocking read, nothing polls, and
a watchdog kills a core that hangs. It connects only to the ports its own
core printed in its manifest.

Usage: test_fb_ring.py [--ui-free | --gui] [--repeat N] [path/to/koncepcja]
  (default)  the binary is started with -H
  --ui-free  the binary is a KONCPC_MODERN_UI=0 build, started without -H
  --gui      the GUI build under SDL's dummy drivers (the threaded loop)
fb_dump is $KONCPC_FB_DUMP, else fb_dump next to the binary, else
./fb_dump. The binary defaults to ./koncepcja, or $KONCPC_EXE when set.
Exit status 0 when every run passed.
"""

import os
import re
import struct
import subprocess
import sys
import tempfile
import time
import traceback
import zlib
from collections import Counter
from pathlib import Path
from typing import Tuple

sys.path.insert(0, str(Path(__file__).resolve().parent))
import kcpc_client as kc  # noqa: E402
from kcpc_client import FbRing  # noqa: E402
from test_exit_criterion import Core, IO_TIMEOUT_S, ROOT  # noqa: E402

FRAMES_WATCHED = 5
# Frames from power-on to the banner and "Ready" (about 120 at 50 Hz), with
# room to spare.
BOOT_FRAMES = 200
WAIT_MS = 30000


def find_fb_dump(exe: Path) -> Path:
    suffix = '.exe' if sys.platform == 'win32' else ''
    if os.environ.get('KONCPC_FB_DUMP'):
        return Path(os.environ['KONCPC_FB_DUMP']).resolve()
    beside = exe.parent / f'fb_dump{suffix}'
    if beside.exists():
        return beside
    return ROOT / f'fb_dump{suffix}'


def ring_name(pid: int) -> str:
    """docs/protocol/fb-ring-v1.md, "Name and lifetime"."""
    if sys.platform == 'win32':
        return f'Local\\kcpc-fb-{pid}'
    return f'/kcpc-fb-{pid}'


def read_png_rgb(path: Path) -> Tuple[int, int, bytes]:
    """(width, height, RGB rows) of an 8-bit RGB PNG, unfiltered or with
    filter 0 on every row (what fb_dump writes)."""
    data = path.read_bytes()
    assert data[:8] == b'\x89PNG\r\n\x1a\n', 'not a PNG'
    pos = 8
    idat = b''
    width = height = 0
    while pos < len(data):
        length, kind = struct.unpack_from('>I4s', data, pos)
        body = data[pos + 8:pos + 8 + length]
        crc, = struct.unpack_from('>I', data, pos + 8 + length)
        assert crc == zlib.crc32(kind + body), f'bad CRC in {kind!r}'
        if kind == b'IHDR':
            width, height, depth, colour = struct.unpack_from('>IIBB', body)
            assert (depth, colour) == (8, 2), (depth, colour)
        elif kind == b'IDAT':
            idat += body
        pos += 12 + length
    raw = zlib.decompress(idat)
    line = 1 + width * 3
    assert len(raw) == line * height, (len(raw), line, height)
    rows = []
    for y in range(height):
        assert raw[y * line] == 0, f'row {y}: filter {raw[y * line]}'
        rows.append(raw[y * line + 1:(y + 1) * line])
    return width, height, b''.join(rows)


def recognisable(rgb: bytes) -> str:
    """The boot screen's colours: blue paper covering most of the frame,
    yellow ink on it. Anything else (black, one colour, noise) fails."""
    pixels = Counter(rgb[i:i + 3] for i in range(0, len(rgb), 3))
    (paper, count), = pixels.most_common(1)
    share = count / (len(rgb) // 3)
    r, g, b = paper
    assert b > 0x60 and r < 0x40 and g < 0x40 and share > 0.5, (
        f'the frame is not blue paper: {paper.hex()} on {share:.0%}')
    ink = [c for c in pixels if c[0] > 0xC0 and c[1] > 0xC0 and c[2] < 0x40]
    assert ink, f'no yellow ink: colours {[c.hex() for c in pixels][:8]}'
    return f'paper #{paper.hex()} on {share:.0%}, ink #{ink[0].hex()}'


def fb_ring_exit(exe: Path, mode: str, fb_dump: Path) -> str:
    core = Core(exe, mode)
    tmp = tempfile.mkdtemp(prefix='koncpc-fbdump-')
    try:
        with core.client() as ctl:
            # 1. The ring is announced and named.
            hello = ctl.hello('fb-ring exit', role=kc.ROLE_CONTROLLER)
            assert hello.role == kc.ROLE_CONTROLLER, hello
            needed = kc.CAP_FB_INFO | kc.CAP_FB_SHM | kc.CAP_RUN_CONTROL
            assert hello.caps & needed == needed, f'caps {hello.caps:#x}'
            info = ctl.fb_info()
            assert info.transport == kc.FB_TRANSPORT_SHM, info
            assert info.shm_name == ring_name(hello.pid), (
                f'{info.shm_name!r} is not the name for pid {hello.pid}')
            assert (info.width, info.height, info.stride) == (
                768, 272, 2304), info

            # 2. EVT_FRAME carries the ring's sequence number, frame by frame.
            assert ctl.subscribe(kc.CLASS_FRAME | kc.CLASS_RUN, 1) == (
                kc.CLASS_FRAME | kc.CLASS_RUN, 1)
            frames = []
            while len(frames) < FRAMES_WATCHED:
                event = ctl.next_event(IO_TIMEOUT_S)
                if event.opcode == kc.EVT_FRAME:
                    frames.append(event)
            for before, after in zip(frames, frames[1:]):
                assert before.fb_seq != 0, before
                assert (after.fb_seq - before.fb_seq
                        == after.frame - before.frame), (before, after)

            # 3. On the paused machine: one frame, four readers, one hash.
            # `wait vbl` runs the machine on to a frame where the firmware's
            # banner is up, then pauses it: a blocking request, no polling.
            ctl.subscribe(kc.CLASS_RUN)
            waited = core.ipc(f'wait vbl {BOOT_FRAMES} {int(WAIT_MS)}')
            assert waited.startswith('OK'), waited
            text = core.ipc('hash frame')
            m = re.match(r'OK crc32=([0-9A-F]{8}) frame=(\d+) paused=1 ', text)
            assert m, text
            core_crc, core_frame = int(m.group(1), 16), int(m.group(2))

            with FbRing(info.shm_name) as ring:
                assert ring.producer_pid == hello.pid, ring.producer_pid
                seq, pixels = ring.read_latest()
            assert zlib.crc32(pixels) == core_crc, (
                f'the ring\'s frame {seq} hashes {zlib.crc32(pixels):08X}, '
                f'the core\'s `hash frame` {core_crc:08X}')
            assert ctl.fb_info().fb_seq == seq, (ctl.fb_info(), seq)

            png = Path(tmp) / 'frame.png'
            dump = subprocess.run([str(fb_dump), info.shm_name, str(png)],
                                  capture_output=True, text=True,
                                  timeout=IO_TIMEOUT_S, check=False)
            assert dump.returncode == 0, (dump.returncode, dump.stderr)
            m = re.match(r'seq=(\d+) crc32=([0-9A-F]{8}) width=768 '
                         r'height=272$', dump.stdout.strip())
            assert m, dump.stdout
            assert (int(m.group(1)), int(m.group(2), 16)) == (seq, core_crc), (
                f'fb_dump: {dump.stdout.strip()}; ring seq {seq}, core '
                f'{core_crc:08X}')

            # 4. The picture is the frame, rows doubled, and a CPC screen.
            width, height, rgb = read_png_rgb(png)
            assert (width, height) == (768, 544), (width, height)
            doubled = b''.join(pixels[y * 2304:(y + 1) * 2304] * 2
                               for y in range(272))
            assert rgb == doubled, 'the PNG is not the frame'
            look = recognisable(pixels)
        code = core.quit()
        assert code == 0, f'the core exited with {code}'
        return (f'hash frame {core_crc:08X} after frame {core_frame} == ring '
                f'seq {seq} == fb_dump; {look}')
    except Exception:
        sys.stderr.write(''.join(core.stderr_lines[-30:]))
        raise
    finally:
        core.stop()
        for leftover in Path(tmp).glob('*'):
            leftover.unlink()
        os.rmdir(tmp)


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
    fb_dump = find_fb_dump(exe)
    if not fb_dump.exists():
        print(f'FAIL: {fb_dump} does not exist; build fb_dump first')
        return 1

    print(f'Frame ring exit criterion against {exe} ({mode}), {fb_dump}')
    failed = 0
    for run in range(1, repeat + 1):
        begin = time.monotonic()
        try:
            detail = fb_ring_exit(exe, mode, fb_dump)
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
