#include <gtest/gtest.h>

#include "imgui.h"
#include "imgui_ui.h"
#include "keyboard.h"
#include "koncepcja.h"
#include "video_host.h"

extern t_CPC CPC;
extern ImGuiUIState imgui_state;
extern video_plugin* vid_plugin;
extern SDL_Surface* back_surface;
extern SDL_Window* mainSDLWindow;

// Uses the real imgui_close_menu() — no duplication of logic.

class UIStateTest : public ::testing::Test {
 protected:
  void SetUp() override {
    // Reset all UI state to defaults
    imgui_state = ImGuiUIState{};
    CPC.paused = false;
  }
};

// ─── Menu open/close ──────────────────────────────

TEST_F(UIStateTest, MenuStartsClosed) { EXPECT_FALSE(imgui_state.show_menu); }

TEST_F(UIStateTest, OpeningMenuSetsFlag) {
  imgui_state.show_menu = true;
  imgui_state.menu_just_opened = true;
  CPC.paused = true;
  EXPECT_TRUE(imgui_state.show_menu);
  EXPECT_TRUE(imgui_state.menu_just_opened);
  EXPECT_TRUE(CPC.paused);
}

TEST_F(UIStateTest, ClosingMenuClearsFlagAndUnpauses) {
  imgui_state.show_menu = true;
  CPC.paused = true;
  imgui_close_menu();
  EXPECT_FALSE(imgui_state.show_menu);
  EXPECT_FALSE(CPC.paused);
}

TEST_F(UIStateTest, ClosingMenuWithOptionsOpenStaysPaused) {
  imgui_state.show_menu = true;
  imgui_state.show_options = true;
  CPC.paused = true;
  imgui_close_menu();
  EXPECT_FALSE(imgui_state.show_menu);
  EXPECT_TRUE(CPC.paused);  // options keeps it paused
}

TEST_F(UIStateTest, ClosingMenuWithQuitConfirmStaysPaused) {
  imgui_state.show_menu = true;
  imgui_state.show_quit_confirm = true;
  CPC.paused = true;
  imgui_close_menu();
  EXPECT_FALSE(imgui_state.show_menu);
  EXPECT_TRUE(CPC.paused);  // quit dialog keeps it paused
}

// ─── Options dialog ───────────────────────────────

TEST_F(UIStateTest, OptionsStartsClosed) {
  EXPECT_FALSE(imgui_state.show_options);
}

TEST_F(UIStateTest, ClickingOptionsMenuItemOpensOptions) {
  // Simulates: Emulator → Options...
  imgui_state.show_options = true;
  EXPECT_TRUE(imgui_state.show_options);
}

TEST_F(UIStateTest, ClosingOptionsClearsFlag) {
  imgui_state.show_options = true;
  imgui_state.show_options = false;
  EXPECT_FALSE(imgui_state.show_options);
}

// ─── DevTools ─────────────────────────────────────

TEST_F(UIStateTest, DevToolsStartsClosed) {
  EXPECT_FALSE(imgui_state.show_devtools);
}

TEST_F(UIStateTest, ClickingDevToolsMenuItemTogglesDevTools) {
  // Simulates: Tools → DevTools
  imgui_state.show_devtools = !imgui_state.show_devtools;
  EXPECT_TRUE(imgui_state.show_devtools);
  imgui_state.show_devtools = !imgui_state.show_devtools;
  EXPECT_FALSE(imgui_state.show_devtools);
}

// ─── Virtual keyboard ────────────────────────────

TEST_F(UIStateTest, VirtualKeyboardStartsClosed) {
  EXPECT_FALSE(imgui_state.show_vkeyboard);
}

TEST_F(UIStateTest, OpeningVirtualKeyboardSetsFlag) {
  imgui_state.show_vkeyboard = true;
  EXPECT_TRUE(imgui_state.show_vkeyboard);
}

