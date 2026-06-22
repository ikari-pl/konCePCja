#include <gtest/gtest.h>

#include <string>

#include "keyboard.h"
#include "koncepcja.h"

extern t_CPC CPC;

class InputMapperTest : public testing::Test {
 public:
  void SetUp() {
    CPC.resources_path = "resources";
    CPC.InputMapper = new InputMapper(&CPC);
    CPC.InputMapper->init();
  }
};

TEST_F(InputMapperTest, Keymapping) {
  CPC.kbd_layout = "keymap_us.map";
  CPC.keyboard = 0;
  CPC.InputMapper->init();
  // Exclaim
  ASSERT_EQ(0x80 | MOD_CPC_SHIFT,
            CPC.InputMapper->CPCscancodeFromKeysym(SDLK_1, SDL_KMOD_LSHIFT));

  CPC.kbd_layout = "keymap_uk_linux.map";
  CPC.keyboard = 0;
  CPC.InputMapper->init();
  // Pound
  ASSERT_EQ(0x30 | MOD_CPC_SHIFT,
            CPC.InputMapper->CPCscancodeFromKeysym(SDLK_3, SDL_KMOD_RSHIFT));

  CPC.kbd_layout = "keymap_fr_win.map";
  CPC.keyboard = 1;
  CPC.InputMapper->init();
  // E acute
  ASSERT_EQ(0x81,
            CPC.InputMapper->CPCscancodeFromKeysym(SDLK_2, SDL_KMOD_NONE));

  CPC.kbd_layout = "keymap_es_linux.map";
  CPC.keyboard = 2;
  CPC.InputMapper->init();
  // N Tilde
  ASSERT_EQ(0x35 | MOD_CPC_SHIFT,
            CPC.InputMapper->CPCscancodeFromKeysym(
                static_cast<SDL_Keycode>(241), SDL_KMOD_LSHIFT));
}

// Keystone: shortcut display strings are derived from the live binding map, so
// menu/UI hints can never drift from the real keys.
TEST_F(InputMapperTest, ShortcutForActionDerivesFromBindings) {
  CPC.kbd_layout = "keymap_us.map";
  CPC.keyboard = 0;
  CPC.InputMapper->init();

  // Single-binding emulator commands.
  EXPECT_EQ("F5", CPC.InputMapper->shortcutForAction(KONCPC_RESET));
  EXPECT_EQ("F10", CPC.InputMapper->shortcutForAction(KONCPC_EXIT));
  EXPECT_EQ("F8", CPC.InputMapper->shortcutForAction(KONCPC_FPS));

  // DevTools' real key after the US keymap loads is F12 (the keymap entry
  // overwrites the base Shift+F2 binding in the forward map).  The derived
  // string reports the TRUE binding — this is exactly why menu labels that
  // still say "Shift+F2" are wrong and must render from this helper instead.
  EXPECT_EQ("F12", CPC.InputMapper->shortcutForAction(KONCPC_DEVTOOLS));

  // The free-function wrapper used by every UI surface agrees.
  EXPECT_EQ(CPC.InputMapper->shortcutForAction(KONCPC_RESET),
            koncpc_action_shortcut(KONCPC_RESET));
}
