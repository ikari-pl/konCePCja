#include <gtest/gtest.h>
#include "keyboard_manager.h"
#include "keyboard.h"
#include "koncepcja.h"

extern t_CPC CPC;
extern byte bit_values[8];

// Scancode encoding: line = scancode >> 4, bit = scancode & 7
// CPC keyboard matrix is active-low: pressed = bit CLEARED, released = bit SET
//
// Scancodes used in tests (from cpc_kbd[0], lowercase/unmodified):
//   CPC_a = 0x85  -> line 8, bit 5
//   CPC_b = 0x66  -> line 6, bit 6
//   CPC_s = 0x74  -> line 7, bit 4
//   CPC_SPACE = 0x57 -> line 5, bit 7
//   CPC_RETURN = 0x22 -> line 2, bit 2
//   CPC_ENTER = 0x06 -> line 0, bit 6

static constexpr CPCScancode KEY_A     = 0x85;  // line 8, bit 5
static constexpr CPCScancode KEY_B     = 0x66;  // line 6, bit 6
static constexpr CPCScancode KEY_S     = 0x74;  // line 7, bit 4
static constexpr CPCScancode KEY_SPACE = 0x57;  // line 5, bit 7
static constexpr CPCScancode KEY_RETURN = 0x22; // line 2, bit 2
static constexpr CPCScancode KEY_ENTER = 0x06;  // line 0, bit 6

// Helper: extract line and bit from a scancode
static int scancode_line(CPCScancode sc) { return static_cast<byte>(sc) >> 4; }
static int scancode_bit(CPCScancode sc)  { return static_cast<byte>(sc) & 7; }

// Helper: check if a key is pressed (bit CLEARED) in the matrix
static bool is_pressed(byte keyboard_matrix[], CPCScancode sc) {
    int line = scancode_line(sc);
    int bit  = scancode_bit(sc);
    return (keyboard_matrix[line] & bit_values[bit]) == 0;
}

// Helper: check if a key is released (bit SET) in the matrix
static bool is_released(byte keyboard_matrix[], CPCScancode sc) {
    return !is_pressed(keyboard_matrix, sc);
}

class KeyboardManagerTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Initialize matrix to all released (0xFF)
        for (int i = 0; i < 16; i++) {
            matrix[i] = 0xFF;
        }
        // Create a fresh KeyboardManager for each test
        km = KeyboardManager();
        CPC.keyboard_support_mode = KeyboardSupportMode::Direct;
    }

    void setMode(KeyboardSupportMode mode) {
        CPC.keyboard_support_mode = mode;
    }

    byte matrix[16];
    KeyboardManager km;
};

// ============================================================
// Direct mode tests
// ============================================================

TEST_F(KeyboardManagerTest, Direct_KeydownSetsMatrixBit) {
    setMode(KeyboardSupportMode::Direct);

    ASSERT_TRUE(is_released(matrix, KEY_A));
    km.handle_keydown(KEY_A, matrix);
    EXPECT_TRUE(is_pressed(matrix, KEY_A));
}

TEST_F(KeyboardManagerTest, Direct_KeyupClearsMatrixBitImmediately) {
    setMode(KeyboardSupportMode::Direct);

    km.handle_keydown(KEY_A, matrix);
    ASSERT_TRUE(is_pressed(matrix, KEY_A));

    km.handle_keyup(KEY_A, matrix, true, /*current_frame=*/0);
    EXPECT_TRUE(is_released(matrix, KEY_A));
}

TEST_F(KeyboardManagerTest, Direct_KeyupDoesNotDeferRelease) {
    setMode(KeyboardSupportMode::Direct);

    km.handle_keydown(KEY_A, matrix);
    km.handle_keyup(KEY_A, matrix, true, 0);

    // Key should already be released without needing update()
    EXPECT_TRUE(is_released(matrix, KEY_A));

    // update() should not change anything
    km.update(matrix, 0);
    EXPECT_TRUE(is_released(matrix, KEY_A));
}

