// Disassembly panel view for Caprice32
#include "CapriceDisasmPanelView.h"
#include "CapriceDevTools.h"
#include <string>

using namespace wGui;

CapriceDisasmPanelView::CapriceDisasmPanelView(CApplication& application, SDL_Surface* surface, const CRect& WindowRect, DevTools* devtools) : CView(application, surface, nullptr, WindowRect)
{
  Application().MessageServer()->RegisterMessageClient(this, CMessage::CTRL_MESSAGEBOXRETURN);
  m_pDisasmPanel = new CapriceDisasmPanel(CRect(CPoint(0, 0), WindowRect.Width(), WindowRect.Height()), this, nullptr, devtools);
}

void CapriceDisasmPanelView::LoadSymbols(const std::string& filename)
{
  m_pDisasmPanel->LoadSymbols(filename);
}

void CapriceDisasmPanelView::PaintToSurface(SDL_Surface& ScreenSurface, SDL_Surface& FloatingSurface, const CPoint& Offset) const
{
  if (m_bVisible)
  {
    const SDL_PixelFormatDetails* fmt = SDL_GetPixelFormatDetails(ScreenSurface.format);
    SDL_Palette* pal = SDL_GetSurfacePalette(&ScreenSurface);
    SDL_FillSurfaceRect(&ScreenSurface, nullptr, SDL_MapRGB(fmt, pal, 255, 255, 255));

    for (const auto child : m_ChildWindows)
    {
      if (child)
      {
        child->PaintToSurface(ScreenSurface, FloatingSurface, Offset);
      }
    }
  }
}

void CapriceDisasmPanelView::PreUpdate()
{
  m_pDisasmPanel->PreUpdate();
}

void CapriceDisasmPanelView::PostUpdate()
{
  m_pDisasmPanel->PostUpdate();
}
