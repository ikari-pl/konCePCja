// AsmSourceStore: the host-owned assembler source the IPC 'asm' commands and
// the DevTools Assembler window share (beads-cv2.4).

#include "asm_source.h"

#include <gtest/gtest.h>

#include <string>

TEST(AsmSourceStore, SetReplacesTheTextAndMovesTheGeneration) {
  AsmSourceStore store;
  auto const g0 = store.generation();
  auto const g1 = store.set("ld a,1");
  EXPECT_GT(g1, g0);
  EXPECT_EQ(g1, store.generation());
  EXPECT_EQ("ld a,1", store.text());

  std::uint64_t seen = 0;
  EXPECT_EQ("ld a,1", store.text(seen));
  EXPECT_EQ(g1, seen);
}

TEST(AsmSourceStore, TextIsCappedLikeTheOldEditorBuffer) {
  AsmSourceStore store;
  store.set(std::string(AsmSourceStore::kMaxBytes + 10, 'x'));
  EXPECT_EQ(AsmSourceStore::kMaxBytes, store.text().size());
}

// The editor writes back only if nobody wrote since it loaded the text: a
// keystroke and an IPC 'asm text' in the same frame must not drop the IPC
// write.
TEST(AsmSourceStore, ConditionalSetLosesToAnInterveningWriter) {
  AsmSourceStore store;
  auto const loaded = store.set("nop");
  auto const ipc = store.set("ret");  // IPC wrote while the editor was open
  EXPECT_EQ(0u, store.set_if_generation("nop ; edited", loaded));
  EXPECT_EQ("ret", store.text());
  EXPECT_EQ(ipc, store.generation());

  auto const edited = store.set_if_generation("ret ; edited", ipc);
  EXPECT_GT(edited, ipc);
  EXPECT_EQ("ret ; edited", store.text());
}
