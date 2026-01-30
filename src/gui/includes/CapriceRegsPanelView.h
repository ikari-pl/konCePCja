#ifndef CAPRICE_REGS_PANEL_VIEW_H
#define CAPRICE_REGS_PANEL_VIEW_H

#include "wg_view.h"
#include "wg_groupbox.h"
#include "cap_register.h"

namespace wGui {

class CapriceRegsPanelView final : public CView {
  public:
    CapriceRegsPanelView(CApplication& application, SDL_Surface* surface, const CRect& WindowRect);
    ~CapriceRegsPanelView() final = default;

    void UpdateZ80();

    void PaintToSurface(SDL_Surface& ScreenSurface, SDL_Surface& FloatingSurface, const CPoint& Offset) const override;
    void Flip() const override;

  private:
    CGroupBox* m_pRegsGroup;
    CRegister* m_pRegAF;
    CRegister* m_pRegBC;
    CRegister* m_pRegDE;
    CRegister* m_pRegHL;
    CRegister* m_pRegIX;
    CRegister* m_pRegIY;
    CRegister* m_pRegSP;
    CRegister* m_pRegPC;
    CRegister* m_pRegI;
    CRegister* m_pRegR;

    CapriceRegsPanelView(const CapriceRegsPanelView&) = delete;
    CapriceRegsPanelView& operator=(const CapriceRegsPanelView&) = delete;
};

}

#endif
