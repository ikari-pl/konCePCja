#include "kcpc/fb_ring_reader.h"

namespace kcpc {

bool FbRingReader::open(const std::string& shm_name, std::string& error) {
  close();
  if (shm_name.empty()) {
    error = "no frame ring named (the core does not publish one)";
    failure_ = FbOpenFailure::kNoName;
    return false;
  }
  if (!region_.open_read(shm::base_name(shm_name), error)) {
    failure_ = FbOpenFailure::kNoRegion;
    return false;
  }
  header_ = fb_ring_header(region_.data(), region_.size());
  if (header_ == nullptr) {
    // Which way it is no ring: the header's own checks, in its order.
    if (region_.size() < sizeof(FbRingHeader)) {
      error = shm_name + " is " + std::to_string(region_.size()) +
              " bytes: too small to hold a frame ring";
      failure_ = FbOpenFailure::kTooSmall;
    } else {
      const auto* h = static_cast<const FbRingHeader*>(region_.data());
      if (h->magic.load(std::memory_order_acquire) != kFbRingMagic) {
        error = shm_name + " holds no frame ring yet (try again)";
        failure_ = FbOpenFailure::kNotYet;
      } else if (h->version != kFbRingVersion) {
        error = shm_name + " is a version " + std::to_string(h->version) +
                " frame ring; this reader reads version " +
                std::to_string(kFbRingVersion);
        failure_ = FbOpenFailure::kVersion;
      } else {
        error = shm_name + " holds no frame ring this reader can use";
        failure_ = FbOpenFailure::kBadHeader;
      }
    }
    region_.close();
    return false;
  }
  frame_bytes_ = static_cast<std::size_t>(header_->slot_bytes);
  failure_ = FbOpenFailure::kNone;
  return true;
}

void FbRingReader::close() {
  header_ = nullptr;
  frame_bytes_ = 0;
  region_.close();
}

std::uint32_t FbRingReader::width() const {
  return header_ != nullptr ? header_->width : 0;
}

std::uint32_t FbRingReader::height() const {
  return header_ != nullptr ? header_->height : 0;
}

std::uint32_t FbRingReader::stride() const {
  return header_ != nullptr ? header_->stride : 0;
}

std::uint64_t FbRingReader::producer_pid() const {
  return header_ != nullptr ? header_->producer_pid : 0;
}

std::uint64_t FbRingReader::latest_seq() const {
  return header_ != nullptr
             ? header_->latest_seq.load(std::memory_order_acquire)
             : 0;
}

FbReadStatus FbRingReader::read_latest(std::vector<std::uint8_t>& dst,
                                       std::uint64_t& seq, int attempts) {
  if (header_ == nullptr) return FbReadStatus::kBadHeader;
  // The size open() validated, never the header's word of the moment: a
  // header rewritten since is caught by fb_ring_read_latest()'s own check
  // and is kBadHeader, not an allocation.
  dst.resize(frame_bytes_);
  return fb_ring_read_latest(region_.data(), region_.size(), dst.data(),
                             dst.size(), seq, attempts);
}

}  // namespace kcpc
