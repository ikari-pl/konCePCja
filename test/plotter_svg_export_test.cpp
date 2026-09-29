/* plotter_export_svg_segments() error reporting (beads-5os).
 *
 * The SVG writer is the live export path behind IPC 'plotter export' and the
 * UI's Save SVG button. It used to return true whenever fopen() succeeded,
 * so a full disk left a truncated file behind an "exported SVG" message.
 */

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "file_size_limit.h"
#include "plotter.h"

namespace {

std::vector<PlotSegment> some_lines(int n) {
  std::vector<PlotSegment> segs;
  for (int i = 0; i < n; ++i) {
    PlotSegment s;
    s.x1 = static_cast<float>(i);
    s.y1 = 0;
    s.x2 = static_cast<float>(i);
    s.y2 = 1000;
    s.pen = 1 + (i % 2);
    segs.push_back(s);
  }
  return segs;
}

class PlotterSvgExportTest : public testing::Test {
 protected:
  void TearDown() override {
    std::error_code ec;
    std::filesystem::remove(path_, ec);
  }
  std::filesystem::path path_ =
      std::filesystem::temp_directory_path() / "koncepcja_plotter_export.svg";
};

}  // namespace

TEST_F(PlotterSvgExportTest, WritesCompleteDocument) {
  ASSERT_TRUE(
      plotter_export_svg_segments(some_lines(10), 0.269f, path_.string()));
  std::ifstream in(path_);
  std::stringstream ss;
  ss << in.rdbuf();
  EXPECT_NE(ss.str().find("</svg>"), std::string::npos);
}

TEST_F(PlotterSvgExportTest, UnopenablePathFails) {
  // A directory cannot be opened for writing.
  EXPECT_FALSE(plotter_export_svg_segments(
      some_lines(1), 0.269f, std::filesystem::temp_directory_path().string()));
}

TEST_F(PlotterSvgExportTest, WriteErrorAfterOpenFails) {
  if (!ScopedFileSizeLimit::supported()) {
    GTEST_SKIP() << "no per-process file size limit on this platform";
  }
  bool ok = true;
  {
    // Opens fine, then runs out of room a few hundred bytes in.
    ScopedFileSizeLimit const limit(256);
    ASSERT_TRUE(limit.active());
    ok = plotter_export_svg_segments(some_lines(200), 0.269f, path_.string());
  }
  EXPECT_FALSE(ok) << "a truncated SVG was reported as exported";
}
