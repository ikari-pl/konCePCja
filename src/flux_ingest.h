/* flux_ingest.h — unified content-sniffing flux dispatcher.
 *
 * One entry point that identifies ANY supported flux container by its MAGIC
 * bytes (with the file extension breaking only the magic-less KryoFlux STREAM
 * tie) and transcodes it to in-memory SuperCard Pro (SCP) bytes ready for
 * subcycle::Machine::insert_flux(). This is the single dispatch seam the
 * clean-room decoders (src/ipf_decode, src/hfe, src/kryoflux_stream,
 * src/hw/a2r) hang off — it does NOT reimplement any of them, only routes.
 *
 *   Container    magic (offset 0)              module
 *   ---------    ----------------              ------
 *   Ipf          "CAPS"                        ipf::Image (clean-room decoder)
 *   Scp          "SCP"                         verbatim pass-through
 *   Hfe          "HXCPICFE" / "HXCHFEV3"       hfe_to_scp
 *   A2R          "A2R2" / "A2R3"               a2r_to_scp
 *   KryoFlux     (none — ".raw" ext only)      kryoflux_stream_to_scp
 *
 * The ".raw" extension is ambiguous (IPF CTRAW dumps and KryoFlux STREAM both
 * use it); magic wins, so a ".raw" that begins with "CAPS" decodes as IPF and
 * only a magic-less ".raw" is treated as a KryoFlux stream.
 *
 * Header-light on purpose: this pulls in nothing but the standard library so
 * both wiring sites (subcycle_bridge.cpp, slotshandler.cpp) can include it
 * cheaply. The decoder/module headers live only in flux_ingest.cpp.
 */
#ifndef KONCPC_FLUX_INGEST_H
#define KONCPC_FLUX_INGEST_H

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace flux {

enum class Container : std::uint8_t { Unknown, Scp, Hfe, KryoFluxStream, A2R, Ipf };

// Identify the container by MAGIC BYTES first; `ext_hint` (a lowercased
// extension such as ".raw") only breaks the magic-less KryoFlux STREAM tie.
// `ext_hint` may be empty. Returns Container::Unknown for empty/garbage input.
Container sniff(const uint8_t* data, size_t len, std::string_view ext_hint);

// Transcode ANY supported flux container to in-memory SCP bytes ready for
// insert_flux(). Returns {} on unsupported/failure, logging exactly one reason.
//
// IPF note: the clean-room decoder (ipf::Image) handles SPS-encoder IPFs. A
// CAPS-encoder (encoderType 1) IPF is unsupported and returns {} with a clear
// log — there is no fallback (re-image such a disc as SPS/SCP/HFE).
std::vector<uint8_t> to_scp(const uint8_t* data, size_t len,
                            std::string_view ext_hint);

}  // namespace flux

#endif  // KONCPC_FLUX_INGEST_H