TEST_F(KeyboardManagerTest, Direct_MultipleKeysIndependent) {
    setMode(KeyboardSupportMode::Direct);

    km.handle_keydown(KEY_A, matrix);
    km.handle_keydown(KEY_B, matrix);

    EXPECT_TRUE(is_pressed(matrix, KEY_A));
    EXPECT_TRUE(is_pressed(matrix, KEY_B));

    // Release only A
    km.handle_keyup(KEY_A, matrix, false, 0);
    EXPECT_TRUE(is_released(matrix, KEY_A));
    EXPECT_TRUE(is_pressed(matrix, KEY_B));

    // Release B
    km.handle_keyup(KEY_B, matrix, false, 0);
    EXPECT_TRUE(is_released(matrix, KEY_B));
}

// ============================================================
// BufferedUntilRead mode tests
// ============================================================

TEST_F(KeyboardManagerTest, Buffered_KeydownSetsMatrixBit) {
    setMode(KeyboardSupportMode::BufferedUntilRead);

    km.handle_keydown(KEY_A, matrix);
    EXPECT_TRUE(is_pressed(matrix, KEY_A));
}

TEST_F(KeyboardManagerTest, Buffered_KeyupDefersUntilScanned) {
    setMode(KeyboardSupportMode::BufferedUntilRead);

    km.handle_keydown(KEY_A, matrix);
    ASSERT_TRUE(is_pressed(matrix, KEY_A));

    // Keyup should NOT immediately release because line hasn't been scanned
    km.handle_keyup(KEY_A, matrix, true, 0);
    EXPECT_TRUE(is_pressed(matrix, KEY_A));  // Still pressed!
}

TEST_F(KeyboardManagerTest, Buffered_NotifyScannedThenUpdateReleasesKey) {
    setMode(KeyboardSupportMode::BufferedUntilRead);

    km.handle_keydown(KEY_A, matrix);
    km.handle_keyup(KEY_A, matrix, true, 0);

    // Key should still be pressed (pending release)
    ASSERT_TRUE(is_pressed(matrix, KEY_A));

    // Simulate PPI scanning the line where KEY_A lives
    int line = scancode_line(KEY_A);  // line 8
    km.notify_scanned(line);

    // Now update() should process the pending release
    km.update(matrix, 0);
    EXPECT_TRUE(is_released(matrix, KEY_A));
}

TEST_F(KeyboardManagerTest, Buffered_NotifyScannedOnlyAffectsCorrectLine) {
    setMode(KeyboardSupportMode::BufferedUntilRead);

    // KEY_A is on line 8, KEY_B is on line 6
    km.handle_keydown(KEY_A, matrix);
    km.handle_keydown(KEY_B, matrix);
    km.handle_keyup(KEY_A, matrix, false, 0);
    km.handle_keyup(KEY_B, matrix, false, 0);

    ASSERT_TRUE(is_pressed(matrix, KEY_A));
    ASSERT_TRUE(is_pressed(matrix, KEY_B));

    // Scan line 8 (KEY_A's line), not line 6 (KEY_B's line)
    km.notify_scanned(scancode_line(KEY_A));
    km.update(matrix, 0);

    // KEY_A should now be released, KEY_B still pressed
    EXPECT_TRUE(is_released(matrix, KEY_A));
    EXPECT_TRUE(is_pressed(matrix, KEY_B));

    // Now scan KEY_B's line
    km.notify_scanned(scancode_line(KEY_B));
    km.update(matrix, 0);

    EXPECT_TRUE(is_released(matrix, KEY_B));
}

TEST_F(KeyboardManagerTest, Buffered_UpdateWithoutScanDoesNotRelease) {
    setMode(KeyboardSupportMode::BufferedUntilRead);

    km.handle_keydown(KEY_A, matrix);
    km.handle_keyup(KEY_A, matrix, true, 0);

    // Call update without scanning -- should NOT release
    km.update(matrix, 0);
    EXPECT_TRUE(is_pressed(matrix, KEY_A));

    km.update(matrix, 100);
    EXPECT_TRUE(is_pressed(matrix, KEY_A));  // Still pressed regardless of frame count
}

TEST_F(KeyboardManagerTest, Buffered_KeyupAfterScanReleasesImmediately) {
    setMode(KeyboardSupportMode::BufferedUntilRead);

    // Press a key, scan its line, then release -- the key_needs_scan flag is cleared
    km.handle_keydown(KEY_A, matrix);
    km.notify_scanned(scancode_line(KEY_A));

    // Now key_needs_scan[KEY_A] == false, so keyup should release immediately
    km.handle_keyup(KEY_A, matrix, true, 0);
    EXPECT_TRUE(is_released(matrix, KEY_A));
}

