#include "CapriceRegsPanelView.h"
#include "z80.h"
#include <algorithm>

extern t_z80regs z80;

namespace wGui {

namespace {
constexpr int kPanelWidth = 260;
constexpr int kPanelHeight = 130;
constexpr int kPadding = 10;
constexpr int kRowHeight = 20;
constexpr int kColumnWidth = 110;
constexpr int kColumnSpacing = 20;
}

CapriceRegsPanelView::CapriceRegsPanelView(CApplication& application, SDL_Surface* surface, const CRect& WindowRect)
  : CView(application, surface, /*backSurface=*/nullptr, WindowRect)
{
  const int panelX = std::max(0, WindowRect.Width() - kPanelWidth - kPadding);
  const int panelY = kPadding;
  m_pRegsGroup = new CGroupBox(CRect(CPoint(panelX, panelY), kPanelWidth, kPanelHeight), this, "Z80 regs");

  const int leftX = kPadding;
  const int rightX = leftX + kColumnWidth + kColumnSpacing;
  const int startY = kPadding;

  m_pRegAF = new CRegister(CRect(CPoint(leftX, startY + 0 * kRowHeight), kColumnWidth, kRowHeight), m_pRegsGroup, "AF");
  m_pRegBC = new CRegister(CRect(CPoint(rightX, startY + 0 * kRowHeight), kColumnWidth, kRowHeight), m_pRegsGroup, "BC");

  m_pRegDE = new CRegister(CRect(CPoint(leftX, startY + 1 * kRowHeight), kColumnWidth, kRowHeight), m_pRegsGroup, "DE");
  m_pRegHL = new CRegister(CRect(CPoint(rightX, startY + 1 * kRowHeight), kColumnWidth, kRowHeight), m_pRegsGroup, "HL");

  m_pRegIX = new CRegister(CRect(CPoint(leftX, startY + 2 * kRowHeight), kColumnWidth, kRowHeight), m_pRegsGroup, "IX");
  m_pRegIY = new CRegister(CRect(CPoint(rightX, startY + 2 * kRowHeight), kColumnWidth, kRowHeight), m_pRegsGroup, "IY");

  m_pRegSP = new CRegister(CRect(CPoint(leftX, startY + 3 * kRowHeight), kColumnWidth, kRowHeight), m_pRegsGroup, "SP");
  m_pRegPC = new CRegister(CRect(CPoint(rightX, startY + 3 * kRowHeight), kColumnWidth, kRowHeight), m_pRegsGroup, "PC");

  m_pRegI = new CRegister(CRect(CPoint(leftX, startY + 4 * kRowHeight), kColumnWidth, kRowHeight), m_pRegsGroup, "I");
  m_pRegR = new CRegister(CRect(CPoint(rightX, startY + 4 * kRowHeight), kColumnWidth, kRowHeight), m_pRegsGroup, "R");
}

void CapriceRegsPanelView::UpdateZ80()
{
  m_pRegAF->SetValue(z80.AF.w.l);
  m_pRegBC->SetValue(z80.BC.w.l);
  m_pRegDE->SetValue(z80.DE.w.l);
  m_pRegHL->SetValue(z80.HL.w.l);
  m_pRegIX->SetValue(z80.IX.w.l);
  m_pRegIY->SetValue(z80.IY.w.l);
  m_pRegSP->SetValue(z80.SP.w.l);
  m_pRegPC->SetValue(z80.PC.w.l);
  m_pRegI->SetValue(z80.I);
  m_pRegR->SetValue(z80.R);
}

void CapriceRegsPanelView::PaintToSurface(SDL_Surface& ScreenSurface, SDL_Surface& FloatingSurface, const CPoint& Offset) const
{
  if (m_bVisible)
  {
    for (const auto child : m_ChildWindows)
    {
      if (child)
      {
        child->PaintToSurface(ScreenSurface, FloatingSurface, Offset);
      }
    }
  }
}

void CapriceRegsPanelView::Flip() const
{
  // Main loop handles display updates; avoid extra flips here.
}

}
