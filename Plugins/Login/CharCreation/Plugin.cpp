// Hooks per setting, all on the character creation window (UINewMakeCharWnd):
// - CenterWindow: UIWindowMgr::EnsureWindow moves the WID_NEWMAKECHARWND window 64 pixels down after creating it.
//   (WARP's other half, dropping bg_back2.tga, is already how rodecomp creates the background.)
// - NameHeight: UINewMakeCharWnd::OnCreate resizes the name edit to 130 x NameHeight after the original.
// - JobId: CLoginMode::OnChangeState(10), which sends the creation request from g_charInfo, runs with g_charInfo.job
//   set to JobId, restored afterwards.
// - SecondJob: after UINewMakeCharWnd::SendMsg(ID_SLOT) selects the second race (job 4218), the job becomes SecondJob
//   and the preview is reloaded; UIDescriptRace::Paint draws the race tile with a copy of DrawRaceDescription that
//   shows SecondJob instead of 4218.
// - HairStyles / SecondHairLimit: UINewMakeCharWnd::SendMsg handles MAKECHAR_BUTTON_HAIR_STYLE itself and
//   UIHairStyleButton::SendMsg handles UM_SETCURITEM for both races, with the limits raised. Above 23 human styles the
//   grid pages: cells 0-22 show page*23 + cell, cell 23 shows img_hairStyle99.bmp and advances the page.
// - HairColourGuard / HairColours: UINewMakeCharWnd::SendMsg handles the hair colour swatches (ID_RADIOBTN, 0x1EF)
//   itself. The guard refuses a colour whose head palette (CSession::GetHeadPalName) does not exist, and after every
//   command resets a colour the new style/sex/race has no palette for, so Paint never loads a missing palette. Above 9
//   colours OnCreate adds a tenth swatch (color99) that pages the nine swatches through the colours.
#include "RoPlugin.h"

#include "Client/Mode/CLoginMode.h"
#include "Client/Session/Session.h"
#include "Contents/Tables/MessageTable.h"
#include "Render/DeviceContext/DCSurface.h"
#include "Render/DeviceContext/DeviceContext.h"
#include "Render/Renderer.h"
#include "Render/Textures/SpriteAtlas.h"
#include "Render/Textures/Texture.h"
#include "Resource/MSGSTRINGID.h"
#include "Resource/ResourceMgr.h"
#include "Resource/Sprites/ActionResource.h"
#include "Resource/Sprites/ImfResource.h"
#include "Resource/Sprites/SpriteResource.h"
#include "UI/Buttons/UIBmpRadioBtn.h"
#include "UI/Login/UIDescriptRace.h"
#include "UI/Login/UINewMakeCharWnd.h"
#include "UI/Text/TextWrapping.h"
#include "UI/Window/UIEnums.h"
#include "UI/Window/UIWindowMgr.h"
#include "UI/Window/WindowId.h"

