// Headless ImGui for render tests — no platform or renderer backend at all.
//
// Shared by every test that draws konCePCja UI without a window. Keep the
// vertex-count discipline: a collapsed or clipped window silently draws
// nothing, so "it did not crash" proves nothing — assert TotalVtxCount.

#pragma once

#include <functional>

#include "imgui.h"

namespace koncpc_test {

// A headless ImGui context — no platform or renderer backend at all.
//
// ImGui needs a backend for input and for uploading the font texture, neither
// of which a test cares about. Because we deliberately do NOT set
// ImGuiBackendFlags_RendererHasTextures, ImGui takes its legacy path, so all
// the logic that actually breaks — layout, clipping, ID hashing, label
// handling, draw-list emission — runs exactly as it does in the shipping app.
class HeadlessImGui {
 public:
  HeadlessImGui() {
    ctx_ = ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(1600.0f, 1000.0f);
    io.DeltaTime = 1.0f / 60.0f;
    // A test must never read or clobber the developer's real imgui.ini.
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.Fonts->AddFontDefault();

    // Stand in for the renderer backend: on the legacy path ImGui asserts that
    // the atlas was rasterised by the backend (imgui_draw.cpp:2772).
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

  // Runs one frame and returns how many vertices it emitted.
  //
  // The vertex count is what keeps these tests honest. An ImGui window that is
  // collapsed, clipped or off-screen silently draws nothing — Selectable() and
  // friends early-return on window->SkipItems before touching their arguments
  // (imgui_widgets.cpp:7308). So "it did not crash" would also pass when no
  // widget ran at all. Asserting vertices were produced proves the code under
  // test actually executed.
  int frame(const std::function<void()>& body) {
    ImGui::NewFrame();
    body();
    ImGui::Render();
    return ImGui::GetDrawData()->TotalVtxCount;
  }

  // ImGui needs a couple of frames to settle a newly created window
  // (appearing, auto-fit, table column widths). Returns the last frame's count.
  int settled_frames(const std::function<void()>& body, int n = 3) {
    int vtx = 0;
    for (int i = 0; i < n; i++) vtx = frame(body);
    return vtx;
  }

 private:
  ImGuiContext* ctx_ = nullptr;
};

}  // namespace koncpc_test
