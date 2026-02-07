#include <gtest/gtest.h>

#include "koncepcja.h"
#include "keyboard.h"
#include <string>

extern t_CPC CPC;

class InputMapperTest : public testing::Test {
  public:
    void SetUp() {
      CPC.resources_path = "resources";
	  CPC.InputMapper = new InputMapper(&CPC);
	  CPC.InputMapper->init();
    }
};

TEST_F(InputMapperTest, StringToEventsSimpleString)
{
  CPC.kbd_layout ="keymap_us.map";
  CPC.keyboard = 0;
  CPC.InputMapper->init();

  std::string input = "cat";

  auto tmp = CPC.InputMapper->StringToEvents(input);
  std::vector<SDL_Event> result(tmp.begin(), tmp.end());

  ASSERT_EQ(6, result.size());

  // Result must be an alternance of key down / key up
  for(int i = 0; i < 3; ++i) {
    ASSERT_EQ(SDL_EVENT_KEY_DOWN, result[2*i].key.type);
    ASSERT_TRUE(result[2*i].key.down);
    ASSERT_EQ(SDL_EVENT_KEY_UP,   result[2*i+1].key.type);
    ASSERT_FALSE(result[2*i+1].key.down);
  }
  // Only keys without modifier
  for(int i = 0; i < 6; ++i) {
    ASSERT_EQ(SDL_KMOD_NONE, result[i].key.mod);
  }
  // Keys correspond to the input string
  ASSERT_EQ(SDLK_C, result[0].key.key);
  ASSERT_EQ(SDLK_C, result[1].key.key);
  ASSERT_EQ(SDLK_A, result[2].key.key);
  ASSERT_EQ(SDLK_A, result[3].key.key);
  ASSERT_EQ(SDLK_T, result[4].key.key);
  ASSERT_EQ(SDLK_T, result[5].key.key);
}

TEST_F(InputMapperTest, StringToEventsWithEscapedChar)
{
  CPC.kbd_layout ="keymap_us.map";
  CPC.keyboard = 0;
  CPC.InputMapper->init();

  std::string input = "run\"s\btest\n";

  auto tmp = CPC.InputMapper->StringToEvents(input);
  std::vector<SDL_Event> result(tmp.begin(), tmp.end());

  ASSERT_EQ(22, result.size());

  ASSERT_EQ(SDLK_N, result[5].key.key);
  // On US keyboard, " is on ' key with shift pressed
  ASSERT_EQ(SDLK_APOSTROPHE, result[6].key.key);
  ASSERT_EQ(SDLK_S, result[9].key.key);
  ASSERT_EQ(SDLK_BACKSPACE, result[10].key.key);
  ASSERT_EQ(SDLK_T, result[19].key.key);
  ASSERT_EQ(SDLK_RETURN, result[20].key.key);
}

TEST_F(InputMapperTest, StringToEventsWithSpecialChar)
{
  CPC.kbd_layout ="keymap_us.map";
  CPC.keyboard = 0;
  CPC.InputMapper->init();

  std::string input = "\a";
  input += CPC_ESC;

  auto tmp = CPC.InputMapper->StringToEvents(input);
  std::vector<SDL_Event> result(tmp.begin(), tmp.end());

  ASSERT_EQ(2, result.size());

  // First key event is pressing ESCAPE
  ASSERT_EQ(SDLK_ESCAPE, result[0].key.key);
  ASSERT_EQ(SDL_KMOD_NONE, result[0].key.mod);
  ASSERT_EQ(SDL_EVENT_KEY_DOWN, result[0].key.type);
  ASSERT_TRUE(result[0].key.down);
  // Second key event is releasing ESCAPE
  ASSERT_EQ(SDLK_ESCAPE, result[0].key.key);
  ASSERT_EQ(SDL_KMOD_NONE, result[1].key.mod);
  ASSERT_EQ(SDL_EVENT_KEY_UP, result[1].key.type);
  ASSERT_FALSE(result[1].key.down);
}

TEST_F(InputMapperTest, Keymapping)
{
  CPC.kbd_layout ="keymap_us.map";
  CPC.keyboard = 0;
  CPC.InputMapper->init();
  // Exclaim
  ASSERT_EQ(0x80 | MOD_CPC_SHIFT, CPC.InputMapper->CPCscancodeFromKeysym(SDLK_1, SDL_KMOD_LSHIFT));

  CPC.kbd_layout ="keymap_uk_linux.map";
  CPC.keyboard = 0;
  CPC.InputMapper->init();
  // Pound
  ASSERT_EQ(0x30 | MOD_CPC_SHIFT, CPC.InputMapper->CPCscancodeFromKeysym(SDLK_3, SDL_KMOD_RSHIFT));

  CPC.kbd_layout ="keymap_fr_win.map";
  CPC.keyboard = 1;
  CPC.InputMapper->init();
  // E acute
  ASSERT_EQ(0x81, CPC.InputMapper->CPCscancodeFromKeysym(SDLK_2, SDL_KMOD_NONE));

  CPC.kbd_layout ="keymap_es_linux.map";
  CPC.keyboard = 2;
  CPC.InputMapper->init();
  // N Tilde
  ASSERT_EQ(0x35 | MOD_CPC_SHIFT, CPC.InputMapper->CPCscancodeFromKeysym(static_cast<SDL_Keycode>(241), SDL_KMOD_LSHIFT));


}
