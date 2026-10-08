// Host-owned assembler source (beads-cv2.4).  See asm_source.h.

#include "asm_source.h"

#include <utility>

AsmSourceStore g_asm_source;

std::uint64_t AsmSourceStore::set(std::string text) {
  if (text.size() > kMaxBytes) text.resize(kMaxBytes);
  std::scoped_lock const lock(mutex_);
  text_ = std::move(text);
  return ++generation_;
}

std::uint64_t AsmSourceStore::set_if_generation(std::string text,
                                                std::uint64_t expected) {
  if (text.size() > kMaxBytes) text.resize(kMaxBytes);
  std::scoped_lock const lock(mutex_);
  if (generation_ != expected) return 0;
  text_ = std::move(text);
  return ++generation_;
}

std::string AsmSourceStore::text() const {
  std::scoped_lock const lock(mutex_);
  return text_;
}

std::string AsmSourceStore::text(std::uint64_t& generation) const {
  std::scoped_lock const lock(mutex_);
  generation = generation_;
  return text_;
}

std::uint64_t AsmSourceStore::generation() const {
  std::scoped_lock const lock(mutex_);
  return generation_;
}
