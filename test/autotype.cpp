#include <gtest/gtest.h>
#include <vector>
#include <utility>
#include "autotype.h"
#include "keyboard.h"

// Record of key apply calls for testing
struct KeyCall {
  uint16_t cpc_key;
  bool pressed;
};

class AutoTypeTest : public ::testing::Test {
protected:
  AutoTypeQueue queue;
  std::vector<KeyCall> calls;

  AutoTypeKeyFunc recorder() {
    return [this](uint16_t cpc_key, bool pressed) {
      calls.push_back({cpc_key, pressed});
    };
  }

  void SetUp() override {
    queue.clear();
    calls.clear();
  }
};

// --- Parser tests ---

TEST_F(AutoTypeTest, BasicText) {
  auto err = queue.enqueue("HELLO");
  EXPECT_EQ(err, "");
  // H E L L O = 5 chars, each CHAR_PRESS_RELEASE
  EXPECT_EQ(queue.remaining(), 5u);
  auto& actions = queue.actions();
  EXPECT_EQ(actions[0].type, AutoTypeAction::CHAR_PRESS_RELEASE);
  EXPECT_EQ(actions[0].cpc_key, static_cast<uint16_t>(CPC_H));
  EXPECT_EQ(actions[1].cpc_key, static_cast<uint16_t>(CPC_E));
  EXPECT_EQ(actions[2].cpc_key, static_cast<uint16_t>(CPC_L));
  EXPECT_EQ(actions[3].cpc_key, static_cast<uint16_t>(CPC_L));
  EXPECT_EQ(actions[4].cpc_key, static_cast<uint16_t>(CPC_O));
}

TEST_F(AutoTypeTest, LowercaseText) {
  auto err = queue.enqueue("abc");
  EXPECT_EQ(err, "");
  EXPECT_EQ(queue.remaining(), 3u);
  auto& actions = queue.actions();
  EXPECT_EQ(actions[0].cpc_key, static_cast<uint16_t>(CPC_a));
  EXPECT_EQ(actions[1].cpc_key, static_cast<uint16_t>(CPC_b));
  EXPECT_EQ(actions[2].cpc_key, static_cast<uint16_t>(CPC_c));
}

TEST_F(AutoTypeTest, SpecialKeyReturn) {
  auto err = queue.enqueue("~RETURN~");
  EXPECT_EQ(err, "");
  EXPECT_EQ(queue.remaining(), 1u);
  auto& actions = queue.actions();
  EXPECT_EQ(actions[0].type, AutoTypeAction::CHAR_PRESS_RELEASE);
  EXPECT_EQ(actions[0].cpc_key, static_cast<uint16_t>(CPC_RETURN));
}

TEST_F(AutoTypeTest, SpecialKeySpace) {
  auto err = queue.enqueue("~SPACE~");
  EXPECT_EQ(err, "");
  EXPECT_EQ(queue.remaining(), 1u);
  EXPECT_EQ(queue.actions()[0].cpc_key, static_cast<uint16_t>(CPC_SPACE));
}

TEST_F(AutoTypeTest, SpecialKeyCaseInsensitive) {
  auto err = queue.enqueue("~return~");
  EXPECT_EQ(err, "");
  EXPECT_EQ(queue.remaining(), 1u);
  EXPECT_EQ(queue.actions()[0].cpc_key, static_cast<uint16_t>(CPC_RETURN));
}

TEST_F(AutoTypeTest, LiteralTilde) {
  // ~~ should produce nothing (tilde not in CPC char map)
  auto err = queue.enqueue("a~~b");
  EXPECT_EQ(err, "");
  EXPECT_EQ(queue.remaining(), 2u);  // just 'a' and 'b', tilde skipped
  EXPECT_EQ(queue.actions()[0].cpc_key, static_cast<uint16_t>(CPC_a));
  EXPECT_EQ(queue.actions()[1].cpc_key, static_cast<uint16_t>(CPC_b));
}

TEST_F(AutoTypeTest, KeyHoldPress) {
  auto err = queue.enqueue("~+SHIFT~");
  EXPECT_EQ(err, "");
  EXPECT_EQ(queue.remaining(), 1u);
  EXPECT_EQ(queue.actions()[0].type, AutoTypeAction::KEY_PRESS);
  EXPECT_EQ(queue.actions()[0].cpc_key, static_cast<uint16_t>(CPC_LSHIFT));
}

TEST_F(AutoTypeTest, KeyHoldRelease) {
  auto err = queue.enqueue("~-SHIFT~");
  EXPECT_EQ(err, "");
  EXPECT_EQ(queue.remaining(), 1u);
  EXPECT_EQ(queue.actions()[0].type, AutoTypeAction::KEY_RELEASE);
  EXPECT_EQ(queue.actions()[0].cpc_key, static_cast<uint16_t>(CPC_LSHIFT));
}

