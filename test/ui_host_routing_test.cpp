// beads-cv2.6: the runtime reaches the UI only through IUiHost, so a build
// without the modern UI links (KONCPC_MODERN_UI=0) and runs on the null host.
// These pin both halves: the main loop's UI requests go through the host, and
// the null host's answers keep a UI-less run working.

#include <SDL3/SDL_rect.h>
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "imgui_state.h"
#include "iui_host.h"
#include "keyboard.h"
#include "koncepcja.h"
#include "log.h"
#include "menu_actions.h"

extern t_CPC CPC;

namespace {

class DebuggerRecordingHost final : public IUiHost {
 public:
  void process_event(const SDL_Event& /*ev*/) override {}
  bool wants_capture_keyboard() const override { return false; }
  bool wants_capture_mouse() const override { return false; }
  bool any_keyboard_ui_active() const override { return false; }
  void toast(UiToastLevel /*level*/, const std::string& /*msg*/) override {}
  int topbar_height() const override { return 0; }

  bool has_debugger_ui() const override { return true; }
  void set_debugger_visible(bool visible) override {
    visibility.push_back(visible);
  }

  std::vector<bool> visibility;
};

// F12 / Tools ▸ DevTools used to close the DevTools windows by calling
// g_devtools_ui directly, which a build without DevTools cannot link.
TEST(UiHostRoutingTest, DevtoolsToggleShowsAndHidesTheDebuggerThroughTheHost) {
  bool const saved_devtools = imgui_state.show_devtools;
  bool const saved_verbose = log_verbose;
  imgui_state.show_devtools = false;

  DebuggerRecordingHost rec;
  {
    UiHostOverride const use(&rec);
    koncpc_menu_action(KONCPC_DEVTOOLS);
    EXPECT_TRUE(imgui_state.show_devtools);
    EXPECT_TRUE(log_verbose);
    koncpc_menu_action(KONCPC_DEVTOOLS);
    EXPECT_FALSE(imgui_state.show_devtools);
    EXPECT_FALSE(log_verbose);
  }
  EXPECT_EQ((std::vector<bool>{true, false}), rec.visibility);

  imgui_state.show_devtools = saved_devtools;
  log_verbose = saved_verbose;
}

// No UI to ask: F5 with unsaved disk edits must reset at once rather than
// wait on a dialog nobody will draw.
TEST(UiHostRoutingTest, NullHostDeclinesTheResetConfirmation) {
  UiHostOverride const none(nullptr);
  EXPECT_FALSE(ui_host().request_reset_confirmation());
}

// The video plugins attach the UI's draw backend through the host.  Without
// chrome, attaching must succeed (or every plugin would fail to open) and the
// SDL_Renderer plugins must draw the CPC image themselves.
TEST(UiHostRoutingTest, NullHostRenderLayerAttachesAndLeavesTheCpcImageToUs) {
  UiHostOverride const none(nullptr);
  EXPECT_TRUE(ui_host().gpu_attach(nullptr, true, 1.0F));
  EXPECT_TRUE(ui_host().renderer_attach(nullptr, nullptr, 1.0F));
  SDL_FRect const dst{0.0F, 0.0F, 768.0F, 544.0F};
  EXPECT_FALSE(ui_host().renderer_prepare_frame(nullptr, dst));
  // The rest are no-ops; they must not touch the null pointers.
  ui_host().gpu_prepare_frame(nullptr);
  ui_host().gpu_draw(nullptr, nullptr);
  ui_host().render_detached_windows();
  ui_host().gpu_detach();
  ui_host().renderer_draw(nullptr);
  ui_host().renderer_detach();
  ui_host().toggle_command_palette();
  ui_host().request_file_dialog(FileDialogAction::LoadDiskA);
  ui_host().release_video_textures();
  ui_host().await_background_work();
}

// Menu checkmarks read core state, so they work in every build (this used to
// live in imgui_ui.cpp, which a UI-less build does not compile).
TEST(UiHostRoutingTest, ActionCheckmarksFollowTheEmulatorState) {
  unsigned int const saved_speed = CPC.limit_speed;
  CPC.limit_speed = 1;
  EXPECT_TRUE(koncpc_action_is_active(KONCPC_SPEED));
  CPC.limit_speed = 0;
  EXPECT_FALSE(koncpc_action_is_active(KONCPC_SPEED));
  CPC.limit_speed = saved_speed;
  EXPECT_FALSE(koncpc_action_is_active(KONCPC_RESET)) << "not a toggle";
}

}  // namespace
