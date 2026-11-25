/* koncpc_set_renderer must ignore out-of-range plugin indices (beads-op21):
 * CPC.scr_style is unsigned, so a negative index would wrap to a huge value
 * and the plugin flip would index out of bounds.
 */

#include <gtest/gtest.h>

#include <vector>

#include "imgui_state.h"
#include "koncepcja.h"
#include "menu_bridge.h"
#include "video_plugin.h"

extern t_CPC CPC;
extern std::vector<video_plugin> video_plugin_list;

namespace {

class RendererBoundsTest : public ::testing::Test {
 protected:
  void SetUp() override {
    saved_list_ = video_plugin_list;
    saved_style_ = CPC.scr_style;
    saved_pending_ = imgui_state.video_reinit_pending;
    video_plugin_list.assign(2, video_plugin{});
    CPC.scr_style = 7;
    imgui_state.video_reinit_pending = false;
  }
  void TearDown() override {
    video_plugin_list = saved_list_;
    CPC.scr_style = saved_style_;
    imgui_state.video_reinit_pending = saved_pending_;
  }

  std::vector<video_plugin> saved_list_;
  unsigned int saved_style_;
  bool saved_pending_;
};

TEST_F(RendererBoundsTest, OutOfRangeIndicesAreIgnored) {
  koncpc_set_renderer(-1);
  EXPECT_EQ(CPC.scr_style, 7u);
  EXPECT_FALSE(imgui_state.video_reinit_pending);

  koncpc_set_renderer(2);  // one past the end
  EXPECT_EQ(CPC.scr_style, 7u);
  EXPECT_FALSE(imgui_state.video_reinit_pending);
}

TEST_F(RendererBoundsTest, ValidIndexIsApplied) {
  koncpc_set_renderer(1);
  EXPECT_EQ(CPC.scr_style, 1u);
  EXPECT_TRUE(imgui_state.video_reinit_pending);
}

}  // namespace
