#include "kcpc/fb_ring_reader.h"

namespace kcpc {

namespace {

// shm::Region takes the base name and makes the OS name of it itself.
std::string base_of(const std::string& shm_name) {
  std::string const prefix = shm::os_name("");
  if (shm_name.compare(0, prefix.size(), prefix) == 0) {
    return shm_name.substr(prefix.size());
  }
  return shm_name;
}

}  // namespace

bool FbRingReader::open(const std::string& shm_name, std::string& error) {
  close();
  if (shm_name.empty()) {
    error = "no frame ring named (the core does not publish one)";
    return false;
  }
  if (!region_.open_read(base_of(shm_name), error)) return false;
  header_ = fb_ring_header(region_.data(), region_.size());
  if (header_ == nullptr) {
    error = shm_name + " holds no version " + std::to_string(kFbRingVersion) +
            " frame ring";
    region_.close();
    return false;
  }
  return true;
}

void FbRingReader::close() {
  header_ = nullptr;
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

std::size_t FbRingReader::frame_bytes() const {
  return header_ != nullptr ? static_cast<std::size_t>(header_->slot_bytes)
                            : 0;
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
  dst.resize(frame_bytes());
  return fb_ring_read_latest(region_.data(), region_.size(), dst.data(),
                             dst.size(), seq, attempts);
}

}  // namespace kcpc
