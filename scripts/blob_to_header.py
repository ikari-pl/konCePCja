#!/usr/bin/env python3
"""Regenerate src/shaders/{blit,crt}_dxbc_blobs.h from compiled DXBC blobs.

Reads .dxbc files from src/shaders/blobs/ and rewrites the matching C++
header so the byte arrays embed the latest bytecode.  Writes a
placeholder zero-byte array if a blob is missing — the runtime
treats empty arrays as "DXBC not available on this build".

Run after scripts/compile_shaders.sh.  Idempotent; safe to run with
no .dxbc files present (produces empty arrays).
"""
from __future__ import annotations

import argparse
import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
BLOBS = ROOT / "src" / "shaders" / "blobs"
HEADER_DIR = ROOT / "src" / "shaders"

# (blob_filename, c_array_name)
BLIT_BLOBS = [
    ("blit.vert.dxbc", "kBlitVertexDXBC"),
    ("blit.frag.dxbc", "kBlitFragmentDXBC"),
]

CRT_BLOBS = [
    ("crt_basic.frag.dxbc",  "kCrtBasicFragmentDXBC"),
    ("crt_full.frag.dxbc",   "kCrtFullFragmentDXBC"),
    ("crt_lottes.frag.dxbc", "kCrtLottesFragmentDXBC"),
]


def emit_array(name: str, data: bytes) -> str:
    """Format a byte sequence as `alignas(4) inline constexpr ... = { ... };`."""
    if not data:
        return (
            f"// {name}: empty placeholder — DXBC blob not yet generated.\n"
            f"// The Windows CI workflow (.github/workflows/shader-blobs.yml)\n"
            f"// runs fxc.exe and commits the populated bytes back to this header.\n"
            f"alignas(4) inline constexpr std::uint8_t {name}[] = {{ 0x00 }};\n"
            f"inline constexpr std::size_t {name}_size = 0;  // empty marker\n"
        )
    rows = []
    for i in range(0, len(data), 12):
        chunk = data[i : i + 12]
        rows.append(", ".join(f"0x{b:02x}" for b in chunk))
    body = ",\n    ".join(rows)
    return (
        f"alignas(4) inline constexpr std::uint8_t {name}[] = {{\n"
        f"    {body}\n"
        f"}};\n"
        f"inline constexpr std::size_t {name}_size = sizeof({name});\n"
    )


def write_header(path: pathlib.Path, guard: str, blobs: list[tuple[str, str]]) -> None:
    sections = []
    for blob_name, array_name in blobs:
        blob_path = BLOBS / blob_name
        data = blob_path.read_bytes() if blob_path.exists() else b""
        sections.append(f"// === {blob_name} ===\n{emit_array(array_name, data)}")
    body = "\n".join(sections)
    header = (
        "// konCePCja — DXBC bytecode blobs (D3D12 backend).\n"
        "//\n"
        "// GENERATED FILE — do not hand-edit.  Regenerate via:\n"
        "//   scripts/compile_shaders.sh && scripts/blob_to_header.py\n"
        "//\n"
        "// The .github/workflows/shader-blobs.yml Windows CI job runs both\n"
        "// automatically when src/shaders/*.hlsl is touched and commits the\n"
        "// result back to the PR branch.\n"
        "//\n"
        "// alignas(4): SDL_GPU's D3D12 backend may treat the blob as\n"
        "// 32-bit-aligned bytecode.  Match the SPIRV header convention.\n"
        "\n"
        f"#ifndef {guard}\n"
        f"#define {guard}\n"
        "\n"
        "#include <cstddef>\n"
        "#include <cstdint>\n"
        "\n"
        f"{body}"
        f"#endif  // {guard}\n"
    )
    path.write_text(header)
    print(f"  wrote {path.relative_to(ROOT)} ({sum(1 for _ in body.splitlines())} body lines)")


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--check",
        action="store_true",
        help="exit non-zero if the regenerated headers differ from the on-disk copies",
    )
    args = parser.parse_args(argv)

    targets = [
        (HEADER_DIR / "blit_dxbc_blobs.h", "KON_CPC_JA_BLIT_DXBC_BLOBS_H", BLIT_BLOBS),
        (HEADER_DIR / "crt_dxbc_blobs.h",  "KON_CPC_JA_CRT_DXBC_BLOBS_H",  CRT_BLOBS),
    ]

    if args.check:
        # Generate to memory, compare, complain.
        any_diff = False
        for path, guard, blobs in targets:
            tmp = path.with_suffix(".regen")
            write_header(tmp, guard, blobs)
            new = tmp.read_text()
            tmp.unlink()
            cur = path.read_text() if path.exists() else ""
            if new != cur:
                print(f"DIFF: {path.relative_to(ROOT)} — re-run scripts/blob_to_header.py")
                any_diff = True
        return 1 if any_diff else 0

    BLOBS.mkdir(parents=True, exist_ok=True)
    for path, guard, blobs in targets:
        write_header(path, guard, blobs)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
