#include <gtest/gtest.h>
#include "io_dispatch.h"

namespace {

// Helper: make a reg_pair from high and low bytes
static reg_pair make_port(byte high, byte low) {
   reg_pair p;
   p.b.h = high;
   p.b.l = low;
   p.b.h2 = 0;
   p.b.h3 = 0;
   return p;
}

// ── Test fixtures ──────────────────────────────

class IODispatchTest : public testing::Test {
protected:
   void SetUp() override {
      io_dispatch_clear();
   }
};

// ── Registration tests ─────────────────────────

TEST_F(IODispatchTest, RegisterOutHandler) {
   static bool enabled = true;
   static bool called = false;
   called = false;

   auto handler = [](reg_pair, byte) -> bool { called = true; return true; };
   io_register_out(0xFF, handler, &enabled, "test");

   EXPECT_EQ(g_io_dispatch.out_slots[0xFF].count, 1);
   EXPECT_STREQ(g_io_dispatch.out_slots[0xFF].entries[0].name, "test");
}

TEST_F(IODispatchTest, RegisterInHandler) {
   static bool enabled = true;
   auto handler = [](reg_pair, byte&) -> bool { return true; };
   io_register_in(0xFD, handler, &enabled, "test_in");

   EXPECT_EQ(g_io_dispatch.in_slots[0xFD].count, 1);
}

TEST_F(IODispatchTest, MultipleHandlersSamePort) {
   static bool en1 = true, en2 = true;
   auto h1 = [](reg_pair, byte) -> bool { return true; };
   auto h2 = [](reg_pair, byte) -> bool { return true; };

   io_register_out(0xFE, h1, &en1, "handler1");
   io_register_out(0xFE, h2, &en2, "handler2");

   EXPECT_EQ(g_io_dispatch.out_slots[0xFE].count, 2);
}

TEST_F(IODispatchTest, ClearResetsAll) {
   static bool en = true;
   io_register_out(0xFF, [](reg_pair, byte) -> bool { return true; }, &en, "t");
   io_register_in(0xFD, [](reg_pair, byte&) -> bool { return true; }, &en, "t");

   io_dispatch_clear();

   EXPECT_EQ(g_io_dispatch.out_slots[0xFF].count, 0);
   EXPECT_EQ(g_io_dispatch.in_slots[0xFD].count, 0);
}

// ── Dispatch tests ─────────────────────────────

TEST_F(IODispatchTest, OutHandlerCalled) {
   static bool enabled = true;
   static byte last_val = 0;
   last_val = 0;

   io_register_out(0xFF, [](reg_pair, byte val) -> bool {
      last_val = val;
      return true;
   }, &enabled, "test");

   io_dispatch_out(make_port(0xFF, 0x00), 0x42);
   EXPECT_EQ(last_val, 0x42);
}

TEST_F(IODispatchTest, InHandlerModifiesRetVal) {
   static bool enabled = true;
   io_register_in(0xFD, [](reg_pair, byte& ret_val) -> bool {
      ret_val = 0x55;
      return true;
   }, &enabled, "test");

   byte result = io_dispatch_in(make_port(0xFD, 0x08), 0xFF);
   EXPECT_EQ(result, 0x55);
}

TEST_F(IODispatchTest, DisabledHandlerSkipped) {
   static bool enabled = false;
   static bool called = false;
   called = false;

   io_register_out(0xFF, [](reg_pair, byte) -> bool {
      called = true;
      return true;
   }, &enabled, "test");

   io_dispatch_out(make_port(0xFF, 0x00), 0x42);
   EXPECT_FALSE(called);
}

TEST_F(IODispatchTest, EnabledFlagCheckedDynamically) {
   static bool enabled = false;
   static byte last_val = 0;
   last_val = 0;

   io_register_out(0xFF, [](reg_pair, byte val) -> bool {
      last_val = val;
      return true;
   }, &enabled, "test");

   // Disabled — not called
   io_dispatch_out(make_port(0xFF, 0x00), 0x11);
   EXPECT_EQ(last_val, 0);

   // Enable at runtime — now called
   enabled = true;
   io_dispatch_out(make_port(0xFF, 0x00), 0x22);
   EXPECT_EQ(last_val, 0x22);
}

TEST_F(IODispatchTest, EmptySlotFastPath) {
   // No handlers registered — should return default value
   byte result = io_dispatch_in(make_port(0xAA, 0x00), 0xBB);
   EXPECT_EQ(result, 0xBB);
}

TEST_F(IODispatchTest, MultipleHandlersBothFire) {
   static bool en = true;
   static int call_count = 0;
   call_count = 0;

   io_register_out(0xFE, [](reg_pair, byte) -> bool {
      call_count++;
      return true;
   }, &en, "h1");
   io_register_out(0xFE, [](reg_pair, byte) -> bool {
      call_count++;
      return true;
   }, &en, "h2");

   io_dispatch_out(make_port(0xFE, 0x00), 0x00);
   EXPECT_EQ(call_count, 2);
}

TEST_F(IODispatchTest, WrongPortHighNotDispatched) {
   static bool en = true;
   static bool called = false;
   called = false;

   io_register_out(0xFF, [](reg_pair, byte) -> bool {
      called = true;
      return true;
   }, &en, "test");

   io_dispatch_out(make_port(0xFE, 0x00), 0x42);
   EXPECT_FALSE(called);
}

// ── Core hook tests ────────────────────────────

TEST_F(IODispatchTest, KbdReadHookAndMask) {
   static bool enabled = true;
   io_register_kbd_read_hook([](int line) -> byte {
      if (line == 9) return 0xF0;  // mask lower nibble
      return 0xFF;
   }, &enabled);

   EXPECT_EQ(io_fire_kbd_read_hooks(9), 0xF0);
   EXPECT_EQ(io_fire_kbd_read_hooks(5), 0xFF);
}

TEST_F(IODispatchTest, KbdReadHookDisabled) {
   static bool enabled = false;
   io_register_kbd_read_hook([](int) -> byte {
      return 0x00;  // would mask everything
   }, &enabled);

   // Disabled — no modification (returns 0xFF)
   EXPECT_EQ(io_fire_kbd_read_hooks(9), 0xFF);
}

TEST_F(IODispatchTest, KbdReadHookMultipleAnded) {
   static bool en = true;
   io_register_kbd_read_hook([](int) -> byte { return 0xF0; }, &en);
   io_register_kbd_read_hook([](int) -> byte { return 0x0F; }, &en);

   // Both masks ANDed: 0xF0 & 0x0F = 0x00
   EXPECT_EQ(io_fire_kbd_read_hooks(9), 0x00);
}

TEST_F(IODispatchTest, KbdLineHookFires) {
   static bool en = true;
   static int last_line = -1;
   last_line = -1;

   io_register_kbd_line_hook([](int line) { last_line = line; }, &en);

   io_fire_kbd_line_hooks(9);
   EXPECT_EQ(last_line, 9);
}

TEST_F(IODispatchTest, TapeMotorHookFires) {
   static bool en = true;
   static bool last_state = false;

   io_register_tape_motor_hook([](bool on) { last_state = on; }, &en);

   io_fire_tape_motor_hooks(true);
   EXPECT_TRUE(last_state);
   io_fire_tape_motor_hooks(false);
   EXPECT_FALSE(last_state);
}

TEST_F(IODispatchTest, FdcMotorHookFires) {
   static bool en = true;
   static bool last_state = false;

   io_register_fdc_motor_hook([](bool on) { last_state = on; }, &en);

   io_fire_fdc_motor_hooks(true);
   EXPECT_TRUE(last_state);
}

TEST_F(IODispatchTest, NoHooksReturnsFastPath) {
   // kbd_read with no hooks should return 0xFF
   EXPECT_EQ(io_fire_kbd_read_hooks(0), 0xFF);

   // Other hooks should just not crash
   io_fire_kbd_line_hooks(0);
   io_fire_tape_motor_hooks(false);
   io_fire_fdc_motor_hooks(false);
}

}  // namespace