TEST_F(AutoTypeTest, KeyHoldSingleChar) {
  auto err = queue.enqueue("~+A~");
  EXPECT_EQ(err, "");
  EXPECT_EQ(queue.remaining(), 1u);
  EXPECT_EQ(queue.actions()[0].type, AutoTypeAction::KEY_PRESS);
  EXPECT_EQ(queue.actions()[0].cpc_key, static_cast<uint16_t>(CPC_A));
}

TEST_F(AutoTypeTest, PauseFrames) {
  auto err = queue.enqueue("~PAUSE 5~");
  EXPECT_EQ(err, "");
  EXPECT_EQ(queue.remaining(), 1u);
  EXPECT_EQ(queue.actions()[0].type, AutoTypeAction::PAUSE);
  EXPECT_EQ(queue.actions()[0].pause_frames, 5);
}

TEST_F(AutoTypeTest, PauseLargeValue) {
  auto err = queue.enqueue("~PAUSE 100~");
  EXPECT_EQ(err, "");
  EXPECT_EQ(queue.actions()[0].pause_frames, 100);
}

TEST_F(AutoTypeTest, MixedRunQuote) {
  // RUN"<RETURN> is a common CPC command
  auto err = queue.enqueue("RUN\"~RETURN~");
  EXPECT_EQ(err, "");
  EXPECT_EQ(queue.remaining(), 5u);  // R U N " RETURN
  auto& a = queue.actions();
  EXPECT_EQ(a[0].cpc_key, static_cast<uint16_t>(CPC_R));
  EXPECT_EQ(a[1].cpc_key, static_cast<uint16_t>(CPC_U));
  EXPECT_EQ(a[2].cpc_key, static_cast<uint16_t>(CPC_N));
  EXPECT_EQ(a[3].cpc_key, static_cast<uint16_t>(CPC_DBLQUOTE));
  EXPECT_EQ(a[4].cpc_key, static_cast<uint16_t>(CPC_RETURN));
}

TEST_F(AutoTypeTest, ErrorUnrecognizedKey) {
  auto err = queue.enqueue("~FOO~");
  EXPECT_NE(err, "");
  EXPECT_NE(err.find("FOO"), std::string::npos);
  // Queue should be empty since enqueue failed before appending
  EXPECT_EQ(queue.remaining(), 0u);
}

TEST_F(AutoTypeTest, ErrorUnclosedTilde) {
  auto err = queue.enqueue("hello~RETURN");
  EXPECT_NE(err, "");
  EXPECT_NE(err.find("unclosed"), std::string::npos);
}

TEST_F(AutoTypeTest, ErrorBadPause) {
  auto err = queue.enqueue("~PAUSE abc~");
  EXPECT_NE(err, "");
}

TEST_F(AutoTypeTest, ErrorPauseZero) {
  auto err = queue.enqueue("~PAUSE 0~");
  EXPECT_NE(err, "");
}

TEST_F(AutoTypeTest, FunctionKeys) {
  auto err = queue.enqueue("~F0~~F9~");
  EXPECT_EQ(err, "");
  EXPECT_EQ(queue.remaining(), 2u);
  EXPECT_EQ(queue.actions()[0].cpc_key, static_cast<uint16_t>(CPC_F0));
  EXPECT_EQ(queue.actions()[1].cpc_key, static_cast<uint16_t>(CPC_F9));
}

TEST_F(AutoTypeTest, CursorKeys) {
  auto err = queue.enqueue("~UP~~DOWN~~LEFT~~RIGHT~");
  EXPECT_EQ(err, "");
  EXPECT_EQ(queue.remaining(), 4u);
  EXPECT_EQ(queue.actions()[0].cpc_key, static_cast<uint16_t>(CPC_CUR_UP));
  EXPECT_EQ(queue.actions()[1].cpc_key, static_cast<uint16_t>(CPC_CUR_DOWN));
  EXPECT_EQ(queue.actions()[2].cpc_key, static_cast<uint16_t>(CPC_CUR_LEFT));
  EXPECT_EQ(queue.actions()[3].cpc_key, static_cast<uint16_t>(CPC_CUR_RIGHT));
}

TEST_F(AutoTypeTest, DigitsAndSymbols) {
  auto err = queue.enqueue("10 PRINT \"HELLO\"");
  EXPECT_EQ(err, "");
  // 1 0 ' ' P R I N T ' ' " H E L L O "
  EXPECT_EQ(queue.remaining(), 16u);
}

