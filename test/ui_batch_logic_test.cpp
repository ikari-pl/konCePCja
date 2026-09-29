// Pure logic behind five UI fixes: the debugger step group (beads-4i4), the
// Disc Tools listing key (beads-p5t), toast placement (beads-ar4), the Options
// button row and its Cancel (beads-3v8, beads-ydkw). The render paths that use
// them are covered in test/devtools_render_test.cpp.

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "imgui_ui_testable.h"

// ── Debugger step group ─────────────────────────────────────────────────────

TEST(DebugStepControls, StepsOnlyWhilePausedAndNoWalkIsRunning) {
  EXPECT_TRUE(debug_step_controls(true, false, true).step_enabled);
  EXPECT_FALSE(debug_step_controls(false, false, true).step_enabled)
      << "stepping a running CPU makes no sense";
  EXPECT_FALSE(debug_step_controls(true, true, true).step_enabled)
      << "a step issued during a walk races it over the ephemeral breakpoint";
  // The two surfaces must agree: compact only changes the words.
  for (bool paused : {false, true}) {
    for (bool walking : {false, true}) {
      EXPECT_EQ(debug_step_controls(paused, walking, true).step_enabled,
                debug_step_controls(paused, walking, false).step_enabled);
      EXPECT_EQ(debug_step_controls(paused, walking, true).run_pause_resumes,
                debug_step_controls(paused, walking, false).run_pause_resumes);
    }
  }
}

TEST(DebugStepControls, RunPauseSaysWhatAClickWillDo) {
  DebugStepControls const paused = debug_step_controls(true, false, true);
  EXPECT_TRUE(paused.run_pause_resumes);
  EXPECT_STREQ("Run", paused.run_pause_label);
  DebugStepControls const running = debug_step_controls(false, false, true);
  EXPECT_FALSE(running.run_pause_resumes);
  EXPECT_STREQ("Pause", running.run_pause_label);
  EXPECT_STREQ("Resume",
               debug_step_controls(true, false, false).run_pause_label);
}

TEST(DebugStepControls, CompactLabelsForTheDisassemblyMenuBar) {
  DebugStepControls const c = debug_step_controls(true, false, true);
  EXPECT_STREQ("In", c.step_in_label);
  EXPECT_STREQ("Over", c.step_over_label);
  EXPECT_STREQ("Out", c.step_out_label);
  DebugStepControls const full = debug_step_controls(true, false, false);
  EXPECT_STREQ("Step In", full.step_in_label);
  EXPECT_STREQ("Step Over", full.step_over_label);
  EXPECT_STREQ("Step Out", full.step_out_label);
  EXPECT_STREQ("Stepping out...",
               debug_step_controls(true, true, false).step_out_label);
}

// ── Disc Tools listing key ──────────────────────────────────────────────────

TEST(DiscToolsListing, StaleWhenTheDriveOrItsMediumChanges) {
  DiscToolsMediaKey const listed{0, 7};
  EXPECT_FALSE(disc_tools_listing_stale(listed, {0, 7}));
  EXPECT_TRUE(disc_tools_listing_stale(listed, {0, 8})) << "disk swapped";
  EXPECT_TRUE(disc_tools_listing_stale(listed, {1, 7})) << "other drive";
}

TEST(DiscToolsListing, NeverListedIsAlwaysStale) {
  // The member starts at drive -1 so the first frame lists, whatever the
  // generation happens to be.
  DiscToolsMediaKey const never{-1, 0};
  EXPECT_TRUE(disc_tools_listing_stale(never, {0, 0}));
  EXPECT_TRUE(disc_tools_listing_stale(never, {1, 0}));
}

// ── Toast placement ─────────────────────────────────────────────────────────

TEST(ToastRect, AnchorsBottomRightOfTheWorkRect) {
  // A 1000x800 viewport at (2000, 100), e.g. a DevTools window on the second
  // monitor. 16px right margin, 40px from the bottom.
  ToastRect const r = toast_rect(2000, 100, 1000, 800, 200, 30, 40, 16);
  EXPECT_FLOAT_EQ(2000 + 1000 - 200 - 16, r.x);
  EXPECT_FLOAT_EQ(100 + 800 - 40 - 30, r.y);
  EXPECT_FLOAT_EQ(200, r.w);
  EXPECT_FLOAT_EQ(30, r.h);
}