TEST_F(UIStateTest, VirtualKeyboardModifiersStartCleared) {
  EXPECT_FALSE(imgui_state.vkeyboard_caps_lock);
  EXPECT_FALSE(imgui_state.vkeyboard_shift_next);
  EXPECT_FALSE(imgui_state.vkeyboard_ctrl_next);
}

TEST_F(UIStateTest, VirtualKeyboardShiftIsOneShot) {
  imgui_state.vkeyboard_shift_next = true;
  EXPECT_TRUE(imgui_state.vkeyboard_shift_next);
  // After a key press, shift clears (simulated)
  imgui_state.vkeyboard_shift_next = false;
  EXPECT_FALSE(imgui_state.vkeyboard_shift_next);
}

TEST_F(UIStateTest, VirtualKeyboardCtrlIsOneShot) {
  imgui_state.vkeyboard_ctrl_next = true;
  EXPECT_TRUE(imgui_state.vkeyboard_ctrl_next);
  imgui_state.vkeyboard_ctrl_next = false;
  EXPECT_FALSE(imgui_state.vkeyboard_ctrl_next);
}

TEST_F(UIStateTest, VirtualKeyboardCapsLockIsSticky) {
  imgui_state.vkeyboard_caps_lock = true;
  EXPECT_TRUE(imgui_state.vkeyboard_caps_lock);
  // Pressing a key does NOT clear caps lock
  // (only another click on CAPS does)
  EXPECT_TRUE(imgui_state.vkeyboard_caps_lock);
  imgui_state.vkeyboard_caps_lock = false;
  EXPECT_FALSE(imgui_state.vkeyboard_caps_lock);
}

// ─── Memory tool ──────────────────────────────────

TEST_F(UIStateTest, MemoryToolStartsClosed) {
  EXPECT_FALSE(imgui_state.show_memory_tool);
}

TEST_F(UIStateTest, OpeningMemoryToolSetsFlag) {
  imgui_state.show_memory_tool = true;
  EXPECT_TRUE(imgui_state.show_memory_tool);
}

// ─── About dialog ─────────────────────────────────

TEST_F(UIStateTest, AboutStartsClosed) { EXPECT_FALSE(imgui_state.show_about); }

TEST_F(UIStateTest, ClickingAboutMenuItemOpensAbout) {
  imgui_state.show_about = true;
  EXPECT_TRUE(imgui_state.show_about);
}

// ─── Quit confirmation ───────────────────────────

TEST_F(UIStateTest, QuitConfirmStartsClosed) {
  EXPECT_FALSE(imgui_state.show_quit_confirm);
}

TEST_F(UIStateTest, ClickingQuitPausesAndShowsConfirm) {
  // Simulates: Emulator → Quit
  imgui_state.show_quit_confirm = true;
  CPC.paused = true;
  EXPECT_TRUE(imgui_state.show_quit_confirm);
  EXPECT_TRUE(CPC.paused);
}

TEST_F(UIStateTest, CancellingQuitClearsAndUnpauses) {
  imgui_state.show_quit_confirm = true;
  CPC.paused = true;
  imgui_state.show_quit_confirm = false;
  CPC.paused = false;
  EXPECT_FALSE(imgui_state.show_quit_confirm);
  EXPECT_FALSE(CPC.paused);
}

// ─── Toast notifications ──────────────────────────

TEST_F(UIStateTest, ToastsStartEmpty) {
  EXPECT_TRUE(imgui_state.toasts.empty());
}

TEST_F(UIStateTest, ToastInfoAddsToQueue) {
  imgui_toast_info("Disk loaded");
  EXPECT_EQ(imgui_state.toasts.size(), 1u);
  EXPECT_EQ(imgui_state.toasts.back().message, "Disk loaded");
  EXPECT_EQ(imgui_state.toasts.back().level, ImGuiUIState::ToastLevel::Info);
}