// ============================================================
// Min2Frames mode tests
// ============================================================

TEST_F(KeyboardManagerTest, Min2Frames_KeydownSetsMatrixBit) {
    setMode(KeyboardSupportMode::Min2Frames);

    km.handle_keydown(KEY_A, matrix);
    EXPECT_TRUE(is_pressed(matrix, KEY_A));
}

TEST_F(KeyboardManagerTest, Min2Frames_KeyupDefersRelease) {
    setMode(KeyboardSupportMode::Min2Frames);
    dword frame = 10;

    km.handle_keydown(KEY_A, matrix);
    km.handle_keyup(KEY_A, matrix, true, frame);

    // Key should still be pressed immediately after keyup
    EXPECT_TRUE(is_pressed(matrix, KEY_A));
}

TEST_F(KeyboardManagerTest, Min2Frames_ReleaseAfterTwoFrames) {
    setMode(KeyboardSupportMode::Min2Frames);
    dword frame = 10;

    km.handle_keydown(KEY_A, matrix);
    km.handle_keyup(KEY_A, matrix, true, frame);

    // Frame 11: not yet (need frame >= 12)
    km.update(matrix, frame + 1);
    EXPECT_TRUE(is_pressed(matrix, KEY_A));

    // Frame 12: should release (10 + 2 = 12)
    km.update(matrix, frame + 2);
    EXPECT_TRUE(is_released(matrix, KEY_A));
}

TEST_F(KeyboardManagerTest, Min2Frames_ReleaseExactlyAtTargetFrame) {
    setMode(KeyboardSupportMode::Min2Frames);
    dword frame = 0;

    km.handle_keydown(KEY_A, matrix);
    km.handle_keyup(KEY_A, matrix, true, frame);

    // release_frame = 0 + 2 = 2
    km.update(matrix, 1);
    EXPECT_TRUE(is_pressed(matrix, KEY_A));

    km.update(matrix, 2);
    EXPECT_TRUE(is_released(matrix, KEY_A));
}

TEST_F(KeyboardManagerTest, Min2Frames_LateUpdateStillReleases) {
    setMode(KeyboardSupportMode::Min2Frames);
    dword frame = 5;

    km.handle_keydown(KEY_A, matrix);
    km.handle_keyup(KEY_A, matrix, true, frame);

    // Skip ahead well past the target frame
    km.update(matrix, 100);
    EXPECT_TRUE(is_released(matrix, KEY_A));
}

// ============================================================
// Cancel pending release by re-pressing
// ============================================================

TEST_F(KeyboardManagerTest, Buffered_RepressCancelsPendingRelease) {
    setMode(KeyboardSupportMode::BufferedUntilRead);

    km.handle_keydown(KEY_A, matrix);
    km.handle_keyup(KEY_A, matrix, true, 0);

    // KEY_A is still pressed, release is pending
    ASSERT_TRUE(is_pressed(matrix, KEY_A));

    // Press the same key again -- this should cancel the pending release
    km.handle_keydown(KEY_A, matrix);

    // Now scan and update -- the cancelled release should not release the key
    km.notify_scanned(scancode_line(KEY_A));
    km.update(matrix, 0);

    // Key should still be pressed because the pending release was cancelled
    // and a new keydown was registered (which re-added key_needs_scan)
    EXPECT_TRUE(is_pressed(matrix, KEY_A));
}

TEST_F(KeyboardManagerTest, Min2Frames_RepressCancelsPendingRelease) {
    setMode(KeyboardSupportMode::Min2Frames);
    dword frame = 10;

    km.handle_keydown(KEY_A, matrix);
    km.handle_keyup(KEY_A, matrix, true, frame);

    // Press again before the 2-frame window expires
    km.handle_keydown(KEY_A, matrix);

    // Advance past the original release frame
    km.update(matrix, frame + 5);

    // Key should still be pressed -- the re-press cancelled the pending release
    EXPECT_TRUE(is_pressed(matrix, KEY_A));
}

// ============================================================
// Multiple keys pressed and released in sequence
// ============================================================

