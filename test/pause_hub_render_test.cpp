// The F1 pause hub, rendered headlessly.
//
// The hub offered Resume / Reset / Screenshot and a save-state grid, but no
// way to eject media and no way into the debugger — the two things a paused
// user reaches for (beads-7sh). The enable logic for the Eject row is pure
// (hub_media_buttons); the render tests prove the row and the DevTools button
// actually draw, by vertex count, the way test/devtools_render_test.cpp does.

#include <gtest/gtest.h>

#include <functional>

#include "imgui.h"
#include "imgui_state.h"
#include "imgui_ui.h"
#include "imgui_ui_testable.h"
#include "koncepcja.h"

extern t_CPC CPC;

namespace {

class HeadlessImGui {
 public:
  HeadlessImGui() {
    ctx_ = ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(1600.0f, 1000.0f);
    io.DeltaTime = 1.0f / 60.0f;
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.Fonts->AddFontDefault();
    unsigned char* pixels = nullptr;
    int tex_w = 0;
    int tex_h = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &tex_w, &tex_h);
    io.Fonts->SetTexID(static_cast<ImTextureID>(1));
  }
  HeadlessImGui(const HeadlessImGui&) = delete;
  HeadlessImGui& operator=(const HeadlessImGui&) = delete;
  HeadlessImGui(HeadlessImGui&&) = delete;
  HeadlessImGui& operator=(HeadlessImGui&&) = delete;
  ~HeadlessImGui() { ImGui::DestroyContext(ctx_); }

  int frame(const std::function<void()>& body) {
    ImGui::NewFrame();
    body();
    ImGui::Render();
    return ImGui::GetDrawData()->TotalVtxCount;
  }
  int settled_frames(const std::function<void()>& body, int n = 3) {
    int vtx = 0;
    for (int i = 0; i < n; i++) vtx = frame(body);
    return vtx;
  }

 private:
  ImGuiContext* ctx_ = nullptr;
};

class PauseHubRenderTest : public ::testing::Test {
 protected:
  void SetUp() override {
    saved_state_ = imgui_state;
    saved_cpc_ = CPC;
    imgui_state = ImGuiUIState{};
    imgui_state.show_menu = true;
    CPC.paused = true;
  }
  void TearDown() override {
    imgui_state = saved_state_;
    CPC = saved_cpc_;
  }
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

TEST_F(PauseHubRenderTest, EjectAndDevToolsDoNotFireUnclicked) {
  // Rendering must not raise the eject confirmations or toggle DevTools by
  // itself — only a click may (the buttons share the Media menu's flow).
  HeadlessImGui gui;
  gui.settled_frames([] { imgui_render_menu(); });
  EXPECT_EQ(-1, imgui_state.eject_confirm_drive);
  EXPECT_FALSE(imgui_state.eject_confirm_tape);
  EXPECT_FALSE(imgui_state.show_devtools);
}