#include <windows.h>
#include <timeapi.h>

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace {
  constexpr const char* kName = "CharCreation";
  constexpr int kDoramJob = 4218;
  constexpr int kStylesPerPage = 23;  // cell 23 is the pager
  constexpr int kStyleCells = 24;
  constexpr int kPagerStyle = 98;     // draws img_hairStyle99.bmp
  constexpr int kColoursPerPage = 9;
  constexpr const char* kArtDir = "유저인터페이스\\make_character_ver2\\";
  constexpr int kColourValue = 0x1EF;

  bool g_center;
  int g_nameHeight;
  int g_jobId;
  int g_secondJob;
  int g_secondHairLimit;
  int g_hairStyles;
  bool g_guard;
  int g_hairColours;

  bool g_styleHook;   // MAKECHAR_BUTTON_HAIR_STYLE and UM_SETCURITEM handled here
  bool g_colourHook;  // the colour swatches handled here

  int g_hairPage;
  int g_colourPage;
  UIBmpRadioBtn* g_colourPager;

  UIWindow* (UIWindowMgr::*g_ensureWindow)(int);
  void (UINewMakeCharWnd::*g_onCreate)(int, int);
  int (UINewMakeCharWnd::*g_sendMsg)(UIWindow*, int, std::intptr_t, std::intptr_t, std::intptr_t, std::intptr_t);
  int (UIHairStyleButton::*g_buttonSendMsg)(UIWindow*, int, std::intptr_t, std::intptr_t, std::intptr_t, std::intptr_t);
  void (CLoginMode::*g_onChangeState)(int);
  void (UIDescriptRace::*g_racePaint)();

  bool StylePaging() {
    return g_hairStyles > kStylesPerPage;
  }

  int StylePages() {
    return (g_hairStyles + kStylesPerPage - 1) / kStylesPerPage;
  }

  int ColourPages() {
    return (g_hairColours + kColoursPerPage - 1) / kColoursPerPage;
  }

  struct RadioAccess : UIBmpRadioBtn {
    void SetValue(int value) {
      this->m_value = value;
    }
  };

  struct HairButtonHook : UIHairStyleButton {
    int Cell() const {
      return this->m_style;
    }

    void ShowPreview(const char* name) {
      this->m_preview->Enable(1);
      this->m_preview->SetBitmap(name, 0, 0);
      this->m_preview->SetBitmap(name, 1, 0);
      this->m_preview->SetBitmap(name, 2, 0);
      this->m_preview->SetBitmap("유저인터페이스\\make_character_ver2\\img_hairStyle_none.bmp", 3, 0);
      this->InvalidateChildren();
    }

    void HidePreview() {
      if (this->m_preview) {
        this->m_preview->Enable(0);
        this->InvalidateChildren();
      }
    }

    int SendMsgDetour(UIWindow* sender, int message, std::intptr_t a, std::intptr_t b, std::intptr_t c, std::intptr_t d) {
      if (message != UM_SETCURITEM || (this->m_race != 0 && this->m_race != 1) || this->m_preview == nullptr) {
        return (this->*g_buttonSendMsg)(sender, message, a, b, c, d);
      }
      char name[128] = {};
      const int cell = this->m_style;
      if (this->m_race == 0) {
        int style = cell;
        if (StylePaging()) {
          style = cell == kStylesPerPage ? kPagerStyle : g_hairPage * kStylesPerPage + cell;
        }
        if (style != kPagerStyle && (style >= g_hairStyles || (!StylePaging() && cell >= kStylesPerPage))) {
          this->HidePreview();
          return 0;
        }
        sprintf_s(name, this->m_sex != 0 ? "유저인터페이스\\make_character_ver2\\img_hairStyle%02d.bmp"
                                         : "유저인터페이스\\make_character_ver2\\img_hairStyle_girl%02d.bmp",
                  style + 1);
      } else {
        if (cell >= g_secondHairLimit) {
          this->HidePreview();
          return 0;
        }
        sprintf_s(name, this->m_sex == 0 ? "유저인터페이스\\make_character_ver2\\img_hairStyle_doramGirl%02d.bmp"
                                         : "유저인터페이스\\make_character_ver2\\img_hairStyle_doramBoy%02d.bmp",
                  cell + 1);
      }
      this->ShowPreview(name);
      return 0;
    }
  };

  struct MakeCharHook : UINewMakeCharWnd {
    HairButtonHook* Button(int index) {
      return static_cast<HairButtonHook*>(this->m_hairStyleButtons[index]);
    }

    bool PaletteExists(int colour) {
      if (colour <= 0) {
        return true;
      }
      const std::string name =
          g_session.GetHeadPalName(this->m_character.head, this->m_character.job, this->m_character.sex, colour);
      return ResourceMgr::Instance().Exists(name.c_str());
    }

    void RefreshSwatches() {
      char name[128];
      for (int index = 0; index < kColoursPerPage; ++index) {
        const int colour = g_colourPage * kColoursPerPage + index;
        UIBmpRadioBtn* button = this->m_hairColorButtons[index];
        if (g_hairColours > kColoursPerPage) {
          sprintf_s(name, "%scolor%02d_off.bmp", kArtDir, colour + 1);
          button->SetBitmap(name, 0);
          sprintf_s(name, "%scolor%02d_over.bmp", kArtDir, colour + 1);
          button->SetBitmap(name, 1);
          sprintf_s(name, "%scolor%02d_on.bmp", kArtDir, colour + 1);
          button->SetBitmap(name, 2);
        }
        button->SendMsg(this, UM_SETSTATE, colour == this->m_character.headPalette ? 1 : 0, 0, 0, 0);
        button->Invalidate();
      }
    }

    void RefreshStyles() {
      for (int index = 0; index < kStyleCells; ++index) {
        HairButtonHook* button = this->Button(index);
        button->SendMsg(nullptr, UM_SETCURITEM, 0, 0, 0, 0);
        const int style = g_hairPage * kStylesPerPage + index;
        button->SendMsg(this, UM_SETSTATE, index < kStylesPerPage && style == this->m_character.head - 1 ? 1 : 0, 0, 0, 0);
      }
    }

    void SelectStyle(UIWindow* sender) {
      int cell = -1;
      for (int index = 0; index < kStyleCells; ++index) {
        if (sender == this->m_hairStyleButtons[index]) {
          cell = index;
        }
      }
      if (cell < 0) {
        return;
      }
      const bool human = this->m_race == 0;
      if (human && StylePaging() && cell == kStylesPerPage) {
        g_hairPage = (g_hairPage + 1) % StylePages();
        this->RefreshStyles();
        return;
      }
      const int style = human && StylePaging() ? g_hairPage * kStylesPerPage + cell : cell;
      if (human ? (style >= g_hairStyles || cell >= kStylesPerPage) : style >= g_secondHairLimit) {
        return;
      }
      for (int index = 0; index < kStyleCells; ++index) {
        this->m_hairStyleButtons[index]->SendMsg(this, UM_SETSTATE, index == cell ? 1 : 0, 0, 0, 0);
      }
      this->m_character.head = static_cast<short>(style + 1);
      this->UpdatePreviewResources();
    }

    void SelectColour(UIWindow* sender) {
      if (g_colourPager != nullptr && sender == g_colourPager) {
        g_colourPage = (g_colourPage + 1) % ColourPages();
        this->RefreshSwatches();
        return;
      }
      int cell = -1;
      for (int index = 0; index < kColoursPerPage; ++index) {
        if (sender == this->m_hairColorButtons[index]) {
          cell = index;
        }
      }
      const int colour = g_colourPage * kColoursPerPage + cell;
      if (cell < 0 || colour >= g_hairColours || (g_guard && !this->PaletteExists(colour))) {
        return;
      }
      this->m_character.headPalette = static_cast<short>(colour);
      this->RefreshSwatches();
      this->UpdatePreviewResources();
    }

    void GuardPalette() {
      if (this->PaletteExists(this->m_character.headPalette)) {
        return;
      }
      this->m_character.headPalette = 0;
      g_colourPage = 0;
      this->RefreshSwatches();
      this->UpdatePreviewResources();
    }

    void OnCreateDetour(int width, int height) {
      g_hairPage = 0;
      g_colourPage = 0;
      g_colourPager = nullptr;
      (this->*g_onCreate)(width, height);
      if (g_nameHeight != 20) {
        this->m_nameEdit->Resize(0x82, g_nameHeight);
      }
      if (g_hairColours > kColoursPerPage) {
        UIBmpRadioBtn* pager = new UIBmpRadioBtn();
        pager->SetBitmap("유저인터페이스\\make_character_ver2\\color99_off.bmp", 0);
        pager->SetBitmap("유저인터페이스\\make_character_ver2\\color99_over.bmp", 1);
        pager->SetBitmap("유저인터페이스\\make_character_ver2\\color99_on.bmp", 2);
        static_cast<RadioAccess*>(pager)->SetValue(kColourValue);
        pager->Move((kColoursPerPage % 5) * 0x1B + 0x26B, (kColoursPerPage / 5) * 0x18 + 0x132);
        this->AddChild(pager);
        g_colourPager = pager;
      }
    }

    int SendMsgDetour(UIWindow* sender, int message, std::intptr_t a, std::intptr_t b, std::intptr_t c, std::intptr_t d) {
      if (message != UM_COMMAND) {
        return (this->*g_sendMsg)(sender, message, a, b, c, d);
      }
      if (a == MAKECHAR_BUTTON_HAIR_STYLE && g_styleHook) {
        this->SelectStyle(sender);
        return 0;
      }
      if (a == ID_RADIOBTN && b == kColourValue && g_colourHook) {
        this->SelectColour(sender);
        return 0;
      }
      if (a == ID_SLOT) {
        g_hairPage = 0;
      }
      const int result = (this->*g_sendMsg)(sender, message, a, b, c, d);
      if (a == ID_SLOT && g_secondJob != kDoramJob && this->m_race == 1 && this->m_character.job == kDoramJob) {
        this->m_character.job = static_cast<short>(g_secondJob);
        this->UpdatePreviewResources();
      }
      if (g_guard && a != ID_OK && a != ID_CANCEL) {
        this->GuardPalette();
      }
      return result;
    }
  };

  struct WindowMgrHook : UIWindowMgr {
    UIWindow* EnsureWindowDetour(int id) {
      UIWindow* window = (this->*g_ensureWindow)(id);
      if (id == WID_NEWMAKECHARWND && window != nullptr) {
        window->Move(window->geometry.x, window->geometry.y + 0x40);
      }
      return window;
    }
  };

  struct LoginModeHook : CLoginMode {
    void OnChangeStateDetour(int value) {
      if (value != 10) {
        (this->*g_onChangeState)(value);
        return;
      }
      const std::int16_t job = g_charInfo.job;
      g_charInfo.job = static_cast<std::int16_t>(g_jobId);
      (this->*g_onChangeState)(value);
      g_charInfo.job = job;
    }
  };

  // UIDescriptRace::Paint is UIBitmapButton::Paint plus the inlined DrawRaceDescription; this is a copy of both with
  // the second race's job taken from SecondJob.
  struct RaceHook : UIDescriptRace {
    void DrawDescription(int message, int text) {
      this->DrawTextAt(50, 23, LookupMessage(message), 0, 0, 12, 0x99634C, 1, 0);
      this->DrawTextAt(49, 22, LookupMessage(message), 0, 0, 12, 0xFFFFFF, 1, 0);
      std::vector<std::string> lines;
      WrapPlainText(LookupMessage(text), lines, 40);
      int y = 22;
      for (unsigned index = 0; index < lines.size(); ++index) {
        this->DrawTextAt(112, y, lines[index].c_str(), 0, 0, 12, 0, 0, 0);
        y += 20;
      }
    }

    void PaintDetour() {
      this->UIBitmapButton::Paint();
      if (this->m_race == 1) {
        if (!g_session.m_makeableDoram) {
          return;
        }
        this->DrawDescription(MSI_NEWMAKECHAR_DORAMINFO, MSI_CASH_MARK2);
      } else if (this->m_race == 0) {
        this->DrawDescription(MSI_NEWMAKECHAR_DORAMJOB, MSI_CASH_MARK1);
      }

      const int job = this->m_race != 1 ? 0 : g_secondJob;
      const int action = this->m_noRace != 0 ? 9 : 1;
      int frame = 0;
      for (int layer = 0; layer < 2; ++layer) {
        CActRes* act = nullptr;
        CSprRes* sprite = nullptr;
        CImfRes* imf = nullptr;
        if (layer == 0) {
          act = static_cast<CActRes*>(ResourceMgr::Instance().Load(g_session.GetBodyActionResourceName(job, g_session.m_sex, job, 0)));
          sprite = static_cast<CSprRes*>(ResourceMgr::Instance().Load(g_session.GetBodySprName(job, g_session.m_sex, job, 0)));
          imf = static_cast<CImfRes*>(ResourceMgr::Instance().Load(g_session.GetBodyImfName(job, g_session.m_sex, 1, job).c_str()));
        } else {
          int head = 1;
          act = static_cast<CActRes*>(ResourceMgr::Instance().Load(g_session.GetHeadActName(job, g_session.m_sex, &head).c_str()));
          sprite = static_cast<CSprRes*>(ResourceMgr::Instance().Load(g_session.GetHeadSprName(job, g_session.m_sex, &head).c_str()));
          imf = static_cast<CImfRes*>(ResourceMgr::Instance().Load(g_session.GetBodyImfName(job, g_session.m_sex, head, job).c_str()));
        }

        if (this->m_noRace) {
          if (this->m_createTime + 60 <= timeGetTime()) {
            int next = this->m_frameIndex + 1;
            if (next >= static_cast<int>(act->FrameCount(action))) {
              next = 0;
            }
            this->m_frameIndex = next;
            this->m_createTime = timeGetTime();
          }
          frame = this->m_frameIndex;
        }

        const ActionFrame* actionFrame = &act->Frame(action, frame);
        const std::uint32_t* palette = sprite->palette;
        POINT point = imf->GetLongPoint(layer, action, frame);
        if (layer == 1) {
          const ActionFrame* body = &static_cast<CActRes*>(
              ResourceMgr::Instance().Load(g_session.GetBodyActionResourceName(job, g_session.m_sex, job, 0)))->Frame(action, frame);
          if (actionFrame->anchorCount != 0 && body->anchorCount != 0 &&
              actionFrame->anchors[0].attachmentId == body->anchors[0].attachmentId) {
            point.x += body->anchors[0].x - actionFrame->anchors[0].x;
            point.y += body->anchors[0].y - actionFrame->anchors[0].y;
          }
        }

        const SpriteFrameCell& cell = actionFrame->Cell(layer);
        if (cell.image == -1) {
          continue;
        }

        const SpriteRleImage* image = sprite->images[cell.imageType][cell.image];
        SpriteAtlasRecord record{};
        CTexture* texture;
        if (cell.imageType == 0) {
          texture = g_renderer->atlas.Find(image, palette, record);
          if (!texture) {
            texture = g_renderer->atlas.Insert(image, palette, record);
          }
        } else {
          texture = static_cast<CTexture*>(const_cast<void*>(image->surface));
        }

        this->geometry.surface->DrawSurface(cell.x + point.x + 60, cell.y + point.y + 140, texture,
                                            static_cast<int>(record.u0 * 256.0f), static_cast<int>(record.v0 * 256.0f),
                                            image->width, image->height, cell.mirrored, image->doubleWidth + 1,
                                            image->doubleHeight + 1, 0);
      }
    }
  };
}