TEST_F(KeyboardManagerTest, Min2Frames_MultipleKeysReleasedInSequence) {
    setMode(KeyboardSupportMode::Min2Frames);

    // Press A at frame 0
    km.handle_keydown(KEY_A, matrix);
    // Press B at frame 1
    km.handle_keydown(KEY_B, matrix);

    // Release A at frame 2
    km.handle_keyup(KEY_A, matrix, false, 2);
    // Release B at frame 3
    km.handle_keyup(KEY_B, matrix, false, 3);

    // At frame 3: A's release_frame = 4, B's release_frame = 5
    km.update(matrix, 3);
    EXPECT_TRUE(is_pressed(matrix, KEY_A));
    EXPECT_TRUE(is_pressed(matrix, KEY_B));

    // At frame 4: A should release, B still pending
    km.update(matrix, 4);
    EXPECT_TRUE(is_released(matrix, KEY_A));
    EXPECT_TRUE(is_pressed(matrix, KEY_B));

    // At frame 5: B should release
    km.update(matrix, 5);
    EXPECT_TRUE(is_released(matrix, KEY_B));
}

TEST_F(KeyboardManagerTest, Buffered_MultipleKeysOnDifferentLines) {
    setMode(KeyboardSupportMode::BufferedUntilRead);

    // KEY_A (line 8), KEY_SPACE (line 5), KEY_RETURN (line 2)
    km.handle_keydown(KEY_A, matrix);
    km.handle_keydown(KEY_SPACE, matrix);
    km.handle_keydown(KEY_RETURN, matrix);

    EXPECT_TRUE(is_pressed(matrix, KEY_A));
    EXPECT_TRUE(is_pressed(matrix, KEY_SPACE));
    EXPECT_TRUE(is_pressed(matrix, KEY_RETURN));

    // Release all
    km.handle_keyup(KEY_A, matrix, false, 0);
    km.handle_keyup(KEY_SPACE, matrix, false, 0);
    km.handle_keyup(KEY_RETURN, matrix, false, 0);

    // All still pressed (pending scan)
    EXPECT_TRUE(is_pressed(matrix, KEY_A));
    EXPECT_TRUE(is_pressed(matrix, KEY_SPACE));
    EXPECT_TRUE(is_pressed(matrix, KEY_RETURN));

    // Scan line 5 only (SPACE's line)
    km.notify_scanned(scancode_line(KEY_SPACE));
    km.update(matrix, 0);

    EXPECT_TRUE(is_pressed(matrix, KEY_A));
    EXPECT_TRUE(is_released(matrix, KEY_SPACE));
    EXPECT_TRUE(is_pressed(matrix, KEY_RETURN));

    // Scan lines 8 and 2
    km.notify_scanned(scancode_line(KEY_A));
    km.notify_scanned(scancode_line(KEY_RETURN));
    km.update(matrix, 0);

    EXPECT_TRUE(is_released(matrix, KEY_A));
    EXPECT_TRUE(is_released(matrix, KEY_RETURN));
}

TEST_F(KeyboardManagerTest, Buffered_TwoKeysOnSameLine) {
    setMode(KeyboardSupportMode::BufferedUntilRead);

    // KEY_A = 0x85 (line 8, bit 5) and KEY_S = 0x74 (line 7, bit 4)
    // We need two keys on the same line. Let's construct scancode values:
    // KEY_ENTER = 0x06 (line 0, bit 6)
    // CPC_0 = 0x40 (line 4, bit 0), CPC_9 = 0x41 (line 4, bit 1)
    // Actually, let's use two keys that happen to be on the same line.
    // CPC_8 = 0x50 (line 5, bit 0), CPC_SPACE = 0x57 (line 5, bit 7)
    // Both are on line 5!

    CPCScancode KEY_8 = 0x50;  // line 5, bit 0

    km.handle_keydown(KEY_8, matrix);
    km.handle_keydown(KEY_SPACE, matrix);

    EXPECT_TRUE(is_pressed(matrix, KEY_8));
    EXPECT_TRUE(is_pressed(matrix, KEY_SPACE));

    km.handle_keyup(KEY_8, matrix, false, 0);
    km.handle_keyup(KEY_SPACE, matrix, false, 0);

    // Both still pressed
    EXPECT_TRUE(is_pressed(matrix, KEY_8));
    EXPECT_TRUE(is_pressed(matrix, KEY_SPACE));

    // Scan line 5 -- both keys' needs_scan should be cleared
    km.notify_scanned(5);
    km.update(matrix, 0);

    // Both should be released
    EXPECT_TRUE(is_released(matrix, KEY_8));
    EXPECT_TRUE(is_released(matrix, KEY_SPACE));
}

