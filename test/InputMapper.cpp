#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "host_chords.h"
#include "keyboard.h"
#include "koncepcja.h"
#include "menu_actions.h"

extern t_CPC CPC;

class InputMapperTest : public testing::Test {
 public:
  void SetUp() {
    CPC.resources_path = "resources";
    CPC.InputMapper = new InputMapper(&CPC);
    CPC.InputMapper->init();
  }
};

// koncpc_reload_host_keymap() is what the Settings ▸ Input combo (and its
// Cancel path) call to make a changed CPC.kbd_layout take effect live: the
// same InputMapper::init() + joystick relayer as startup.
TEST_F(InputMapperTest, ReloadHostKeymapAppliesTheLiveLayout) {
  CPC.keyboard = 0;
  CPC.kbd_layout = "keymap_us.map";
  koncpc_reload_host_keymap();
  // Shift+1 is '!' on a US host keyboard.
  EXPECT_EQ(0x80 | MOD_CPC_SHIFT,
            CPC.InputMapper->CPCscancodeFromKeysym(SDLK_1, SDL_KMOD_LSHIFT));

  CPC.kbd_layout = "keymap_uk_linux.map";
  koncpc_reload_host_keymap();
  // Shift+3 is the pound sign on a UK host keyboard; the US map gives '#'.
  EXPECT_EQ(0x30 | MOD_CPC_SHIFT,
            CPC.InputMapper->CPCscancodeFromKeysym(SDLK_3, SDL_KMOD_RSHIFT));

  // No mapper yet (startup order): a no-op, not a crash.
  InputMapper* const mapper = CPC.InputMapper;
  CPC.InputMapper = nullptr;
  koncpc_reload_host_keymap();
  CPC.InputMapper = mapper;
}

// A host key held across the switch was pressed through the old map; its
// release would resolve through the new one to a different CPC key, leaving
// the original matrix bit down for good.  The reload releases every row.
TEST_F(InputMapperTest, ReloadHostKeymapReleasesEveryHeldKey) {
  CPC.keyboard = 0;
  CPC.kbd_layout = "keymap_us.map";
  keyboard_matrix[3].store(0xFE, std::memory_order_relaxed);  // a key down
  keyboard_matrix_live[3].store(0xFE, std::memory_order_relaxed);
  keyboard_matrix[9].store(0x7F, std::memory_order_relaxed);

  koncpc_reload_host_keymap();

  for (int i = 0; i < 16; ++i) {
    EXPECT_EQ(0xFF, keyboard_matrix[i].load(std::memory_order_relaxed))
        << "row " << i;
    EXPECT_EQ(0xFF, keyboard_matrix_live[i].load(std::memory_order_relaxed))
        << "row " << i;
  }
}

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

// beads-bqx: the native macOS menu showed no shortcuts at all, because it was
// built before the InputMapper existed and the title text is derived from the
// mapper's live bindings. These pin the text the menu builds from.
TEST_F(InputMapperTest, NativeMenuTitleCarriesTheLiveShortcut) {
  CPC.kbd_layout = "keymap_us.map";
  CPC.keyboard = 0;
  CPC.InputMapper->init();
  EXPECT_EQ("Reset  (F5)", koncpc_action_menu_title(KONCPC_RESET));
  EXPECT_EQ("Quit  (F10)", koncpc_action_menu_title(KONCPC_EXIT));

  // Built with no mapper -- the old startup order -- every suffix is empty.
  // This is the state the menu was frozen in; the fix builds it after the
  // mapper and refreshes the text each time the menu opens.
  InputMapper* const mapper = CPC.InputMapper;
  CPC.InputMapper = nullptr;
  EXPECT_EQ("Reset", koncpc_action_menu_title(KONCPC_RESET));
  CPC.InputMapper = mapper;
  EXPECT_EQ("Reset  (F5)", koncpc_action_menu_title(KONCPC_RESET));
}

