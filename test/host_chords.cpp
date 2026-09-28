#include "host_chords.h"

#include <gtest/gtest.h>

// Cmd chord = host UI, unmodified key = the CPC. One resolver serves the event
// loop, the menus' accelerator labels and the native key equivalents, so these
// pin the mapping every surface shows.

TEST(HostChords, CommandOnMacControlElsewhere) {
  // macOS: Cmd+O, and Ctrl+O is left to the CPC (Control is a CPC key).
  EXPECT_EQ(HostChord::OpenDisk, host_chord_for('o', false, true, true));
  EXPECT_EQ(HostChord::None, host_chord_for('o', true, false, true));
  // Elsewhere: Ctrl+O, and a stray GUI modifier does nothing.
  EXPECT_EQ(HostChord::OpenDisk, host_chord_for('o', true, false, false));
  EXPECT_EQ(HostChord::None, host_chord_for('o', false, true, false));
}

TEST(HostChords, TheThreeChords) {
  EXPECT_EQ(HostChord::CommandPalette, host_chord_for('k', true, false, false));
  EXPECT_EQ(HostChord::OpenDisk, host_chord_for('o', true, false, false));
  EXPECT_EQ(HostChord::SaveSnapshot, host_chord_for('s', true, false, false));
  // Uppercase keycodes resolve too (Shift held, or a platform reporting 'O').
  EXPECT_EQ(HostChord::OpenDisk, host_chord_for('O', true, false, false));
}

TEST(HostChords, AnUnmodifiedKeyIsNeverAChord) {
  EXPECT_EQ(HostChord::None, host_chord_for('o', false, false, true));
  EXPECT_EQ(HostChord::None, host_chord_for('s', false, false, false));
  EXPECT_EQ(HostChord::None, host_chord_for('k', false, false, false));
}

TEST(HostChords, OtherLettersAreNotChords) {
  for (int k = 'a'; k <= 'z'; ++k) {
    if (k == 'k' || k == 'o' || k == 's') continue;
    EXPECT_EQ(HostChord::None, host_chord_for(k, true, true, false)) << char(k);
  }
}

TEST(HostChords, LabelsNameThePlatformModifier) {
  EXPECT_EQ("Cmd+O", host_chord_label(HostChord::OpenDisk, true));
  EXPECT_EQ("Ctrl+O", host_chord_label(HostChord::OpenDisk, false));
  EXPECT_EQ("Cmd+S", host_chord_label(HostChord::SaveSnapshot, true));
  EXPECT_EQ("Ctrl+K", host_chord_label(HostChord::CommandPalette, false));
  EXPECT_EQ("", host_chord_label(HostChord::None, true));
}

TEST(HostChords, KeyEquivalentsMatchTheResolver) {
  // The native menu builds its key equivalent from host_chord_key(); the
  // event loop resolves that same key back to the same chord.
  for (HostChord c : {HostChord::CommandPalette, HostChord::OpenDisk,
                      HostChord::SaveSnapshot}) {
    char const key = host_chord_key(c);
    ASSERT_NE(0, key);
    EXPECT_EQ(c, host_chord_for(key, false, true, true));
  }
  EXPECT_EQ(0, host_chord_key(HostChord::None));
}