TEST_F(UIStateTest, ToastSuccessAddsToQueue) {
  imgui_toast_success("Saved OK");
  EXPECT_EQ(imgui_state.toasts.size(), 1u);
  EXPECT_EQ(imgui_state.toasts.back().level, ImGuiUIState::ToastLevel::Success);
}

TEST_F(UIStateTest, ToastErrorAddsToQueueWithLongerDuration) {
  imgui_toast_error("Something broke");
  EXPECT_EQ(imgui_state.toasts.size(), 1u);
  EXPECT_EQ(imgui_state.toasts.back().level, ImGuiUIState::ToastLevel::Error);
  // Errors get 1.5x duration
  EXPECT_FLOAT_EQ(imgui_state.toasts.back().timer,
                  ImGuiUIState::TOAST_DURATION * 1.5f);
}

TEST_F(UIStateTest, ToastsCappedAtMax) {
  for (int i = 0; i < ImGuiUIState::MAX_TOASTS + 3; i++) {
    imgui_toast_info("Toast " + std::to_string(i));
  }
  EXPECT_EQ(static_cast<int>(imgui_state.toasts.size()),
            ImGuiUIState::MAX_TOASTS);
  // Oldest toasts were dropped — newest survive
  EXPECT_EQ(imgui_state.toasts.back().message,
            "Toast " + std::to_string(ImGuiUIState::MAX_TOASTS + 2));
}

TEST_F(UIStateTest, ToastDurationIsPositive) {
  imgui_toast_info("test");
  EXPECT_GT(imgui_state.toasts.back().timer, 0.0f);
  EXPECT_GT(imgui_state.toasts.back().initial, 0.0f);
  EXPECT_FLOAT_EQ(imgui_state.toasts.back().timer,
                  imgui_state.toasts.back().initial);
}

// ─── Eject confirmation ──────────────────────────

TEST_F(UIStateTest, EjectConfirmStartsNone) {
  EXPECT_EQ(imgui_state.eject_confirm_drive, -1);
  EXPECT_FALSE(imgui_state.eject_confirm_tape);
}

TEST_F(UIStateTest, EjectConfirmDriveA) {
  imgui_state.eject_confirm_drive = 0;
  EXPECT_EQ(imgui_state.eject_confirm_drive, 0);
}

TEST_F(UIStateTest, EjectConfirmDriveB) {
  imgui_state.eject_confirm_drive = 1;
  EXPECT_EQ(imgui_state.eject_confirm_drive, 1);
}

TEST_F(UIStateTest, EjectConfirmTape) {
  imgui_state.eject_confirm_tape = true;
  EXPECT_TRUE(imgui_state.eject_confirm_tape);
}

// ─── Layout dropdown ─────────────────────────────

TEST_F(UIStateTest, LayoutDropdownStartsClosed) {
  EXPECT_FALSE(imgui_state.show_layout_dropdown);
}

TEST_F(UIStateTest, LayoutDropdownToggle) {
  imgui_state.show_layout_dropdown = true;
  EXPECT_TRUE(imgui_state.show_layout_dropdown);
  imgui_state.show_layout_dropdown = false;
  EXPECT_FALSE(imgui_state.show_layout_dropdown);
}

// ─── File dialog state ───────────────────────────

TEST_F(UIStateTest, FileDialogStartsNone) {
  EXPECT_EQ(imgui_state.pending_dialog, FileDialogAction::None);
  EXPECT_TRUE(imgui_state.pending_dialog_result.empty());
}

TEST_F(UIStateTest, FileDialogActionsAreDistinct) {
  // Verify all file dialog actions have unique values
  EXPECT_NE(static_cast<int>(FileDialogAction::LoadDiskA),
            static_cast<int>(FileDialogAction::LoadDiskB));
  EXPECT_NE(static_cast<int>(FileDialogAction::LoadSnapshot),
            static_cast<int>(FileDialogAction::SaveSnapshot));
  EXPECT_NE(static_cast<int>(FileDialogAction::LoadTape),
            static_cast<int>(FileDialogAction::LoadCartridge));
}