// ============================================================
// update() processes pending releases correctly per mode
// ============================================================

TEST_F(KeyboardManagerTest, Update_NoPendingReleasesIsNoop) {
    setMode(KeyboardSupportMode::Direct);

    // Matrix should remain all 0xFF
    byte expected[16];
    for (int i = 0; i < 16; i++) expected[i] = 0xFF;

    km.update(matrix, 0);
    for (int i = 0; i < 16; i++) {
        EXPECT_EQ(matrix[i], expected[i]) << "Line " << i << " changed unexpectedly";
    }
}

TEST_F(KeyboardManagerTest, Update_DirectModeReleasesPendingImmediately) {
    // If somehow a pending release ends up in the queue while in Direct mode,
    // update() should release it immediately (the fallback `else` branch).
    // This is an edge case -- Direct mode normally releases in handle_keyup.
    // We can test by switching modes after queueing.

    setMode(KeyboardSupportMode::Min2Frames);
    km.handle_keydown(KEY_A, matrix);
    km.handle_keyup(KEY_A, matrix, true, 10);  // queues for frame 12

    // Switch to Direct mode
    setMode(KeyboardSupportMode::Direct);

    // update() in Direct mode should release immediately regardless of frame
    km.update(matrix, 0);
    EXPECT_TRUE(is_released(matrix, KEY_A));
}

// ============================================================
// key_needs_scan map doesn't grow unbounded
// ============================================================

TEST_F(KeyboardManagerTest, Buffered_KeyNeedsScanErasedOnDirectRelease) {
    setMode(KeyboardSupportMode::BufferedUntilRead);

    // Press, scan line, then release -- should clean up
    km.handle_keydown(KEY_A, matrix);
    km.notify_scanned(scancode_line(KEY_A));

    // key_needs_scan[KEY_A] is now false, so keyup releases immediately
    // and erases the entry
    km.handle_keyup(KEY_A, matrix, true, 0);
    EXPECT_TRUE(is_released(matrix, KEY_A));

    // Press and release again to prove the map was cleaned up properly
    // (if the old entry was still there with value=false, a new keydown
    // would set it to true again, and the cycle works)
    km.handle_keydown(KEY_A, matrix);
    EXPECT_TRUE(is_pressed(matrix, KEY_A));
    km.handle_keyup(KEY_A, matrix, true, 0);
    // Not yet scanned, so still pressed
    EXPECT_TRUE(is_pressed(matrix, KEY_A));
}

TEST_F(KeyboardManagerTest, Buffered_KeyNeedsScanErasedAfterUpdate) {
    setMode(KeyboardSupportMode::BufferedUntilRead);

    km.handle_keydown(KEY_A, matrix);
    km.handle_keyup(KEY_A, matrix, true, 0);

    // Scan and update to release
    km.notify_scanned(scancode_line(KEY_A));
    km.update(matrix, 0);
    EXPECT_TRUE(is_released(matrix, KEY_A));

    // Now press/release the same key again -- if the map entry wasn't cleaned,
    // the stale false value would cause immediate release instead of buffering
    km.handle_keydown(KEY_A, matrix);
    EXPECT_TRUE(is_pressed(matrix, KEY_A));
    km.handle_keyup(KEY_A, matrix, true, 0);

    // Should be buffered (still pressed) because handle_keydown set needs_scan=true
    EXPECT_TRUE(is_pressed(matrix, KEY_A));

    // Clean up
    km.notify_scanned(scancode_line(KEY_A));
    km.update(matrix, 0);
    EXPECT_TRUE(is_released(matrix, KEY_A));
}

