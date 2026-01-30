#ifndef DEVTOOLS_H
#define DEVTOOLS_H

#include <string>
#include "SDL.h"
#include "CapriceGui.h"
#include "CapriceDevToolsView.h"
#include "CapriceRegsPanelView.h"
#include "CapriceDisasmPanelView.h"

class DevTools {
  public:
    bool Activate(int scale, bool useMainWindow);
    void Deactivate();

    bool IsActive() const { return active; };
    bool UsesMainWindow() const { return useMainWindow; };

    void LoadSymbols(const std::string& filename);

    void PreUpdate();
    void PostUpdate();

    // Return true if the event was processed
    // (i.e destined to this window)
    bool PassEvent(SDL_Event& e);

  private:
    std::unique_ptr<CapriceGui> capriceGui;
    std::unique_ptr<CapriceDevToolsView> devToolsView;
    std::unique_ptr<wGui::CapriceRegsPanelView> regsPanelView;
    bool active = false;
    bool useMainWindow = false;
    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    SDL_Texture* texture = nullptr;
    SDL_Surface* surface = nullptr;
    int scale = 0;
};

void devtools_set_debug_click(int x, int y);
bool devtools_get_debug_click(int& x, int& y);

#endif
