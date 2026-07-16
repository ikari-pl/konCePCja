/* flux_save.h — Stage 4 "Save-As" core: the live FDC medium of a drive turned
 * into the bytes of a chosen container (.dsk / .scp / .hfe), plus the file
 * writer around them. See docs/hardware/flux-media.md and the Stage-1 encoders
 * (scp_write.h / hfe_write.h) this layer wires up.
 *
 * THE DRIVEA HAZARD (why this exists): under engine=1 the FDC's WRITE DATA /
 * FORMAT land in the sub-cycle medium (its DSK overlay), NOT in the legacy
 * driveA / driveB structs. Saving from those structs silently drops every
 * engine=1 write. So every path here sources bytes from the LIVE FDC medium via
 * the fdc_media_* accessors — never from driveA/driveB.
 *
 * Layering: flux_save_bytes_from_medium is a PURE function over raw medium
 * buffers (the fully unit-tested layer). flux_save_*_dev read those buffers off
 * a live FDC Device. flux_save_bytes / flux_save_to_file / flux_save_caps hang
 * the whole thing off the sub-cycle bridge's drive `unit`. */
#ifndef KONCPC_FLUX_SAVE_H
#define KONCPC_FLUX_SAVE_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

struct Device;  // hw/device.h — the live FDC handle

// Which container the caller wants to write.
enum class SaveFormat : std::uint8_t { Dsk, Scp, Hfe };

// What a drive's live medium can be saved as. A sector-backed disc offers .dsk
// only; a flux-backed drive-A disc additionally offers .scp / .hfe (the flux
// container preserves the weak/protection bits of tracks the CPC never wrote).
struct FluxSaveCaps {
  bool present = false;  // a disc is in the drive at all
  bool can_dsk = false;  // .dsk save available (a sector image / DSK overlay)
  bool can_scp = false;  // .scp export available (flux-backed)
  bool can_hfe = false;  // .hfe export available (flux-backed)
};

// PURE core. Produce the bytes to write for `fmt` from a medium described by
// its raw buffers, any of which may be null:
//   scp / scp_len        pristine flux the disc loaded from (null ⇒ sector-
//                        backed: no weak-bit source, .scp/.hfe unsupported).
//   image / image_len    the writable DSK / sector image (null ⇒ a read-only
//                        flux dump with no overlay: .dsk unsupported).
//   track_dirty / ntracks per-cylinder written map for a writable flux disc
//                        (null ⇒ treat every track as clean → verbatim flux).
// Returns {} and sets `err` on an unsupported/failed combo (e.g. .scp on a
// sector-backed disc). `err` is cleared on success.
std::vector<uint8_t> flux_save_bytes_from_medium(
    const std::uint8_t* scp, std::size_t scp_len, const std::uint8_t* image,
    std::size_t image_len, const bool* track_dirty, int ntracks, SaveFormat fmt,
    std::string& err);

// Read the live medium of drive `unit` off a live FDC Device (unit 0 = A,
// 1 = B) and delegate to flux_save_bytes_from_medium. Flux is drive-A-only, so
// unit 1 always resolves to the sector path.
std::vector<uint8_t> flux_save_bytes_dev(const Device* fdc, std::uint8_t unit,
                                         SaveFormat fmt, std::string& err);

// The save capabilities of drive `unit`'s live medium on `fdc`.
FluxSaveCaps flux_save_caps_dev(const Device* fdc, std::uint8_t unit);

// Write `bytes` to `path` with checked stdio (disk-full / I/O errors are
// reported, never silent). Returns true on success; sets `err` otherwise.
bool flux_write_file(const std::vector<uint8_t>& bytes, const std::string& path,
                     std::string& err);

// --- App-facing: the LIVE drive `unit` of the sub-cycle bridge machine. ---
// These resolve the bridge's FDC Device, so they no-op (return {} / false /
// an empty caps) when the engine is inactive.

// The bytes to write for drive `unit` in `fmt`, or {} + `err` on failure.
std::vector<uint8_t> flux_save_bytes(int unit, SaveFormat fmt,
                                     std::string& err);

// Save drive `unit` to `path` as `fmt`. Returns true on success.
bool flux_save_to_file(int unit, SaveFormat fmt, const std::string& path,
                       std::string& err);

// The save capabilities of drive `unit`'s live medium (for menu/filter gating).
FluxSaveCaps flux_save_caps(int unit);

#endif  // KONCPC_FLUX_SAVE_H