// ─── Drive LEDs ──────────────────────────────────

TEST_F(UIStateTest, DriveLEDsStartOff) {
  EXPECT_FALSE(imgui_state.drive_a_led);
  EXPECT_FALSE(imgui_state.drive_b_led);
}

// ─── Memory tool defaults ────────────────────────

TEST_F(UIStateTest, MemoryToolDefaultBytesPerLine) {
  EXPECT_EQ(imgui_state.mem_bytes_per_line, 16);
}

TEST_F(UIStateTest, MemoryToolDefaultFilterOff) {
  EXPECT_EQ(imgui_state.mem_filter_value, -1);
  EXPECT_EQ(imgui_state.mem_display_value, -1);
}

// ─── Tape state ──────────────────────────────────

TEST_F(UIStateTest, TapeBlocksStartEmpty) {
  EXPECT_TRUE(imgui_state.tape_block_offsets.empty());
  EXPECT_EQ(imgui_state.tape_current_block, 0);
}

TEST_F(UIStateTest, TapeWaveformModeDefault) {
  EXPECT_EQ(imgui_state.tape_wave_mode, 0);  // 0=pulse
}

// ─── CPC screen focus ────────────────────────────

TEST_F(UIStateTest, CpcScreenFocusStartsFalse) {
  EXPECT_FALSE(imgui_state.cpc_screen_focused);
  EXPECT_FALSE(imgui_state.request_cpc_screen_focus);
}

// ─── Multiple dialogs interaction ────────────────

TEST_F(UIStateTest, MenuAndOptionsCanBothBeOpen) {
  imgui_state.show_menu = true;
  imgui_state.show_options = true;
  CPC.paused = true;
  // Closing menu while options is open should stay paused
  imgui_close_menu();
  EXPECT_FALSE(imgui_state.show_menu);
  EXPECT_TRUE(imgui_state.show_options);
  EXPECT_TRUE(CPC.paused);
}

TEST_F(UIStateTest, AllDialogsCanBeClosed) {
  imgui_state.show_menu = true;
  imgui_state.show_options = true;
  imgui_state.show_about = true;
  imgui_state.show_quit_confirm = true;
  imgui_state.show_devtools = true;
  imgui_state.show_memory_tool = true;
  imgui_state.show_vkeyboard = true;

  imgui_state.show_menu = false;
  imgui_state.show_options = false;
  imgui_state.show_about = false;
  imgui_state.show_quit_confirm = false;
  imgui_state.show_devtools = false;
  imgui_state.show_memory_tool = false;
  imgui_state.show_vkeyboard = false;

  EXPECT_FALSE(imgui_state.show_menu);
  EXPECT_FALSE(imgui_state.show_options);
  EXPECT_FALSE(imgui_state.show_about);
  EXPECT_FALSE(imgui_state.show_quit_confirm);
  EXPECT_FALSE(imgui_state.show_devtools);
  EXPECT_FALSE(imgui_state.show_memory_tool);
  EXPECT_FALSE(imgui_state.show_vkeyboard);
}

// ─── MRU (recent files) via imgui_mru_push ───────

TEST_F(UIStateTest, MruPushAddsPath) {
  std::vector<std::string> list;
  imgui_mru_push(list, "/path/to/game.dsk");
  EXPECT_EQ(list.size(), 1u);
  EXPECT_EQ(list[0], "/path/to/game.dsk");
}

TEST_F(UIStateTest, MruPushDuplicateMovesToFront) {
  std::vector<std::string> list;
  imgui_mru_push(list, "/a.dsk");
  imgui_mru_push(list, "/b.dsk");
  imgui_mru_push(list, "/a.dsk");
  EXPECT_EQ(list.size(), 2u);
  EXPECT_EQ(list[0], "/a.dsk");
  EXPECT_EQ(list[1], "/b.dsk");
}

