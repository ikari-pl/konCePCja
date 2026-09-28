// The F1 pause hub, rendered headlessly.
//
// The hub offered Resume / Reset / Screenshot and a save-state grid, but no
// way to eject media and no way into the debugger — the two things a paused
// user reaches for (beads-7sh). The enable logic for the Eject row is pure
// (hub_media_buttons); the render tests prove the row and the DevTools button
// actually draw, by vertex count, the way test/devtools_render_test.cpp does.

#include <gtest/gtest.h>

#include <filesystem>
#include <functional>

#include "headless_imgui.h"
#include "imgui.h"
#include "imgui_state.h"
#include "imgui_ui.h"
#include "imgui_ui_testable.h"
#include "koncepcja.h"

extern t_CPC CPC;

namespace {

using koncpc_test::HeadlessImGui;

class PauseHubRenderTest : public ::testing::Test {
 protected:
  void SetUp() override {
    saved_state_ = imgui_state;
    saved_cpc_ = CPC;
    imgui_state = ImGuiUIState{};
    imgui_state.show_menu = true;
    CPC.paused = true;
    // The save-state grid creates <snap_path>/states/ on first draw; keep
    // that out of the test's working directory.
    snap_dir_ =
        std::filesystem::temp_directory_path() / "koncepcja-pause-hub-test";
    std::filesystem::create_directories(snap_dir_);
    CPC.snap_path = snap_dir_.string();
  }
  void TearDown() override {
    imgui_state = saved_state_;
    CPC = saved_cpc_;
    std::error_code ec;
    std::filesystem::remove_all(snap_dir_, ec);
  }
  std::filesystem::path snap_dir_;
  ImGuiUIState saved_state_;
  t_CPC saved_cpc_;
};

}  // namespace

TEST(HubMediaButtons, EjectIsEnabledOnlyForMediaThatIsThere) {
  HubMediaButtons const none = hub_media_buttons(false, false, false);
  EXPECT_STREQ("Eject A", none.button[0].label);
  EXPECT_STREQ("Eject B", none.button[1].label);
  EXPECT_STREQ("Eject Tape", none.button[2].label);
  for (const HubMediaButton& b : none.button) EXPECT_FALSE(b.enabled);

  HubMediaButtons const a_only = hub_media_buttons(true, false, false);
  EXPECT_TRUE(a_only.button[0].enabled);
  EXPECT_FALSE(a_only.button[1].enabled);
  EXPECT_FALSE(a_only.button[2].enabled);

  HubMediaButtons const tape = hub_media_buttons(false, false, true);
  EXPECT_FALSE(tape.button[0].enabled);
  EXPECT_TRUE(tape.button[2].enabled);
}

TEST_F(PauseHubRenderTest, DrawsTheTransportRowsAndTheDevToolsButton) {
  HeadlessImGui gui;
  int const vtx = gui.settled_frames([] { imgui_render_menu(); });
  // Resume, Reset, Screenshot, three Eject buttons, DevTools, the status
  // columns and the eight save-state cells: well over a hundred quads.
  EXPECT_GT(vtx, 400)
      << "the hub drew almost nothing: a widget early-returned or the window "
         "collapsed";
}

TEST_F(PauseHubRenderTest, TheMediaRowItselfDraws) {
  // Differential, not a floor: the same window with and without the row.
  HeadlessImGui gui;
  auto window = [](const std::function<void()>& body) {
    ImGui::SetNextWindowSize(ImVec2(360, 300));
    ImGui::Begin("row-test", nullptr, ImGuiWindowFlags_NoSavedSettings);
    body();
    ImGui::End();
  };
  int const without = gui.settled_frames([&] { window([] {}); });
  int const with_all_disabled = gui.settled_frames([&] {
    window([] {
      imgui_render_hub_media_row(hub_media_buttons(false, false, false),
                                 340.0f);
    });
  });
  int const with_all_enabled = gui.settled_frames([&] {
    window([] {
      imgui_render_hub_media_row(hub_media_buttons(true, true, true), 340.0f);
    });
  });
  EXPECT_GT(with_all_disabled, without)
      << "three Eject buttons and DevTools must add geometry";
  EXPECT_EQ(with_all_enabled, with_all_disabled)
      << "enabled vs disabled changes colour, not geometry";
}

TEST_F(PauseHubRenderTest, EjectAndDevToolsDoNotFireUnclicked) {
  // Rendering must not raise the eject confirmations or toggle DevTools by
  // itself — only a click may (the buttons share the Media menu's flow).
  HeadlessImGui gui;
  gui.settled_frames([] { imgui_render_menu(); });
  EXPECT_EQ(-1, imgui_state.eject_confirm_drive);
  EXPECT_FALSE(imgui_state.eject_confirm_tape);
  EXPECT_FALSE(imgui_state.show_devtools);
}