TEST_F(AutoTypeTest, EnqueueAppendsToExisting) {
  queue.enqueue("A");
  queue.enqueue("B");
  EXPECT_EQ(queue.remaining(), 2u);
  EXPECT_EQ(queue.actions()[0].cpc_key, static_cast<uint16_t>(CPC_A));
  EXPECT_EQ(queue.actions()[1].cpc_key, static_cast<uint16_t>(CPC_B));
}

TEST_F(AutoTypeTest, UnmappableCharsSkipped) {
  // Tilde is not in the CPC char map; other unmappable chars should be skipped
  auto err = queue.enqueue("a\x01" "b");  // 0x01 is not mappable
  EXPECT_EQ(err, "");
  EXPECT_EQ(queue.remaining(), 2u);  // only 'a' and 'b'
}

// --- Tick tests ---

TEST_F(AutoTypeTest, TickCharPressRelease) {
  queue.enqueue("A");
  // Frame 1: press
  EXPECT_TRUE(queue.tick(recorder()));
  ASSERT_EQ(calls.size(), 1u);
  EXPECT_EQ(calls[0].cpc_key, static_cast<uint16_t>(CPC_A));
  EXPECT_TRUE(calls[0].pressed);
  EXPECT_TRUE(queue.is_active());

  // Frame 2: release
  calls.clear();
  EXPECT_FALSE(queue.tick(recorder()));  // no more actions after release
  ASSERT_EQ(calls.size(), 1u);
  EXPECT_EQ(calls[0].cpc_key, static_cast<uint16_t>(CPC_A));
  EXPECT_FALSE(calls[0].pressed);
  EXPECT_FALSE(queue.is_active());
}

TEST_F(AutoTypeTest, TickTwoChars) {
  queue.enqueue("AB");
  // Frame 1: press A
  EXPECT_TRUE(queue.tick(recorder()));
  EXPECT_EQ(calls.back().cpc_key, static_cast<uint16_t>(CPC_A));
  EXPECT_TRUE(calls.back().pressed);

  // Frame 2: release A
  EXPECT_TRUE(queue.tick(recorder()));
  EXPECT_EQ(calls.back().cpc_key, static_cast<uint16_t>(CPC_A));
  EXPECT_FALSE(calls.back().pressed);

  // Frame 3: press B
  EXPECT_TRUE(queue.tick(recorder()));
  EXPECT_EQ(calls.back().cpc_key, static_cast<uint16_t>(CPC_B));
  EXPECT_TRUE(calls.back().pressed);

  // Frame 4: release B
  EXPECT_FALSE(queue.tick(recorder()));
  EXPECT_EQ(calls.back().cpc_key, static_cast<uint16_t>(CPC_B));
  EXPECT_FALSE(calls.back().pressed);

  EXPECT_FALSE(queue.is_active());
}

TEST_F(AutoTypeTest, TickKeyPress) {
  queue.enqueue("~+SHIFT~");
  // Only press, no release
  EXPECT_FALSE(queue.tick(recorder()));  // no more actions
  ASSERT_EQ(calls.size(), 1u);
  EXPECT_EQ(calls[0].cpc_key, static_cast<uint16_t>(CPC_LSHIFT));
  EXPECT_TRUE(calls[0].pressed);
}

TEST_F(AutoTypeTest, TickKeyRelease) {
  queue.enqueue("~-SHIFT~");
  EXPECT_FALSE(queue.tick(recorder()));
  ASSERT_EQ(calls.size(), 1u);
  EXPECT_EQ(calls[0].cpc_key, static_cast<uint16_t>(CPC_LSHIFT));
  EXPECT_FALSE(calls[0].pressed);
}

TEST_F(AutoTypeTest, TickPause) {
  queue.enqueue("A~PAUSE 3~B");
  // Frame 1: press A
  EXPECT_TRUE(queue.tick(recorder()));
  EXPECT_EQ(calls.size(), 1u);

  // Frame 2: release A
  EXPECT_TRUE(queue.tick(recorder()));
  EXPECT_EQ(calls.size(), 2u);

  // Frame 3: pause starts (frame 1 of 3)
  EXPECT_TRUE(queue.tick(recorder()));
  EXPECT_EQ(calls.size(), 2u);  // no key calls during pause

  // Frame 4: pause (frame 2 of 3)
  EXPECT_TRUE(queue.tick(recorder()));
  EXPECT_EQ(calls.size(), 2u);

  // Frame 5: pause (frame 3 of 3)
  EXPECT_TRUE(queue.tick(recorder()));
  EXPECT_EQ(calls.size(), 2u);

  // Frame 6: press B
  EXPECT_TRUE(queue.tick(recorder()));
  EXPECT_EQ(calls.size(), 3u);
  EXPECT_EQ(calls[2].cpc_key, static_cast<uint16_t>(CPC_B));
  EXPECT_TRUE(calls[2].pressed);

  // Frame 7: release B
  EXPECT_FALSE(queue.tick(recorder()));
  EXPECT_EQ(calls.size(), 4u);
}

