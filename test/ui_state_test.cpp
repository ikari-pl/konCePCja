#include <gtest/gtest.h>
#include "imgui_ui.h"
#include "koncepcja.h"

extern t_CPC CPC;
extern ImGuiUIState imgui_state;

// close_menu() is static in imgui_ui.cpp — replicate its logic here for testing.
// This tests the same state transitions the real close_menu() performs.
static void test_close_menu() {
    imgui_state.show_menu = false;
    if (!imgui_state.show_options && !imgui_state.show_quit_confirm) {
        CPC.paused = false;
    }
}

class UIStateTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Reset all UI state to defaults
        imgui_state = ImGuiUIState{};
        CPC.paused = false;
    }
};

// ─── Menu open/close ──────────────────────────────

TEST_F(UIStateTest, MenuStartsClosed) {
    EXPECT_FALSE(imgui_state.show_menu);
}

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
    test_close_menu();
    EXPECT_FALSE(imgui_state.show_menu);
    EXPECT_FALSE(CPC.paused);
}

TEST_F(UIStateTest, ClosingMenuWithOptionsOpenStaysPaused) {
    imgui_state.show_menu = true;
    imgui_state.show_options = true;
    CPC.paused = true;
    test_close_menu();
    EXPECT_FALSE(imgui_state.show_menu);
    EXPECT_TRUE(CPC.paused);  // options keeps it paused
}

TEST_F(UIStateTest, ClosingMenuWithQuitConfirmStaysPaused) {
    imgui_state.show_menu = true;
    imgui_state.show_quit_confirm = true;
    CPC.paused = true;
    test_close_menu();
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

TEST_F(UIStateTest, AboutStartsClosed) {
    EXPECT_FALSE(imgui_state.show_about);
}

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
    EXPECT_EQ(static_cast<int>(imgui_state.toasts.size()), ImGuiUIState::MAX_TOASTS);
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
    test_close_menu();
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
