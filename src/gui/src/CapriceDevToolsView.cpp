// Developers' tools for konCePCja
#include "CapriceDevToolsView.h"
#include "CapriceDevTools.h"
#include "devtools.h"
#include <string>

using namespace wGui;

CapriceDevToolsView::CapriceDevToolsView(CApplication& application, SDL_Surface* surface, SDL_Renderer* renderer, SDL_Texture* texture, const CRect& WindowRect, DevTools* devtools) : CView(application, surface, nullptr, WindowRect), m_pRenderer(renderer), m_pTexture(texture)
{
  Application().MessageServer()->RegisterMessageClient(this, CMessage::CTRL_MESSAGEBOXRETURN);
  m_pDevToolsFrame = new CapriceDevTools(CRect(CPoint(0, 0), WindowRect.Width(), WindowRect.Height()), this, nullptr, devtools);
  m_pDevToolsFrame->UpdateAll();
}

void CapriceDevToolsView::LoadSymbols(const std::string& filename)
{
  m_pDevToolsFrame->LoadSymbols(filename);
}

void CapriceDevToolsView::PaintToSurface(SDL_Surface& ScreenSurface, SDL_Surface& FloatingSurface, const CPoint& Offset) const
{
  if (m_bVisible)
  {
    // Reset backgound
    const SDL_PixelFormatDetails* fmt = SDL_GetPixelFormatDetails(ScreenSurface.format);
    SDL_Palette* pal = SDL_GetSurfacePalette(&ScreenSurface);
    SDL_FillSurfaceRect(&ScreenSurface, nullptr, SDL_MapRGB(fmt, pal, 255, 255, 255));

    // Draw all child windows recursively
    for (const auto child : m_ChildWindows)
    {
      if (child)
      {
        child->PaintToSurface(ScreenSurface, FloatingSurface, Offset);
      }
    }

    int dbg_x = 0, dbg_y = 0;
    if (devtools_get_debug_click(dbg_x, dbg_y)) {
      SDL_Rect marker = { dbg_x - 2, dbg_y - 2, 5, 5 };
      SDL_FillSurfaceRect(&ScreenSurface, &marker, SDL_MapRGB(fmt, pal, 255, 0, 0));
    }
  }
}

void CapriceDevToolsView::PreUpdate()
{
  m_pDevToolsFrame->PreUpdate();
}

void CapriceDevToolsView::PostUpdate()
{
  m_pDevToolsFrame->PostUpdate();
}

void CapriceDevToolsView::Flip() const
{
  if (m_pRenderer && m_pTexture) {
    SDL_UpdateTexture(m_pTexture, nullptr, m_pScreenSurface->pixels, m_pScreenSurface->pitch);
    SDL_RenderClear(m_pRenderer);
    SDL_RenderTexture(m_pRenderer, m_pTexture, nullptr, nullptr);
    SDL_RenderPresent(m_pRenderer);
  }
}

void CapriceDevToolsView::Close()
{
  m_pDevToolsFrame->CloseFrame();
}