TEST_F(AutoTypeTest, TickShiftedChar) {
  // Hold shift, type a, release shift
  queue.enqueue("~+SHIFT~a~-SHIFT~");
  // Frame 1: press SHIFT (KEY_PRESS, no awaiting_release)
  // KEY_PRESS returns !queue_.empty(), which is true (a and -SHIFT remaining)
  EXPECT_TRUE(queue.tick(recorder()));
  EXPECT_EQ(calls.size(), 1u);
  EXPECT_TRUE(calls[0].pressed);

  // Frame 2: press 'a' (CHAR_PRESS_RELEASE)
  EXPECT_TRUE(queue.tick(recorder()));
  EXPECT_EQ(calls.size(), 2u);
  EXPECT_EQ(calls[1].cpc_key, static_cast<uint16_t>(CPC_a));
  EXPECT_TRUE(calls[1].pressed);

  // Frame 3: release 'a'
  EXPECT_TRUE(queue.tick(recorder()));
  EXPECT_EQ(calls.size(), 3u);
  EXPECT_FALSE(calls[2].pressed);

  // Frame 4: release SHIFT
  EXPECT_FALSE(queue.tick(recorder()));
  EXPECT_EQ(calls.size(), 4u);
  EXPECT_EQ(calls[3].cpc_key, static_cast<uint16_t>(CPC_LSHIFT));
  EXPECT_FALSE(calls[3].pressed);
}

TEST_F(AutoTypeTest, TickEmpty) {
  EXPECT_FALSE(queue.tick(recorder()));
  EXPECT_EQ(calls.size(), 0u);
}

TEST_F(AutoTypeTest, ClearWhileActive) {
  queue.enqueue("ABCDEF");
  queue.tick(recorder());  // press A
  EXPECT_TRUE(queue.is_active());
  queue.clear();
  EXPECT_FALSE(queue.is_active());
  EXPECT_EQ(queue.remaining(), 0u);
  EXPECT_FALSE(queue.tick(recorder()));
}

TEST_F(AutoTypeTest, StatusIdle) {
  EXPECT_FALSE(queue.is_active());
  EXPECT_EQ(queue.remaining(), 0u);
}

TEST_F(AutoTypeTest, StatusActive) {
  queue.enqueue("A");
  EXPECT_TRUE(queue.is_active());
  EXPECT_EQ(queue.remaining(), 1u);
}

TEST_F(AutoTypeTest, ControlKey) {
  auto err = queue.enqueue("~CONTROL~");
  EXPECT_EQ(err, "");
  EXPECT_EQ(queue.actions()[0].cpc_key, static_cast<uint16_t>(CPC_CONTROL));
}

TEST_F(AutoTypeTest, EscKey) {
  auto err = queue.enqueue("~ESC~");
  EXPECT_EQ(err, "");
  EXPECT_EQ(queue.actions()[0].cpc_key, static_cast<uint16_t>(CPC_ESC));
}

TEST_F(AutoTypeTest, CopyKey) {
  auto err = queue.enqueue("~COPY~");
  EXPECT_EQ(err, "");
  EXPECT_EQ(queue.actions()[0].cpc_key, static_cast<uint16_t>(CPC_COPY));
}

TEST_F(AutoTypeTest, TabAndDel) {
  auto err = queue.enqueue("~TAB~~DEL~");
  EXPECT_EQ(err, "");
  EXPECT_EQ(queue.remaining(), 2u);
  EXPECT_EQ(queue.actions()[0].cpc_key, static_cast<uint16_t>(CPC_TAB));
  EXPECT_EQ(queue.actions()[1].cpc_key, static_cast<uint16_t>(CPC_DEL));
}

TEST_F(AutoTypeTest, ClrKey) {
  auto err = queue.enqueue("~CLR~");
  EXPECT_EQ(err, "");
  EXPECT_EQ(queue.actions()[0].cpc_key, static_cast<uint16_t>(CPC_CLR));
}

TEST_F(AutoTypeTest, PauseOneFrame) {
  queue.enqueue("~PAUSE 1~");
  // The pause counts this frame, so only 1 frame
  EXPECT_TRUE(queue.tick(recorder()));  // pause frame (counter = 0 after -1)
  EXPECT_FALSE(queue.tick(recorder())); // queue empty
}
