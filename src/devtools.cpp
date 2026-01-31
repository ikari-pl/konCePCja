#include "devtools.h"

#include <string>
#include "cap32.h"
#include "log.h"
#include "video.h"
#include "wg_error.h"

extern t_CPC CPC;
extern SDL_Window* mainSDLWindow;
extern SDL_Surface* back_surface;

static int g_dbg_click_x = -1;
static int g_dbg_click_y = -1;

bool DevTools::Activate(int scale, bool useMainWindow) {
  this->scale = scale;
  this->useMainWindow = useMainWindow;
  ShowCursor(true);
  if (useMainWindow) {
    CPC.scr_gui_is_currently_on = true;
  }
  try {
    if (useMainWindow) {
      window = mainSDLWindow;
      renderer = nullptr;
      texture = nullptr;
      surface = SDL_CreateSurface(DEVTOOLS_WIDTH, DEVTOOLS_HEIGHT, SDL_PIXELFORMAT_RGBA32);
      if (!surface) { Deactivate(); return false; }
      capriceGui = std::make_unique<CapriceGui>(window, /*bInMainView=*/false, /*scale=*/1);
      capriceGui->Init();
      devToolsView = std::make_unique<CapriceDevToolsView>(*capriceGui, surface, renderer, texture, wGui::CRect(0, 0, DEVTOOLS_WIDTH, DEVTOOLS_HEIGHT), this);
      video_set_devtools_panel(surface, DEVTOOLS_WIDTH, DEVTOOLS_HEIGHT, 1);
    } else {
      // TODO: This position only makes sense for me. Ideally we would probably want to find where current window is, find display size and place
      // the window where there's the most space available. On the other hand, getting display size is not very reliable on multi-screen setups under linux ...
      window = SDL_CreateWindow("Caprice32 - Developers' tools", DEVTOOLS_WIDTH*scale, DEVTOOLS_HEIGHT*scale, 0);
      renderer = SDL_CreateRenderer(window, nullptr);
      if (!window || !renderer) { Deactivate(); return false; }
      surface = SDL_CreateSurface(DEVTOOLS_WIDTH, DEVTOOLS_HEIGHT, SDL_PIXELFORMAT_RGBA32);
      if (!surface) { Deactivate(); return false; }
      texture = SDL_CreateTextureFromSurface(renderer, surface);
      if (!texture) { Deactivate(); return false; }
      {
        const SDL_PixelFormatDetails* fmt = SDL_GetPixelFormatDetails(surface->format);
        SDL_Palette* pal = SDL_GetSurfacePalette(surface);
        SDL_FillSurfaceRect(surface, nullptr, SDL_MapRGB(fmt, pal, 0, 0, 0));
      }
      capriceGui = std::make_unique<CapriceGui>(window, /*bInMainView=*/false, scale);
      capriceGui->Init();
      devToolsView = std::make_unique<CapriceDevToolsView>(*capriceGui, surface, renderer, texture, wGui::CRect(0, 0, DEVTOOLS_WIDTH, DEVTOOLS_HEIGHT), this);
    }
  } catch(wGui::Wg_Ex_App& e) {
      // TODO: improve: this is pretty silent if people don't look at the console
      LOG_ERROR("Failed displaying developer's tools: " << e.what());
      Deactivate();
      return false;
  }
  active = true;
  return true;
}

void DevTools::Deactivate() {
  ShowCursor(false);
  devToolsView = nullptr;
  capriceGui = nullptr;
  if (!useMainWindow) {
    if (texture) SDL_DestroyTexture(texture);
    if (surface) SDL_DestroySurface(surface);
    if (renderer) SDL_DestroyRenderer(renderer);
    if (window) SDL_DestroyWindow(window);
  } else {
    video_clear_devtools_panel();
    if (surface) SDL_DestroySurface(surface);
  }
  texture = nullptr;
  surface = nullptr;
  renderer = nullptr;
  window = nullptr;
  if (useMainWindow) {
    CPC.scr_gui_is_currently_on = false;
  }
  useMainWindow = false;
  active = false;
}

void DevTools::LoadSymbols(const std::string& filename) {
  devToolsView->LoadSymbols(filename);
}

void DevTools::PreUpdate() {
  devToolsView->PreUpdate();
}

void DevTools::PostUpdate() {
  devToolsView->PostUpdate();
  capriceGui->Update();
}

bool DevTools::PassEvent(SDL_Event& event) {
  return capriceGui->ProcessEvent(event);
}

void devtools_set_debug_click(int x, int y) {
  g_dbg_click_x = x;
  g_dbg_click_y = y;
}

bool devtools_get_debug_click(int& x, int& y) {
  if (g_dbg_click_x < 0 || g_dbg_click_y < 0) return false;
  x = g_dbg_click_x;
  y = g_dbg_click_y;
  return true;
}