TEST_F(KeyboardManagerTest, Buffered_ManyKeysPressReleaseCycle) {
    setMode(KeyboardSupportMode::BufferedUntilRead);

    // Press and fully release 5 different keys through scan cycle
    CPCScancode keys[] = {KEY_A, KEY_B, KEY_S, KEY_SPACE, KEY_RETURN};
    for (auto k : keys) {
        km.handle_keydown(k, matrix);
    }
    for (auto k : keys) {
        km.handle_keyup(k, matrix, false, 0);
    }

    // Scan all lines
    for (int line = 0; line < 16; line++) {
        km.notify_scanned(line);
    }
    km.update(matrix, 0);

    // All keys released
    for (auto k : keys) {
        EXPECT_TRUE(is_released(matrix, k));
    }

    // Do it again -- should work identically (no stale map entries)
    for (auto k : keys) {
        km.handle_keydown(k, matrix);
    }
    for (auto k : keys) {
        EXPECT_TRUE(is_pressed(matrix, k));
    }
    for (auto k : keys) {
        km.handle_keyup(k, matrix, false, 0);
    }
    for (int line = 0; line < 16; line++) {
        km.notify_scanned(line);
    }
    km.update(matrix, 0);
    for (auto k : keys) {
        EXPECT_TRUE(is_released(matrix, k));
    }
}

// ============================================================
// Edge cases
// ============================================================

TEST_F(KeyboardManagerTest, InvalidScancode0xFF_IsIgnored) {
    setMode(KeyboardSupportMode::Direct);

    // 0xFF is the "no key" sentinel
    CPCScancode no_key = 0xFF;
    km.handle_keydown(no_key, matrix);

    // Matrix should remain unchanged
    for (int i = 0; i < 16; i++) {
        EXPECT_EQ(matrix[i], 0xFF) << "Line " << i << " was modified by 0xFF scancode";
    }
}

TEST_F(KeyboardManagerTest, InvalidScancode0xFF_KeyupIgnored) {
    setMode(KeyboardSupportMode::BufferedUntilRead);

    CPCScancode no_key = 0xFF;
    km.handle_keyup(no_key, matrix, true, 0);

    // No pending releases should be created
    km.update(matrix, 100);
    for (int i = 0; i < 16; i++) {
        EXPECT_EQ(matrix[i], 0xFF);
    }
}

TEST_F(KeyboardManagerTest, Min2Frames_FrameCountWraparound) {
    setMode(KeyboardSupportMode::Min2Frames);

    // Test with frame count near max -- wrap-around behavior
    dword near_max = 0xFFFFFFFE;
    km.handle_keydown(KEY_A, matrix);
    km.handle_keyup(KEY_A, matrix, true, near_max);

    // release_frame = 0xFFFFFFFE + 2 = 0 (wraps around)
    // At frame 0xFFFFFFFF, 0xFFFFFFFF < 0 is false (unsigned), so...
    // Actually with unsigned arithmetic: release_frame = 0x00000000
    // At frame 0xFFFFFFFF: current_frame (0xFFFFFFFF) >= release_frame (0)
    // This is a known edge case -- the >= comparison means it releases.
    // The practical impact is negligible (happens once every ~2 years at 50fps)
    km.update(matrix, 0xFFFFFFFF);
    // With unsigned wrapping, 0xFFFFFFFF >= 0x00000000 is true
    EXPECT_TRUE(is_released(matrix, KEY_A));
}

TEST_F(KeyboardManagerTest, Direct_ReleaseModifiersFalse) {
    setMode(KeyboardSupportMode::Direct);

    // When release_modifiers is false, SHIFT/CTRL should not be explicitly released
    // Press a key (applyKeypressDirect will set SHIFT/CTRL state based on scancode)
    km.handle_keydown(KEY_A, matrix);

    // Release with release_modifiers=false
    km.handle_keyup(KEY_A, matrix, false, 0);

    // Key A itself should be released
    EXPECT_TRUE(is_released(matrix, KEY_A));
}

TEST_F(KeyboardManagerTest, Buffered_NotifyScanAllLinesReleasesKey) {
    setMode(KeyboardSupportMode::BufferedUntilRead);

    // Press and release a key, then scan all 16 valid lines
    km.handle_keydown(KEY_A, matrix);
    km.handle_keyup(KEY_A, matrix, true, 0);

    for (int i = 0; i < 16; i++) {
        km.notify_scanned(i);
    }
    km.update(matrix, 0);
    EXPECT_TRUE(is_released(matrix, KEY_A));
}

