#!/usr/bin/env python3
"""Phase 8 perf verification — compare GL vs GPU plugin Phase A timing.

Launches the emulator twice (once per plugin under test), lets it settle,
samples metrics via IPC every second for `WARMUP + SAMPLES` seconds,
then prints a side-by-side report of display_time_avg_us (the Phase A
work the Z80 thread blocks on in FrameSignal::wait_consumed).

Usage:
  python3 test/integrated/perf_benchmark/benchmark.py
  python3 test/integrated/perf_benchmark/benchmark.py --plugins 0,21,26,28,29,30
"""
import argparse
import os
import socket
import subprocess
import sys
import time
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[3]
KONCPC = REPO_ROOT / "koncepcja"

WARMUP_SECONDS = 3
SAMPLE_SECONDS = 10


def send_ipc(cmd: str, port: int = 6543, timeout: float = 5.0) -> str:
    # Line-buffered read via makefile(); a single recv() is not
    # guaranteed to return the full response on a TCP stream.
    with socket.create_connection(("localhost", port), timeout=timeout) as sock:
        sock.sendall((cmd + "\n").encode())
        with sock.makefile("r", encoding="utf-8") as f:
            return f.readline().strip()


def wait_for_ipc(port: int, timeout: float = 15.0) -> bool:
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            resp = send_ipc("ping", port=port, timeout=0.5)
            if resp.startswith("OK"):
                return True
        except Exception:
            pass
        time.sleep(0.2)
    return False


def parse_metrics(line: str) -> dict:
    # "OK frame_time_avg_us=... display_time_avg_us=... ..."
    if not line.startswith("OK "):
        raise ValueError(f"not OK: {line!r}")
    out = {}
    for kv in line[3:].split():
        if "=" in kv:
            k, v = kv.split("=", 1)
            try:
                out[k] = float(v)
            except ValueError:
                out[k] = v
    return out


def bench_plugin(scr_style: int) -> dict:
    """Run emulator with given scr_style, sample metrics, return averages."""
    cfg = REPO_ROOT / "koncepcja.cfg"
    # Override scr_style via command line
    env = os.environ.copy()
    env["SDL_VIDEODRIVER"] = env.get("SDL_VIDEODRIVER", "dummy")
    env["SDL_AUDIODRIVER"] = env.get("SDL_AUDIODRIVER", "dummy")

    # Start emulator
    proc = subprocess.Popen(
        [str(KONCPC), "-O", f"video.scr_style={scr_style}"],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        env=env,
    )
    try:
        if not wait_for_ipc(6543, timeout=20):
            raise RuntimeError("IPC never came up")

        # Warmup (Metal first-frame cost, pipeline compile, etc.)
        time.sleep(WARMUP_SECONDS)

        samples = []
        for _ in range(SAMPLE_SECONDS):
            time.sleep(1.0)
            try:
                m = parse_metrics(send_ipc("metrics"))
                samples.append(m)
            except Exception as e:
                print(f"    [warn] sample failed: {e}", file=sys.stderr)

        if not samples:
            return {"error": "no samples"}

        # Average the per-second samples
        keys = samples[0].keys()
        avg = {
            k: sum(s.get(k, 0) for s in samples) / len(samples)
            for k in keys
            if isinstance(samples[0][k], float)
        }
        avg["_samples"] = len(samples)
        return avg
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()


def format_report(rows: list[tuple[int, str, dict]]) -> str:
    lines = []
    lines.append(
        f"{'plugin':>3} {'name':<30}  {'display_us':>12}  {'frame_us':>10}  "
        f"{'z80_us':>10}  {'sleep_us':>10}  {'aq_min_ms':>10}"
    )
    lines.append("-" * 95)
    for idx, name, m in rows:
        if "error" in m:
            lines.append(f"{idx:>3} {name:<30}  ERROR: {m['error']}")
            continue
        lines.append(
            f"{idx:>3} {name:<30}  "
            f"{m.get('display_time_avg_us', 0):>12.1f}  "
            f"{m.get('frame_time_avg_us', 0):>10.1f}  "
            f"{m.get('z80_time_avg_us', 0):>10.1f}  "
            f"{m.get('sleep_time_avg_us', 0):>10.1f}  "
            f"{m.get('audio_queue_min_ms', 0):>10.1f}"
        )
    return "\n".join(lines)


# Plugin index -> short name for the report.  Matches video_plugin_list
# order after Phase 7a (CRT Lottes GPU at 30).
PLUGIN_NAMES = {
    0: "Direct (GL)",
    1: "Direct double (GL)",
    11: "CRT Basic (GL)",
    12: "CRT Full (GL)",
    13: "CRT Lottes (GL)",
    14: "Direct (SDL)",
    20: "Direct (GPU)",
    21: "Super eagle (GPU)",
    22: "Scale2x (GPU)",
    23: "Advanced Scale2x (GPU)",
    24: "TV 2x (GPU)",
    25: "Software bilinear (GPU)",
    26: "Software bicubic (GPU)",
    27: "Dot matrix (GPU)",
    28: "CRT Basic (GPU)",
    29: "CRT Full (GPU)",
    30: "CRT Lottes (GPU)",
}


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--plugins", default="0,11,12,13,20,28,29,30",
                    help="Comma-separated scr_style indices to benchmark")
    args = ap.parse_args()

    idxs = [int(x) for x in args.plugins.split(",") if x.strip()]
    rows = []
    for idx in idxs:
        name = PLUGIN_NAMES.get(idx, f"scr_style={idx}")
        print(f"==> Benchmarking {idx}: {name}", file=sys.stderr)
        m = bench_plugin(idx)
        rows.append((idx, name, m))
        if "error" not in m:
            print(
                f"    display={m.get('display_time_avg_us', 0):.1f}us "
                f"frame={m.get('frame_time_avg_us', 0):.1f}us "
                f"z80={m.get('z80_time_avg_us', 0):.1f}us "
                f"({m.get('_samples', 0):.0f} samples)",
                file=sys.stderr,
            )

    print()
    print(format_report(rows))
    return 0


if __name__ == "__main__":
    sys.exit(main())