TEST_F(InputMapperTest, NativeMenuHostChordsAreTextNotKeyEquivalents) {
  // The #60 rule: Cmd+O etc. appear as "  (Cmd+O)" in the title text; the
  // menu registers no key equivalent (SDL dispatches the chord).
  EXPECT_EQ("Load Disk A...  (Cmd+O)",
            koncpc_menu_title_with_shortcut(
                "Load Disk A...", host_chord_label(HostChord::OpenDisk, true)));
  EXPECT_EQ("About konCePCja",
            koncpc_menu_title_with_shortcut(
                "About konCePCja", host_chord_label(HostChord::None, true)));
  EXPECT_EQ("", koncpc_menu_title_with_shortcut(nullptr, ""));
}

// Every shipped host layout must load: load_layout() rejects a file that maps
// one CPC key twice or one host key (with its modifiers) to two CPC keys, and
// init() ignores that verdict, so a broken file would silently lose keys at
// runtime. Nothing loaded the shipped files before (beads-avox).
TEST(InputMapperLayouts, EveryShippedLayoutLoadsWithoutDuplicates) {
  int checked = 0;
  for (const auto& entry : std::filesystem::directory_iterator("resources")) {
    if (entry.path().extension() != ".map") continue;
    SCOPED_TRACE(entry.path().string());
    InputMapper mapper(&CPC);
    EXPECT_TRUE(mapper.load_layout(entry.path().string()));
    ++checked;
  }
  EXPECT_GE(checked, 10);
}

// The check above has teeth: the same host key + modifier on two CPC keys
// (what the review of #152 suspected in the positional map) is rejected,
// while the same host key with and without Shift is two distinct bindings.
TEST(InputMapperLayouts, DuplicateHostKeyIsRejected) {
  std::filesystem::path const dir = std::filesystem::temp_directory_path();
  std::filesystem::path const dup = dir / "koncpc_dup_host_key.map";
  std::filesystem::path const split = dir / "koncpc_split_host_key.map";
  {
    std::ofstream f(dup);
    f << "CPC_PIPE\tSDLK_BACKSLASH\nCPC_BACKSLASH\tSDLK_BACKSLASH\n";
  }
  {
    std::ofstream f(split);
    f << "CPC_PIPE\tSDLK_BACKSLASH\n"
         "CPC_BACKSLASH\tSDLK_BACKSLASH\tMOD_PC_SHIFT\n";
  }
  InputMapper a(&CPC);
  EXPECT_FALSE(a.load_layout(dup.string()));
  InputMapper b(&CPC);
  EXPECT_TRUE(b.load_layout(split.string()));
  std::filesystem::remove(dup);
  std::filesystem::remove(split);
}

// The positional layout keeps '|', '{' and '[' reachable: each sits on its own
// host key + modifier pair, not on a key another CPC key already took.
TEST_F(InputMapperTest, PositionalLayoutReachesPipeAndBrackets) {
  CPC.kbd_layout = "keymap_positional.map";
  CPC.InputMapper->init();
  InputMapper& m = *CPC.InputMapper;
  EXPECT_EQ(CPC_PIPE, m.CPCkeyFromKeysym(SDLK_BACKSLASH, SDL_KMOD_NONE));
  EXPECT_EQ(CPC_BACKSLASH, m.CPCkeyFromKeysym(SDLK_BACKSLASH, SDL_KMOD_LSHIFT));
  EXPECT_EQ(CPC_LCBRACE, m.CPCkeyFromKeysym(SDLK_LEFTBRACKET, SDL_KMOD_NONE));
  EXPECT_EQ(CPC_LBRACKET,
            m.CPCkeyFromKeysym(SDLK_LEFTBRACKET, SDL_KMOD_LSHIFT));
  EXPECT_EQ(CPC_COLON, m.CPCkeyFromKeysym(SDLK_SEMICOLON, SDL_KMOD_NONE));
  CPC.kbd_layout = "keymap_us.map";
  CPC.InputMapper->init();
}

// Finding #6: the stick-to-keys defaults are key names the config loader
// resolves; a renamed CPC key would otherwise warn and fall back on every
// start.
TEST(InputMapperNames, JoyToKeysDefaultsResolve) {
  for (const char* name : kJoyToKeyDefaults) {
    EXPECT_GE(InputMapper::CPCkeyFromName(name), 0) << name;
  }
  EXPECT_EQ(-1, InputMapper::CPCkeyFromName("CPC_NO_SUCH_KEY"));
}