RO_PLUGIN_EXPORT bool RoPlugin_Init(const RoHostApi* host) {
  if (host->version < 6) {
    host->Log("%s: needs RoPluginHost API 6 for settings, found %u", kName, host->version);
    return false;
  }
  g_center = host->SettingBool(kName, "CenterWindow");
  g_nameHeight = static_cast<int>(host->SettingInt(kName, "NameHeight"));
  g_jobId = static_cast<int>(host->SettingInt(kName, "JobId"));
  g_secondJob = static_cast<int>(host->SettingInt(kName, "SecondJob"));
  g_secondHairLimit = static_cast<int>(host->SettingInt(kName, "SecondHairLimit"));
  g_hairStyles = static_cast<int>(host->SettingInt(kName, "HairStyles"));
  g_guard = host->SettingBool(kName, "HairColourGuard");
  g_hairColours = static_cast<int>(host->SettingInt(kName, "HairColours"));

  g_styleHook = g_hairStyles != kStylesPerPage || g_secondHairLimit != 6;
  g_colourHook = g_guard || g_hairColours != kColoursPerPage;
  const bool secondJob = g_secondJob != kDoramJob;
  const bool onCreate = g_nameHeight != 20 || g_hairColours != kColoursPerPage;
  const bool sendMsg = g_styleHook || g_colourHook || secondJob;
  if (!g_center && !onCreate && !sendMsg && g_jobId < 0) {
    host->Log("%s: every setting is at the client's value, nothing to do", kName);
    return true;
  }
  return (!g_center ||
          ro::Hook(host, &UIWindowMgr::EnsureWindow, &WindowMgrHook::EnsureWindowDetour, g_ensureWindow, kName)) &&
         (!onCreate || ro::HookVirtual(host, "?OnCreate@UINewMakeCharWnd@@UAEXHH@Z", &MakeCharHook::OnCreateDetour,
                                       g_onCreate, kName)) &&
         (!sendMsg || ro::HookVirtual(host, "?SendMsg@UINewMakeCharWnd@@UAEHPAVUIWindow@@HHHHH@Z",
                                      &MakeCharHook::SendMsgDetour, g_sendMsg, kName)) &&
         (!g_styleHook || ro::HookVirtual(host, "?SendMsg@UIHairStyleButton@@UAEHPAVUIWindow@@HHHHH@Z",
                                          &HairButtonHook::SendMsgDetour, g_buttonSendMsg, kName)) &&
         (!secondJob || ro::HookVirtual(host, "?Paint@UIDescriptRace@@UAEXXZ", &RaceHook::PaintDetour, g_racePaint, kName)) &&
         (g_jobId < 0 || ro::HookVirtual(host, "?OnChangeState@CLoginMode@@UAEXH@Z", &LoginModeHook::OnChangeStateDetour,
                                         g_onChangeState, kName));
}
