/* flux_ingest.cpp — see flux_ingest.h. Magic-first container sniff + a single
 * transcode dispatch onto the clean-room decoders. */
#include "flux_ingest.h"

#include <cstring>

#include "hfe.h"             // hfe_to_scp
#include "ipf.h"             // scp_from_mfm_tracks
#include "ipf_decode.h"      // ipf::Image / ipf::Status / ipf::status_str
#include "kryoflux_stream.h" // kryoflux_stream_to_scp
#include "log.h"

extern "C" {
#include "hw/flux.h"  // flux_scp_probe
}

#include "hw/a2r.h"  // a2r_to_scp

namespace flux {

namespace {

// data begins with `magic` (n bytes)?
bool starts_with(const uint8_t* data, size_t len, const char* magic, size_t n) {
  return data != nullptr && len >= n && std::memcmp(data, magic, n) == 0;
}

}  // namespace

Container sniff(const uint8_t* data, size_t len, std::string_view ext_hint) {
  if (data == nullptr || len == 0) return Container::Unknown;

  // Magic wins — this resolves the ".raw" IPF-CTRAW-vs-KryoFlux collision.
  if (starts_with(data, len, "CAPS", 4)) return Container::Ipf;
  if (starts_with(data, len, "SCP", 3)) return Container::Scp;
  if (starts_with(data, len, "HXCPICFE", 8) ||
      starts_with(data, len, "HXCHFEV3", 8))
    return Container::Hfe;
  if (starts_with(data, len, "A2R2", 4) || starts_with(data, len, "A2R3", 4))
    return Container::A2R;

  // Magic-less: the KryoFlux STREAM carries no signature and is identified by
  // its ".raw" extension alone (and only when no magic above matched).
  if (ext_hint == ".raw") return Container::KryoFluxStream;

  return Container::Unknown;
}

std::vector<uint8_t> to_scp(const uint8_t* data, size_t len,
                            std::string_view ext_hint) {
  std::vector<uint8_t> out;
  switch (sniff(data, len, ext_hint)) {
    case Container::Scp:
      // Already SCP — validate geometry, then pass the bytes through verbatim.
      if (flux_scp_probe(data, len) == 1) {
        out.assign(data, data + len);
        return out;
      }
      LOG_ERROR("flux_ingest: SCP input failed geometry validation");
      return {};

    case Container::Hfe: {
      const int hfe_rc = hfe_to_scp(data, len, out);
      if (hfe_rc == 0) return out;
      LOG_ERROR("flux_ingest: HFE transcode failed (rc=" << hfe_rc << ")");
      return {};
    }

    case Container::A2R: {
      const int a2r_rc = a2r_to_scp(data, len, out);
      if (a2r_rc == 0) return out;
      LOG_ERROR("flux_ingest: A2R transcode failed (rc=" << a2r_rc << ")");
      return {};
    }

    case Container::KryoFluxStream: {
      // A single per-track STREAM (one .raw file) → a one-track SCP.
      const int stream_rc = kryoflux_stream_to_scp(data, len, /*cyl=*/0,
                                                   /*side=*/0, out);
      if (stream_rc == 0) return out;
      LOG_ERROR("flux_ingest: KryoFlux STREAM transcode failed (rc="
                << stream_rc << ")");
      return {};
    }

    case Container::Ipf: {
      ipf::Image img;
      const ipf::Status ipf_status = img.open(data, len);
      if (ipf_status == ipf::Status::Ok) {
        out = scp_from_mfm_tracks(img.mirror_side0(/*revs=*/3));
        if (out.empty())
          LOG_ERROR("flux_ingest: clean IPF decoded to no flux (empty image?)");
        return out;
      }
      if (ipf_status == ipf::Status::UnsupportedEncoder) {
        LOG_ERROR(
            "flux_ingest: clean IPF decoder: CAPS-encoder IPF unsupported; "
            "re-image as SPS/SCP/HFE");
        return {};
      }
      LOG_ERROR("flux_ingest: IPF open failed: " << ipf::status_str(ipf_status));
      return {};
    }

    case Container::Unknown:
    default:
      LOG_ERROR("flux_ingest: unrecognised flux container (no known magic)");
      return {};
  }
}

}  // namespace flux