TEST(ToastRect, StacksUpwardWithTheOffset) {
  ToastRect const first = toast_rect(0, 0, 800, 600, 200, 30, 40, 16);
  ToastRect const second = toast_rect(0, 0, 800, 600, 200, 30, 40 + 34, 16);
  EXPECT_FLOAT_EQ(first.y - 34, second.y);
  EXPECT_FLOAT_EQ(first.x, second.x);
}

TEST(ToastRect, ClampsIntoASmallViewport) {
  // A narrow floating window: the toast is wider than it. Keep the left edge
  // (the start of the message) inside rather than hanging off to the left.
  ToastRect const wide = toast_rect(500, 300, 150, 400, 300, 30, 40, 16);
  EXPECT_FLOAT_EQ(500, wide.x);
  // A short window with many toasts stacked: the top ones pin to the top.
  ToastRect const tall = toast_rect(500, 300, 400, 100, 200, 30, 90, 16);
  EXPECT_FLOAT_EQ(300, tall.y);
}

TEST(ToastViewport, FocusedViewportElseMain) {
  EXPECT_EQ(0, toast_viewport_index({}));
  EXPECT_EQ(0, toast_viewport_index({false, false, false}));
  EXPECT_EQ(2, toast_viewport_index({false, false, true}));
  EXPECT_EQ(0, toast_viewport_index({true, false, true}));
}

// ── Options dialog ──────────────────────────────────────────────────────────

TEST(OptionsButtonRow, MacOrderCancelApplySaveWithSaveDefault) {
  const std::vector<OptionsButtonSpec>& row = options_button_row();
  ASSERT_EQ(3u, row.size());
  EXPECT_EQ(OptionsButton::Cancel, row[0].id);
  EXPECT_EQ(OptionsButton::Apply, row[1].id);
  EXPECT_EQ(OptionsButton::Save, row[2].id);
  EXPECT_STREQ("Cancel", row[0].label);
  EXPECT_STREQ("Apply", row[1].label);
  EXPECT_STREQ("Save", row[2].label);
  int defaults = 0;
  for (const OptionsButtonSpec& b : row) defaults += b.is_default ? 1 : 0;
  EXPECT_EQ(1, defaults);
  EXPECT_TRUE(row.back().is_default) << "the default commit sits on the right";
}

TEST(OptionsButtonRow, TooltipsSaySaveWritesTheFileAndApplyDoesNot) {
  const std::vector<OptionsButtonSpec>& row = options_button_row();
  ASSERT_NE(nullptr, row[1].tooltip);
  ASSERT_NE(nullptr, row[2].tooltip);
  std::string const apply = row[1].tooltip;
  std::string const save = row[2].tooltip;
  EXPECT_NE(std::string::npos, apply.find("without saving"));
  EXPECT_NE(std::string::npos, save.find("config file"));
}

TEST(OptionsRevertPlan, CancelResizesTheWindowBackWhenScaleWasPreviewed) {
  OptionsRevertPlan const plan =
      options_revert_plan(3, 1, 1, 1, "keymap_us.map", "keymap_us.map");
  EXPECT_TRUE(plan.rescale_window);
  EXPECT_FALSE(plan.reinit_video);
  EXPECT_FALSE(plan.reload_host_keymap);
}

TEST(OptionsRevertPlan, NothingToUndoWhenNothingWasPreviewed) {
  OptionsRevertPlan const plan =
      options_revert_plan(2, 2, 4, 4, "keymap_uk.map", "keymap_uk.map");
  EXPECT_FALSE(plan.rescale_window);
  EXPECT_FALSE(plan.reinit_video);
  EXPECT_FALSE(plan.reload_host_keymap);
}

TEST(OptionsRevertPlan, StyleAndKeymapPreviewsAreUndoneToo) {
  OptionsRevertPlan const plan =
      options_revert_plan(1, 1, 5, 1, "keymap_fr.map", "keymap_us.map");
  EXPECT_FALSE(plan.rescale_window);
  EXPECT_TRUE(plan.reinit_video);
  EXPECT_TRUE(plan.reload_host_keymap);
}
