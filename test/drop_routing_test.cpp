// konCePCja — drag-&-drop extension routing.
//
// The drop handler used to compare against a hand-kept extension list that
// drifted from what slotshandler's loaders actually accept: flux containers
// (.hfe/.scp/.a2r) loaded fine from the CLI and File menu but a *drop* hit
// the unknown-file toast. The fix routes drops through the one exported list
// (drive_extensions) via an exact-token matcher; these tests pin both halves.

#include <gtest/gtest.h>

#include "imgui_ui_testable.h"
#include "slotshandler.h"

// ── The exact-token matcher ──────────────────────────────────────────

TEST(ExtensionInDottedList, MatchesWholeTokensOnly) {
  const std::string list = ".dsk.ipf.raw.scp.hfe.a2r";
  EXPECT_TRUE(extension_in_dotted_list(list, ".dsk"));
  EXPECT_TRUE(extension_in_dotted_list(list, ".a2r"));  // end of list
  EXPECT_TRUE(extension_in_dotted_list(list, ".hfe"));
  // Substring-but-not-a-token must NOT match — the trap plain find() has.
  EXPECT_FALSE(extension_in_dotted_list(list, ".hf"));
  EXPECT_FALSE(extension_in_dotted_list(list, ".a"));
  EXPECT_FALSE(extension_in_dotted_list(list, ".ra"));
}

TEST(ExtensionInDottedList, RejectsDegenerateInput) {
  EXPECT_FALSE(extension_in_dotted_list(".dsk", ""));
  EXPECT_FALSE(extension_in_dotted_list(".dsk", "."));
  EXPECT_FALSE(extension_in_dotted_list(".dsk", "dsk"));  // no leading dot
  EXPECT_FALSE(extension_in_dotted_list("", ".dsk"));
}

// ── Drift guard: the routing consumes the REAL slot lists ────────────
//
// The drop handler calls extension_in_dotted_list(drive_extensions(DSK_A), e),
// so these tests pin the actual exported list — not a copy. If a format is
// added to (or removed from) slotshandler's drive-A set, this fails until the
// expectations here are updated deliberately.

TEST(DropRouting, DriveAAcceptsEveryDiskAndFluxFormat) {
  const std::string a = drive_extensions(DRIVE::DSK_A);
  for (const char* e : {".dsk", ".ipf", ".raw", ".scp", ".hfe", ".a2r"})
    EXPECT_TRUE(extension_in_dotted_list(a, e)) << e;
}

TEST(DropRouting, DriveARejectsNonDiskMedia) {
  const std::string a = drive_extensions(DRIVE::DSK_A);
  for (const char* e : {".cdt", ".voc", ".sna", ".cpr", ".zip", ".bin", ".hf"})
    EXPECT_FALSE(extension_in_dotted_list(a, e)) << e;
}

// Flux stays drive-A-only by FDC design (side-0/drive-A flux capture). The
// drive-B dialogs and docs rely on this holding.
TEST(DropRouting, DriveBExcludesFluxByDesign) {
  const std::string b = drive_extensions(DRIVE::DSK_B);
  for (const char* e : {".dsk", ".ipf", ".raw"})
    EXPECT_TRUE(extension_in_dotted_list(b, e)) << e;
  for (const char* e : {".scp", ".hfe", ".a2r"})
    EXPECT_FALSE(extension_in_dotted_list(b, e)) << e;
}
