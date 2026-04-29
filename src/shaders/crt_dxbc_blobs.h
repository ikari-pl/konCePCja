// konCePCja — DXBC bytecode blobs (D3D12 backend).
//
// GENERATED FILE — do not hand-edit.  Regenerate via:
//   scripts/compile_shaders.sh && scripts/blob_to_header.py
//
// The .github/workflows/shader-blobs.yml Windows CI job runs both
// automatically when src/shaders/*.hlsl is touched and commits the
// result back to the PR branch.
//
// alignas(4): SDL_GPU's D3D12 backend may treat the blob as
// 32-bit-aligned bytecode.  Match the SPIRV header convention.

#ifndef KON_CPC_JA_CRT_DXBC_BLOBS_H
#define KON_CPC_JA_CRT_DXBC_BLOBS_H

#include <cstddef>
#include <cstdint>

// === crt_basic.frag.dxbc ===
// kCrtBasicFragmentDXBC: empty placeholder — DXBC blob not yet generated.
// The Windows CI workflow (.github/workflows/shader-blobs.yml)
// runs fxc.exe and commits the populated bytes back to this header.
alignas(4) inline constexpr std::uint8_t kCrtBasicFragmentDXBC[] = { 0x00 };
inline constexpr std::size_t kCrtBasicFragmentDXBC_size = 0;  // empty marker

// === crt_full.frag.dxbc ===
// kCrtFullFragmentDXBC: empty placeholder — DXBC blob not yet generated.
// The Windows CI workflow (.github/workflows/shader-blobs.yml)
// runs fxc.exe and commits the populated bytes back to this header.
alignas(4) inline constexpr std::uint8_t kCrtFullFragmentDXBC[] = { 0x00 };
inline constexpr std::size_t kCrtFullFragmentDXBC_size = 0;  // empty marker

// === crt_lottes.frag.dxbc ===
// kCrtLottesFragmentDXBC: empty placeholder — DXBC blob not yet generated.
// The Windows CI workflow (.github/workflows/shader-blobs.yml)
// runs fxc.exe and commits the populated bytes back to this header.
alignas(4) inline constexpr std::uint8_t kCrtLottesFragmentDXBC[] = { 0x00 };
inline constexpr std::size_t kCrtLottesFragmentDXBC_size = 0;  // empty marker
#endif  // KON_CPC_JA_CRT_DXBC_BLOBS_H
