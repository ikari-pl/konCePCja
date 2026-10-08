#pragma once

// The assembler's source text, owned by the host (beads-cv2.4).
//
// The IPC 'asm' commands and the DevTools Assembler window edit the same
// program.  The text used to live inside the DevTools TextEditor, so IPC could
// only reach it through the UI, and a build without DevTools had no assembler
// source at all.  The store is the source of truth; the Assembler window
// mirrors it (loads the text when the generation moves, writes its edits
// back with set_if_generation()).
//
// Thread-safe: the IPC thread writes it while the render thread reads it.

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>

class AsmSourceStore {
 public:
  // Same cap as the old DevTools shadow buffer (64 KB including the NUL).
  static constexpr std::size_t kMaxBytes = 65535;

  // Replaces the text (truncated to kMaxBytes) and returns the new generation.
  std::uint64_t set(std::string text);

  // Replaces the text only if nobody wrote since `expected` was read.
  // Returns the new generation, or 0 when another writer got there first
  // (whose text wins).  The Assembler window uses this so a keystroke and an
  // IPC 'asm text' in the same frame cannot silently drop the IPC write.
  std::uint64_t set_if_generation(std::string text, std::uint64_t expected);

  std::string text() const;
  // Text and the generation it belongs to, read atomically.
  std::string text(std::uint64_t& generation) const;
  std::uint64_t generation() const;

 private:
  mutable std::mutex mutex_;
  std::string text_;
  std::uint64_t generation_ = 1;  // 0 is set_if_generation's "lost" result
};

extern AsmSourceStore g_asm_source;
