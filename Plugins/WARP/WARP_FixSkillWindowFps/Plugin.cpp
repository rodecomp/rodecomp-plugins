// Reimplements the virtual UINewSkillListWnd::Paint (UINewSkillListWnd.cpp): the rodecomp body with the skill_upbar_mid
// bitmap loaded once before its tiling loop instead of on every pass, and null guards on skill_botbar_mid.
#include "RoPlugin.h"

#include "Client/Session/Session.h"
#include "Contents/Tables/MessageTable.h"
#include "Resource/Bitmap/BitmapResource.h"
#include "Resource/Bitmap/TextureNames.h"
#include "Resource/MSGSTRINGID.h"
#include "Resource/ResourceMgr.h"
#include "UI/Lists/UITabControl.h"
#include "UI/Skill/UINewSkillListWnd.h"
#include "Utility/Text/FormatString.h"

#include <cstdio>
#include <cstring>
#include <string>

namespace {
  constexpr const char* kName = "WARP_FixSkillWindowFps";

  void (UINewSkillListWnd::*g_original)();

  CBitmapRes* LoadBitmap(const char* name) {
    return static_cast<CBitmapRes*>(ResourceMgr::Instance().Load(Resolve(name)));
  }

  struct SkillListHook : UINewSkillListWnd {
    void PaintDetour() {
      const int left = this->m_tabs->geometry.width;
      this->ClearSurface(0xFFFF00FF);
      this->FillRegion(left, this->m_topMargin, this->geometry.width - left, this->geometry.height - this->m_bottomMargin - this->m_topMargin, 0xFFFFFF);
      this->DrawRoundedOutline(left, 0, this->geometry.width - left, this->geometry.height, 0xC3C3C3);
      this->DrawRoundedOutline(left + 2, 2, this->geometry.width - left - 4, this->geometry.height - 4, 0xC3C3C3);

      CBitmapRes* bitmap = LoadBitmap("유저인터페이스\\basic_interface\\skill_upbar_left.bmp");
      this->DrawBitmap(left, 0, bitmap, false);
      bitmap = LoadBitmap("유저인터페이스\\basic_interface\\skill_upbar_right.bmp");
      this->DrawBitmap(this->geometry.width - bitmap->image.width, 0, bitmap, false);

      const int upbarMidCount = (this->geometry.width - left - 24) / 5 + ((this->geometry.width - left - 24) % 5 != 0);
      CBitmapRes* upbarMid = LoadBitmap("유저인터페이스\\basic_interface\\skill_upbar_mid.bmp");
      for (int i = 0, x = left + 12; i < upbarMidCount; ++i, x += 5) {
        this->DrawBitmap(x, 0, upbarMid, false);
      }

      bitmap = LoadBitmap("유저인터페이스\\basic_interface\\skill_botbar_left.bmp");
      this->DrawBitmap(left, this->geometry.height - bitmap->image.height, bitmap, false);
      bitmap = LoadBitmap("유저인터페이스\\basic_interface\\skill_botbar_right.bmp");
      this->DrawBitmap(this->geometry.width - bitmap->image.width, this->geometry.height - bitmap->image.height, bitmap, false);

      CBitmapRes* botbarMid = LoadBitmap("유저인터페이스\\basic_interface\\skill_botbar_mid.bmp");
      const int span = this->geometry.width - left - 24;
      const int tileWidth = botbarMid ? botbarMid->image.width : 5;
      const int botbarMidCount = span / tileWidth + (tileWidth != 0 && span % tileWidth != 0);
      for (int i = 0, x = left + 12; i < botbarMidCount; ++i, x += 5) {
        this->DrawBitmap(x, this->geometry.height - (botbarMid ? botbarMid->image.height : 0), botbarMid, false);
      }

      this->DrawTextAt(41, 6, LookupMessage(MSI_SKILLLIST), 0, 0, 12, 0xFFFFFF, 0, 0);
      this->DrawTextAt(40, 5, LookupMessage(MSI_SKILLLIST), 0, 0, 12, 0, 0, 0);

      const int points = this->m_skillPoints;
      char text[80];
      std::memset(text, 0, sizeof(text));
      if (g_session.m_simplifiedSkillList != 0) {
        std::sprintf(text, LookupMessage(MSI_SKILLPOINT), points);
        this->TextWidth(text, 0, 0, 12, 0, 0);
        this->DrawTextAt(30, this->geometry.height - 23, text, 0, 0, 12, 0, 0, 0);
        this->DrawSkillList();
      } else {
        const int total = g_session.m_skillPoint;
        if (points < total) {
          StringFormat(text, sizeof(text), static_cast<std::size_t>(-1), "%s^0000ff%d^000000(^ff0000%d^000000) / %d", LookupMessage(MSI_REFUSE_CHANGE_PASSWD_SITE), points,
                       total - points, total);
        } else {
          StringFormat(text, sizeof(text), static_cast<std::size_t>(-1), "%s%d", LookupMessage(MSI_REFUSE_CHANGE_PASSWD_SITE), total);
        }
        this->DrawWrappedFormattedText(80, this->geometry.height - 23, std::string(text), 0, 0, 0, 0, 12, 0, 0);
        this->DrawSkillTree();
        this->DrawCharacter();
      }

      this->m_needsRedraw = 0;
    }
  };
}

RO_PLUGIN_EXPORT bool RoPlugin_Init(const RoHostApi* host) {
  return ro::HookVirtual(host, "?Paint@UINewSkillListWnd@@UAEXXZ", &SkillListHook::PaintDetour, g_original, kName);
}
