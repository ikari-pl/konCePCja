#!/usr/bin/env python3
"""Regenerate src/shaders/{blit,crt}_{dxbc,spirv}_blobs.h from compiled blobs.

Reads .dxbc files (Windows D3D12) and .spv files (Vulkan) from
src/shaders/blobs/ and rewrites the matching C++ headers so the byte
arrays embed the latest bytecode.  Writes a placeholder zero-byte
array if a blob is missing — the runtime treats empty arrays as
"this format not available on this build".

Run after scripts/compile_shaders.sh.  Idempotent; safe to run with
no blob files present (produces empty arrays).
"""
from __future__ import annotations

import argparse
import pathlib
import sys
from dataclasses import dataclass

ROOT = pathlib.Path(__file__).resolve().parents[1]
BLOBS = ROOT / "src" / "shaders" / "blobs"
HEADER_DIR = ROOT / "src" / "shaders"


@dataclass(frozen=True)
class Format:
    """Per-shader-format metadata used by emit_array / render_header."""
    label: str          # "DXBC" / "SPIRV" — used in comments + fallback notes
    backend: str        # human-readable backend name in the header banner
    workflow_note: str  # one-line CI workflow description for the file header


DXBC = Format(
    label="DXBC",
    backend="D3D12",
    workflow_note=(
        "The .github/workflows/shader-blobs.yml Windows CI job runs fxc.exe\n"
        "automatically when src/shaders/*.hlsl is touched and commits the\n"
        "result back to the PR branch."
    ),
)
SPIRV = Format(
    label="SPIRV",
    backend="Vulkan",
    workflow_note=(
        "The .github/workflows/shader-blobs.yml Linux CI job runs\n"
        "glslangValidator automatically when src/shaders/*.glsl is touched\n"
        "and commits the result back to the PR branch."
    ),
)

# Each table maps blob_filename → c_array_name.  The blob filename's
# extension (.dxbc / .spv) implies which Format applies.
BLIT_DXBC_BLOBS = [
    ("blit.vert.dxbc", "kBlitVertexDXBC"),
    ("blit.frag.dxbc", "kBlitFragmentDXBC"),
]

CRT_DXBC_BLOBS = [
    ("crt_basic.frag.dxbc",  "kCrtBasicFragmentDXBC"),
    ("crt_full.frag.dxbc",   "kCrtFullFragmentDXBC"),
    ("crt_lottes.frag.dxbc", "kCrtLottesFragmentDXBC"),
]

BLIT_SPIRV_BLOBS = [
    ("blit.vert.spv", "kBlitVertexSPIRV"),
    ("blit.frag.spv", "kBlitFragmentSPIRV"),
]

CRT_SPIRV_BLOBS = [
    ("crt_basic.frag.spv",  "kCrtBasicFragmentSPIRV"),
    ("crt_full.frag.spv",   "kCrtFullFragmentSPIRV"),
    ("crt_lottes.frag.spv", "kCrtLottesFragmentSPIRV"),
]


def emit_array(name: str, data: bytes, fmt: Format) -> str:
    """Format a byte sequence as `alignas(4) inline constexpr ... = { ... };`.

    Naming convention: `kFooDXBC[]` / `kFooSPIRV[]` for the array,
    `kFooDXBCSize` / `kFooSPIRVSize` for the byte count.  For missing
    blobs, emit a 1-byte sentinel array with `Size = 0` (consumers MUST
    check Size before reading; sizeof(array) is 1 in that case because
    zero-length arrays aren't standard C++).
    """
    size_name = f"{name}Size"
    if not data:
        return (
            f"// {name}: empty placeholder — {fmt.label} blob not yet generated.\n"
            f"// {fmt.workflow_note.replace(chr(10), chr(10) + '// ')}\n"
            f"// Consumers MUST check {size_name} before reading the array.\n"
            f"alignas(4) inline constexpr std::uint8_t {name}[1] = {{ 0x00 }};\n"
            f"inline constexpr std::size_t {size_name} = 0;\n"
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
        f"inline constexpr std::size_t {size_name} = sizeof({name});\n"
    )