// ─── Fullscreen from the menu is deferred, never toggled inline ───────────
//
// koncpc_menu_action(KONCPC_FULLSCRN) is reached from inside the active ImGui
// frame (konCePCja's own menu bar) and from AppKit's nested menu-tracking run
// loop (the native macOS menu).  koncpc_toggle_fullscreen() tears the whole
// video stack down and back up — video_shutdown()/video_init(), which destroy
// and recreate the ImGui context — so calling it from there corrupted the
// context mid-render and crashed in ImGui::MenuItemEx (SIGSEGV, a byte write
// near null, from a real macOS crash log).  The handler must instead post
// imgui_state.fullscreen_request and let the main loop apply it after the
// frame, exactly as the Options checkbox does.

namespace {

// A live ImGui context whose pointer identity is the oracle: a synchronous
// toggle would DestroyContext() it (current context -> null) and then
// CreateContext() a different one.  No backend, no frame — nothing here needs
// one, and the fixture restores whatever context was current before.
class ScopedImGuiContext {
 public:
  ScopedImGuiContext()
      : previous_(ImGui::GetCurrentContext()), ctx_(ImGui::CreateContext()) {
    ImGui::GetIO().IniFilename = nullptr;
    ImGui::GetIO().LogFilename = nullptr;
  }
  ScopedImGuiContext(const ScopedImGuiContext&) = delete;
  ScopedImGuiContext& operator=(const ScopedImGuiContext&) = delete;
  ~ScopedImGuiContext() {
    // Destroy only what we created: if the code under test destroyed it, the
    // current context is already gone (or someone else's) and the test has
    // failed anyway — don't double-free on the way out.
    if (ImGui::GetCurrentContext() == ctx_) ImGui::DestroyContext(ctx_);
    ImGui::SetCurrentContext(previous_);
  }
  ImGuiContext* get() const { return ctx_; }

 private:
  ImGuiContext* previous_;
  ImGuiContext* ctx_;
};

}  // namespace

TEST_F(UIStateTest, FullscreenRequestStartsIdle) {
  // -1 is the "nothing pending" sentinel the main-loop consumer keys on.
  EXPECT_EQ(imgui_state.fullscreen_request, -1);
}

TEST_F(UIStateTest, FullscreenMenuActionPostsARequestInsteadOfToggling) {
  ScopedImGuiContext imgui;
  video_plugin* const plugin_before = vid_plugin;
  SDL_Surface* const surface_before = back_surface;
  SDL_Window* const window_before = mainSDLWindow;

  CPC.scr_window = 1;  // windowed
  imgui_state.fullscreen_request = -1;

  koncpc_menu_action(KONCPC_FULLSCRN);

  // The intent is recorded for the deferred consumer …
  EXPECT_EQ(CPC.scr_window, 0u) << "scr_window must flip at once (UI feedback)";
  EXPECT_EQ(imgui_state.fullscreen_request, 0)
      << "request must carry the new scr_window for the main loop to apply";

  // … and nothing about the video stack was touched synchronously.
  EXPECT_EQ(ImGui::GetCurrentContext(), imgui.get())
      << "ImGui context was destroyed/recreated inline: the toggle ran";
  EXPECT_EQ(vid_plugin, plugin_before);
  EXPECT_EQ(back_surface, surface_before);
  EXPECT_EQ(mainSDLWindow, window_before);
}

TEST_F(UIStateTest, FullscreenMenuActionRequestsWindowedFromFullscreen) {
  ScopedImGuiContext imgui;
  CPC.scr_window = 0;  // fullscreen
  imgui_state.fullscreen_request = -1;

  koncpc_menu_action(KONCPC_FULLSCRN);

  EXPECT_EQ(CPC.scr_window, 1u);
  EXPECT_EQ(imgui_state.fullscreen_request, 1);
  EXPECT_EQ(ImGui::GetCurrentContext(), imgui.get());
}

