// 'Menu' window for konCePCja
// Inherited from CFrame

#include <map>
#include <string>
#include "std_ex.h"
#include "CapriceMenu.h"
#include "CapriceOptions.h"
#include "CapriceLoadSave.h"
#include "CapriceMemoryTool.h"
#include "CapriceAbout.h"
#include "cap32.h"
#include "slotshandler.h"
#include "cartridge.h"
#include "portable-file-dialogs.h"
#include "log.h"

// CPC emulation properties, defined in cap32.h:
extern t_CPC CPC;
extern t_drive driveA;
extern t_drive driveB;

namespace wGui {

CapriceMenu::CapriceMenu(const CRect& WindowRect, CWindow* pParent, SDL_Surface* screen, CFontEngine* pFontEngine) :
  CFrame(WindowRect, pParent, pFontEngine, "konCePCja - Menu", false), m_pScreenSurface(screen)
{
  Application().MessageServer()->RegisterMessageClient(this, CMessage::CTRL_MESSAGEBOXRETURN);
  SetModal(true);
  std::map<MenuItem, std::string> buttons = {
    { MenuItem::OPTIONS, "Options" },
    { MenuItem::LOAD_DISK_A, "Load Disk A..." },
    { MenuItem::LOAD_DISK_B, "Load Disk B..." },
    { MenuItem::SAVE_DISK_A, "Save Disk A..." },
    { MenuItem::SAVE_DISK_B, "Save Disk B..." },
    { MenuItem::LOAD_SNAPSHOT, "Load Snapshot..." },
    { MenuItem::SAVE_SNAPSHOT, "Save Snapshot..." },
    { MenuItem::LOAD_TAPE, "Load Tape..." },
    { MenuItem::LOAD_CARTRIDGE, "Load Cartridge..." },
    { MenuItem::INSERT_NEW_DISK, "Insert New Disk" },
    { MenuItem::MEMORY_TOOL, "Memory tool" },
    { MenuItem::DEVTOOLS, "DevTools (Shift+F2)" },
    { MenuItem::RESET, "Reset (F5)" },
    { MenuItem::ABOUT, "About" },
    { MenuItem::RESUME, "Resume" },
    { MenuItem::QUIT, "Quit (F10)" }
  };
  CPoint button_space = CPoint(0, 30);
  CRect button_rect(CPoint(20, 10), 180, 20);

  for(auto& b : buttons) {
    CButton *button = new CButton(button_rect, this, b.second);
    button->SetIsFocusable(true);
    m_buttons.emplace_back(b.first, button);
    button_rect += button_space;
  }

  int padding = 20;
  int total_height = button_rect.Top() + button_rect.Height() + padding;
  int total_width = button_rect.Width() + padding * 2;

  CPoint top_left((m_pScreenSurface->w - total_width) / 2, (m_pScreenSurface->h - total_height) / 2);
  SetWindowRect(CRect(top_left, total_width, total_height));

  CRGBColor bg = GetBackgroundColor();
  bg.alpha = 0xCC; // ~80% opacity
  SetBackgroundColor(bg);
}

CapriceMenu::~CapriceMenu() = default;

void CapriceMenu::CloseFrame() {
  // Exit gui
  Application().MessageServer()->QueueMessage(new CMessage(CMessage::APP_EXIT, nullptr, this));
}

bool CapriceMenu::HandleMessage(CMessage* pMessage)
{
  bool bHandled = false;

  MenuItem selected(MenuItem::NONE);
  if (pMessage)
  {
    switch (pMessage->MessageType())
    {
    case CMessage::CTRL_SINGLELCLICK:
      if (pMessage->Destination() == this) {
        for(auto& b : m_buttons) {
          if (pMessage->Source() == b.GetButton()) {
            bHandled = true;
            selected = b.GetItem();
            break;
          }
        }
      }
      break;
    case CMessage::KEYBOARD_KEYDOWN:
      if (m_bVisible && pMessage->Destination() == this) {
        CKeyboardMessage* pKeyboardMessage = dynamic_cast<CKeyboardMessage*>(pMessage);
        if (pKeyboardMessage) {
          switch (pKeyboardMessage->Key) {
            case SDLK_UP:
              bHandled = true;
              CFrame::FocusNext(EFocusDirection::BACKWARD);
              break;
            case SDLK_DOWN:
              bHandled = true;
              CFrame::FocusNext(EFocusDirection::FORWARD);
              break;
            case SDLK_RETURN:
              bHandled = true;
              for(auto &b : m_buttons) {
                if(b.GetButton()->HasFocus()) {
                  selected = b.GetItem();
                }
              }
              break;
            case SDLK_O:
              bHandled = true;
              selected = MenuItem::OPTIONS;
              break;
            case SDLK_M:
              bHandled = true;
              selected = MenuItem::MEMORY_TOOL;
              break;
            case SDLK_D:
              bHandled = true;
              selected = MenuItem::DEVTOOLS;
              break;
            case SDLK_F5:
              bHandled = true;
              selected = MenuItem::RESET;
              break;
            case SDLK_A:
              bHandled = true;
              selected = MenuItem::ABOUT;
              break;
            case SDLK_Q:
            case SDLK_F10:
              bHandled = true;
              selected = MenuItem::QUIT;
              break;
            case SDLK_R:
            case SDLK_ESCAPE:
              bHandled = true;
              selected = MenuItem::RESUME;
              break;
            default:
              break;
          }
        }
      }
      break;
    case CMessage::CTRL_MESSAGEBOXRETURN:
      if (pMessage->Destination() == this) {
        wGui::CValueMessage<CMessageBox::EButton> *pValueMessage = dynamic_cast<CValueMessage<CMessageBox::EButton>*>(pMessage);
        if (pValueMessage && pValueMessage->Value() == CMessageBox::BUTTON_YES)
        {
          cleanExit(0, /*askIfUnsaved=*/false);
        }
      }
      break;
    default:
      break;
    }
  }
  if(!bHandled) {
      bHandled = CFrame::HandleMessage(pMessage);
  }
  switch (selected) {
    case MenuItem::OPTIONS:
      {
        /*CapriceOptions* pOptionsBox = */new CapriceOptions(CRect(ViewToClient(CPoint(m_pScreenSurface->w /2 - 165, m_pScreenSurface->h /2 - 127)), 330, 260), this, nullptr);
        break;
      }
    case MenuItem::LOAD_DISK_A:
      {
        auto f = pfd::open_file("Load Disk A", CPC.current_dsk_path,
          { "Disk Images", "*.dsk *.ipf *.raw *.zip" });
        auto result = f.result();
        if (!result.empty()) {
          CPC.driveA.file = result[0];
          file_load(CPC.driveA);
          CPC.current_dsk_path = result[0].substr(0, result[0].find_last_of("/\\"));
          Application().MessageServer()->QueueMessage(new CMessage(CMessage::APP_EXIT, nullptr, this));
        }
        break;
      }
    case MenuItem::LOAD_DISK_B:
      {
        auto f = pfd::open_file("Load Disk B", CPC.current_dsk_path,
          { "Disk Images", "*.dsk *.ipf *.raw *.zip" });
        auto result = f.result();
        if (!result.empty()) {
          CPC.driveB.file = result[0];
          file_load(CPC.driveB);
          CPC.current_dsk_path = result[0].substr(0, result[0].find_last_of("/\\"));
          Application().MessageServer()->QueueMessage(new CMessage(CMessage::APP_EXIT, nullptr, this));
        }
        break;
      }
    case MenuItem::SAVE_DISK_A:
      {
        if (driveA.tracks == 0) {
          wGui::CMessageBox *pMessageBox = new wGui::CMessageBox(CRect(CPoint(m_ClientRect.Width() /2 - 125, m_ClientRect.Height() /2 - 30), 250, 60), this, nullptr, "Error", "No disk in Drive A", CMessageBox::BUTTON_OK);
          pMessageBox->SetModal(true);
          break;
        }
        auto f = pfd::save_file("Save Disk A", CPC.current_dsk_path,
          { "DSK Image", "*.dsk" });
        auto result = f.result();
        if (!result.empty()) {
          dsk_save(result, &driveA);
          Application().MessageServer()->QueueMessage(new CMessage(CMessage::APP_EXIT, nullptr, this));
        }
        break;
      }
    case MenuItem::SAVE_DISK_B:
      {
        if (driveB.tracks == 0) {
          wGui::CMessageBox *pMessageBox = new wGui::CMessageBox(CRect(CPoint(m_ClientRect.Width() /2 - 125, m_ClientRect.Height() /2 - 30), 250, 60), this, nullptr, "Error", "No disk in Drive B", CMessageBox::BUTTON_OK);
          pMessageBox->SetModal(true);
          break;
        }
        auto f = pfd::save_file("Save Disk B", CPC.current_dsk_path,
          { "DSK Image", "*.dsk" });
        auto result = f.result();
        if (!result.empty()) {
          dsk_save(result, &driveB);
          Application().MessageServer()->QueueMessage(new CMessage(CMessage::APP_EXIT, nullptr, this));
        }
        break;
      }
    case MenuItem::LOAD_SNAPSHOT:
      {
        auto f = pfd::open_file("Load Snapshot", CPC.current_snap_path,
          { "Snapshots", "*.sna *.zip" });
        auto result = f.result();
        if (!result.empty()) {
          CPC.snapshot.file = result[0];
          file_load(CPC.snapshot);
          CPC.current_snap_path = result[0].substr(0, result[0].find_last_of("/\\"));
          Application().MessageServer()->QueueMessage(new CMessage(CMessage::APP_EXIT, nullptr, this));
        }
        break;
      }
    case MenuItem::SAVE_SNAPSHOT:
      {
        auto f = pfd::save_file("Save Snapshot", CPC.current_snap_path,
          { "Snapshot", "*.sna" });
        auto result = f.result();
        if (!result.empty()) {
          snapshot_save(result);
          Application().MessageServer()->QueueMessage(new CMessage(CMessage::APP_EXIT, nullptr, this));
        }
        break;
      }
    case MenuItem::LOAD_TAPE:
      {
        auto f = pfd::open_file("Load Tape", CPC.current_tape_path,
          { "Tape Images", "*.cdt *.voc *.zip" });
        auto result = f.result();
        if (!result.empty()) {
          CPC.tape.file = result[0];
          file_load(CPC.tape);
          CPC.current_tape_path = result[0].substr(0, result[0].find_last_of("/\\"));
          Application().MessageServer()->QueueMessage(new CMessage(CMessage::APP_EXIT, nullptr, this));
        }
        break;
      }
    case MenuItem::LOAD_CARTRIDGE:
      {
        auto f = pfd::open_file("Load Cartridge", CPC.current_cart_path,
          { "Cartridges", "*.cpr *.zip" });
        auto result = f.result();
        if (!result.empty()) {
          CPC.cartridge.file = result[0];
          file_load(CPC.cartridge);
          CPC.current_cart_path = result[0].substr(0, result[0].find_last_of("/\\"));
          emulator_reset();
          Application().MessageServer()->QueueMessage(new CMessage(CMessage::APP_EXIT, nullptr, this));
        }
        break;
      }
    case MenuItem::INSERT_NEW_DISK:
      {
        /*CapriceLoadSave* pLoadSaveBox = */new CapriceLoadSave(CRect(ViewToClient(CPoint(m_pScreenSurface->w /2 - 165, m_pScreenSurface->h /2 - 127)), 330, 260), this, nullptr);
        break;
      }
    case MenuItem::MEMORY_TOOL:
      {
        /*CapriceMemoryTool* pMemoryTool = */new CapriceMemoryTool(CRect(ViewToClient(CPoint(m_pScreenSurface->w /2 - 165, m_pScreenSurface->h /2 - 140)), 330, 270), this, nullptr);
        break;
      }
    case MenuItem::DEVTOOLS:
      {
        showDevTools();
        Application().MessageServer()->QueueMessage(new CMessage(CMessage::APP_EXIT, nullptr, this));
        break;
      }
    case MenuItem::RESET:
      {
        emulator_reset();
        // Exit gui
        Application().MessageServer()->QueueMessage(new CMessage(CMessage::APP_EXIT, nullptr, this));
        break;
      }
    case MenuItem::ABOUT:
      {
        int about_width(220), about_height(260);
        /*CapriceAbout* pAboutBox = */new CapriceAbout(CRect(ViewToClient(CPoint((m_pScreenSurface->w - about_width)/2, (m_pScreenSurface->h - about_height)/2)), about_width, about_height), this, nullptr);
        break;
      }
    case MenuItem::RESUME:
      {
        // Exit gui
        Application().MessageServer()->QueueMessage(new CMessage(CMessage::APP_EXIT, nullptr, this));
        break;
      }
    case MenuItem::QUIT:
      {
        if (driveAltered()) {
          wGui::CMessageBox* m_pMessageBox = new wGui::CMessageBox(CRect(CPoint(m_ClientRect.Width() /2 - 125, m_ClientRect.Height() /2 - 40), 250, 80), this, nullptr, "Quit without saving?", "Unsaved changes. Do you really want to quit?", CMessageBox::BUTTON_YES | CMessageBox::BUTTON_NO);
          m_pMessageBox->SetModal(true);
        } else {
          cleanExit(0, /*askIfUnsaved=*/false);
        }
        break;
      }
    case MenuItem::NONE:
      break;
  }

  return bHandled;
}

} // namespace wGui