TEST_F(KeyboardManagerTest, Min2Frames_MultipleUpdatesIdempotent) {
    setMode(KeyboardSupportMode::Min2Frames);

    km.handle_keydown(KEY_A, matrix);
    km.handle_keyup(KEY_A, matrix, true, 0);

    // Release at frame 2
    km.update(matrix, 2);
    EXPECT_TRUE(is_released(matrix, KEY_A));

    // Calling update again should be safe (no double-release)
    km.update(matrix, 3);
    EXPECT_TRUE(is_released(matrix, KEY_A));

    // Matrix should still be all released
    EXPECT_EQ(matrix[scancode_line(KEY_A)], 0xFF);
}

TEST_F(KeyboardManagerTest, Buffered_ScanBeforeKeyup) {
    setMode(KeyboardSupportMode::BufferedUntilRead);

    km.handle_keydown(KEY_A, matrix);
    ASSERT_TRUE(is_pressed(matrix, KEY_A));

    // Scan the line BEFORE keyup happens
    km.notify_scanned(scancode_line(KEY_A));

    // Now keyup -- key_needs_scan should be false, so release immediately
    km.handle_keyup(KEY_A, matrix, true, 0);
    EXPECT_TRUE(is_released(matrix, KEY_A));
}

TEST_F(KeyboardManagerTest, Buffered_RapidPressReleasePressRelease) {
    setMode(KeyboardSupportMode::BufferedUntilRead);

    // First press-release cycle
    km.handle_keydown(KEY_A, matrix);
    km.handle_keyup(KEY_A, matrix, true, 0);

    // Second press cancels the pending release
    km.handle_keydown(KEY_A, matrix);
    EXPECT_TRUE(is_pressed(matrix, KEY_A));

    // Second release
    km.handle_keyup(KEY_A, matrix, true, 0);

    // Scan and update
    km.notify_scanned(scancode_line(KEY_A));
    km.update(matrix, 0);

    // Should be released after scan
    EXPECT_TRUE(is_released(matrix, KEY_A));
}

TEST_F(KeyboardManagerTest, Min2Frames_RapidPressReleasePressRelease) {
    setMode(KeyboardSupportMode::Min2Frames);

    // First press at frame 0, release at frame 1 (target: frame 3)
    km.handle_keydown(KEY_A, matrix);
    km.handle_keyup(KEY_A, matrix, true, 1);

    // Second press at frame 2 cancels the pending release
    km.handle_keydown(KEY_A, matrix);
    EXPECT_TRUE(is_pressed(matrix, KEY_A));

    // Frame 3: the cancelled release should not fire
    km.update(matrix, 3);
    EXPECT_TRUE(is_pressed(matrix, KEY_A));

    // Second release at frame 4 (target: frame 6)
    km.handle_keyup(KEY_A, matrix, true, 4);

    km.update(matrix, 5);
    EXPECT_TRUE(is_pressed(matrix, KEY_A));

    km.update(matrix, 6);
    EXPECT_TRUE(is_released(matrix, KEY_A));
}

// ============================================================
// Interaction: keydown does not affect other keys' matrix bits
// ============================================================

TEST_F(KeyboardManagerTest, KeydownOnlyAffectsTargetBit) {
    setMode(KeyboardSupportMode::Direct);

    // Save the initial state of all lines
    byte initial[16];
    for (int i = 0; i < 16; i++) initial[i] = 0xFF;

    km.handle_keydown(KEY_A, matrix);  // line 8, bit 5

    // Check that ONLY line 8 bit 5 changed (and possibly line 2 for SHIFT/CTRL)
    // applyKeypressDirect for a non-modified key releases SHIFT (line 2, bit 5)
    // and releases CTRL (line 2, bit 7). Since they start at 0xFF (released),
    // the "|=" for unshift/uncontrol is a no-op.
    int key_line = scancode_line(KEY_A);
    for (int i = 0; i < 16; i++) {
        if (i == key_line) {
            // This line should have bit 5 cleared
            EXPECT_EQ(matrix[i], static_cast<byte>(0xFF & ~bit_values[scancode_bit(KEY_A)]));
        } else {
            EXPECT_EQ(matrix[i], 0xFF) << "Line " << i << " was unexpectedly modified";
        }
    }
}