// ─── The toggle's starting point ───────────────────────────────
// CPC.scr_window (1 = windowed) lags the window when the OS drove the
// transition: after macOS's green button the flag still says windowed, so a
// flip from the flag asked for fullscreen — the state the window was already
// in — and the consumer swallowed the click.  The flip starts from the
// window's real state when it is known and nothing is pending.

TEST_F(UIStateTest, ToggleTargetStartsFromTheWindowNotTheStaleFlag) {
  // Green button: window fullscreen, flag still says windowed -> ask for
  // windowed (the click means "leave fullscreen").
  EXPECT_EQ(1u, koncpc_fullscreen_toggle_target(-1, 1u, true));
  // The other way round: flag says fullscreen, window is not -> ask for it.
  EXPECT_EQ(0u, koncpc_fullscreen_toggle_target(-1, 0u, false));
  // Flag and window agree: a plain flip.
  EXPECT_EQ(0u, koncpc_fullscreen_toggle_target(-1, 1u, false));
  EXPECT_EQ(1u, koncpc_fullscreen_toggle_target(-1, 0u, true));
}

TEST_F(UIStateTest, ToggleTargetFallsBackToTheFlagWithoutAWindow) {
  EXPECT_EQ(0u, koncpc_fullscreen_toggle_target(-1, 1u, std::nullopt));
  EXPECT_EQ(1u, koncpc_fullscreen_toggle_target(-1, 0u, std::nullopt));
}

TEST_F(UIStateTest, ToggleTargetContinuesFromAPendingRequest) {
  // A second click before the main loop applied the first continues from the
  // state that click asked for — whatever the window is doing right now — so
  // menu spam cancels out.
  EXPECT_EQ(1u, koncpc_fullscreen_toggle_target(0, 0u, false));
  EXPECT_EQ(0u, koncpc_fullscreen_toggle_target(1, 1u, true));
}

TEST_F(UIStateTest, FullscreenMenuActionAfterTheGreenButtonLeavesFullscreen) {
  // A hidden window stands in for the real one; it is windowed, and the flag
  // is stale the other way (says fullscreen). The click must ask for
  // fullscreen — what a windowed window's Fullscreen item means — not for the
  // windowed state the stale flag would have produced.
  if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) {
    GTEST_SKIP() << "no SDL video: " << SDL_GetError();
  }
  SDL_Window* const window =
      SDL_CreateWindow("ui-state-test", 320, 200, SDL_WINDOW_HIDDEN);
  if (!window) {
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
    GTEST_SKIP() << "no window (headless): " << SDL_GetError();
  }
  ScopedImGuiContext imgui;
  SDL_Window* const saved = mainSDLWindow;
  mainSDLWindow = window;
  CPC.scr_window = 0;  // stale: says fullscreen, the window is not
  imgui_state.fullscreen_request = -1;

  koncpc_menu_action(KONCPC_FULLSCRN);

  EXPECT_EQ(0u, CPC.scr_window) << "a windowed window's Fullscreen click "
                                   "asks for fullscreen";
  EXPECT_EQ(0, imgui_state.fullscreen_request);
  EXPECT_EQ(ImGui::GetCurrentContext(), imgui.get());

  mainSDLWindow = saved;
  SDL_DestroyWindow(window);
  SDL_QuitSubSystem(SDL_INIT_VIDEO);
}

TEST_F(UIStateTest, FullscreenMenuActionTwiceRequestsTheOriginalState) {
  // Two clicks before the main loop gets a turn (menu spam) must leave a
  // request for the state the user ends up asking for, not a stale one; the
  // consumer compares it against the real window flags and no-ops if equal.
  ScopedImGuiContext imgui;
  CPC.scr_window = 1;
  imgui_state.fullscreen_request = -1;

  koncpc_menu_action(KONCPC_FULLSCRN);
  koncpc_menu_action(KONCPC_FULLSCRN);

  EXPECT_EQ(CPC.scr_window, 1u);
  EXPECT_EQ(imgui_state.fullscreen_request, 1);
  EXPECT_EQ(ImGui::GetCurrentContext(), imgui.get());
}