def render_header(guard: str, blobs: list[tuple[str, str]], fmt: Format) -> str:
    sections = []
    for blob_name, array_name in blobs:
        blob_path = BLOBS / blob_name
        data = blob_path.read_bytes() if blob_path.exists() else b""
        sections.append(f"// === {blob_name} ===\n{emit_array(array_name, data, fmt)}")
    body = "\n".join(sections)
    return (
        f"// konCePCja — {fmt.label} bytecode blobs ({fmt.backend} backend).\n"
        "//\n"
        "// GENERATED FILE — do not hand-edit.  Regenerate via:\n"
        "//   scripts/compile_shaders.sh && scripts/blob_to_header.py\n"
        "//\n"
        f"// {fmt.workflow_note.replace(chr(10), chr(10) + '// ')}\n"
        "//\n"
        "// alignas(4): SDL_GPU may treat the blob as 32-bit-aligned\n"
        "// bytecode.  Vulkan SPIR-V is a 32-bit-word stream; D3D12 DXBC\n"
        "// objects benefit from the same alignment on strict-alignment\n"
        "// architectures (ARM Linux / ARM Windows).\n"
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


def write_header(
    path: pathlib.Path, guard: str, blobs: list[tuple[str, str]], fmt: Format
) -> None:
    text = render_header(guard, blobs, fmt)
    path.write_text(text)
    print(f"  wrote {path.relative_to(ROOT)} ({text.count(chr(10))} lines)")


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--check",
        action="store_true",
        help="exit non-zero if the regenerated headers differ from the on-disk copies",
    )
    parser.add_argument(
        "--format",
        choices=("all", "dxbc", "spirv"),
        default="all",
        help=(
            "which shader format(s) to regenerate.  Default is 'all' — handy for "
            "developers running scripts/compile_shaders.sh locally with both "
            "toolchains installed.  CI jobs pass 'dxbc' (Windows) or 'spirv' "
            "(Linux) so a single-format runner doesn't stomp the other format's "
            "headers (which it can't compile and would otherwise overwrite with "
            "empty placeholders).  --check honours the same filter."
        ),
    )
    args = parser.parse_args(argv)

    # (output_path, header_guard, blob_table, format)
    all_targets = [
        (HEADER_DIR / "blit_dxbc_blobs.h",  "KON_CPC_JA_BLIT_DXBC_BLOBS_H",  BLIT_DXBC_BLOBS,  DXBC),
        (HEADER_DIR / "crt_dxbc_blobs.h",   "KON_CPC_JA_CRT_DXBC_BLOBS_H",   CRT_DXBC_BLOBS,   DXBC),
        (HEADER_DIR / "blit_spirv_blobs.h", "KON_CPC_JA_BLIT_SPIRV_BLOBS_H", BLIT_SPIRV_BLOBS, SPIRV),
        (HEADER_DIR / "crt_spirv_blobs.h",  "KON_CPC_JA_CRT_SPIRV_BLOBS_H",  CRT_SPIRV_BLOBS,  SPIRV),
    ]
    if args.format == "dxbc":
        targets = [t for t in all_targets if t[3] is DXBC]
    elif args.format == "spirv":
        targets = [t for t in all_targets if t[3] is SPIRV]
    else:
        targets = all_targets

    if args.check:
        any_diff = False
        for path, guard, blobs, fmt in targets:
            new = render_header(guard, blobs, fmt)
            cur = path.read_text() if path.exists() else ""
            if new != cur:
                print(f"DIFF: {path.relative_to(ROOT)} — re-run scripts/blob_to_header.py")
                any_diff = True
        return 1 if any_diff else 0

    BLOBS.mkdir(parents=True, exist_ok=True)
    for path, guard, blobs, fmt in targets:
        write_header(path, guard, blobs, fmt)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
