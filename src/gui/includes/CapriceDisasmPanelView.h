// Disassembly panel view for Caprice32
#ifndef _WG_CAPRICEDISASMPANELVIEW_H
#define _WG_CAPRICEDISASMPANELVIEW_H

#include <string>
#include "wg_point.h"
#include "wg_rect.h"
#include "wg_view.h"
#include "CapriceDevTools.h"

class DevTools;

class CapriceDisasmPanelView : public wGui::CView
{
  protected:
    wGui::CapriceDisasmPanel* m_pDisasmPanel;

  public:
    CapriceDisasmPanelView(wGui::CApplication& application, SDL_Surface* surface, const wGui::CRect& WindowRect, DevTools* devtools);
    ~CapriceDisasmPanelView() final = default;

    void LoadSymbols(const std::string& filename);

    void PaintToSurface(SDL_Surface& ScreenSurface, SDL_Surface& FloatingSurface, const wGui::CPoint& Offset) const override;

    void PreUpdate();
    void PostUpdate();
};

#endif
