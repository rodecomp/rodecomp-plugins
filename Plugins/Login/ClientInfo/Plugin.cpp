// clientinfo.xml support for Gravity's unpatched client, rebuilt from the third-party "clientinfo" kit that used to be
// patched into the exe (unpatch REPORT patches A, C, T, U) plus the login packet tweaks that went with it (0007, Y).
// Gravity's WinMain calls InitializeServiceStrings, whose case 0/1 runs InitializeServiceConfiguration (hard-coded
// kRO addresses and GM lists); no clientinfo.xml reader is ever called. All state lives in this plugin.
// - A: the InitializeServiceStrings detour opens clientinfo.xml through CFile::Open (so CustomPaths' ClientInfo
//   setting still renames it) and parses it with Gravity's XmlDocument. Before the original it applies <clientinfo>'s
//   own tags as Gravity's ReadClientInfo does (servicetype, servertype, hideaccountlist, extendedslot, readfolder,
//   loading, viewhomepage): that function has no caller in the original, so the link drops it and plugins cannot call
//   it. Then it sets g_serviceType from connection 0's <langtype>, since the service type drives
//   InitializeServiceStrings. After the original (which may run InitializeServiceConfiguration and LoadEtcInfo) it
//   applies connection 0: address, port, domain, <aid>/<yellow> <admin> lists into g_adminAccountIds/g_monsterJobs
//   (monster jobs fall back to the admin list), <loading><image> screens (loading00.jpg if none) and
//   g_serviceFlag07 = langtype.
// - C (ServerList): with two or more <connection> entries, CLoginMode::SendMsg turns LMM_GOTOSELECTACCOUNT (the login
//   window's Cancel; argument2 201 still quits) into state 6 with the clientinfo list; CLoginMode::OnChangeState(6)
//   fills serverList from up to 8 <connection> entries (name from <display> else <address>, ^RRGGBB stripped) when
//   that list was asked for or the char-server list has no 1..8 entries, as the kit did, then sets each <balloon> on
//   the list box after the original. LMM_SELECTSVR on that list applies the chosen connection and returns to the
//   login window (state 3). While OnChangeState(4) runs, CConnection::Connect gets the clientinfo address, domain and
//   port instead of kRO's.
// - 0007 (LoginVersionFlag): Gravity only reads g_serviceFlag07 as the version of the login packets (0x64, 0x825,
//   0xC0E and the ban notice), so it is set to 0x80000000 instead of the langtype.
// - Y (AlwaysSendLogin): OnChangeState(4) skips the 0x64 packet for service types 0, 12 and 18 right after
//   SendExecutableDigest. That detour hides the type (g_serviceType = -1 takes the plain-password path) until
//   CConnection::Send of the 0x64 packet, which gets the client type of the real service type; the type is restored
//   there or when OnChangeState returns.
// - T (QuitPrompt): UIButton/UIStylingShopButton::OnLBtnUp: a Cancel button in mode state 2 asks
//   MSI_DO_YOU_REALLY_WANT_TO_QUIT and sends MM_QUIT on Yes instead of passing Cancel to its window. The kit also
//   patched UITextureButton::OnLBtnUp, but the original never constructs a UITextureButton (its constructor has no
//   caller), so the link drops the class and there is nothing to hook.
// - U (StatusWindowGuard): UIWindowMgr::CloseWindow(WID_STATUSWND) with statusWnd a sentinel 1..8 drops it without
//   Schedule (delete).
// Kit patch B (restore g_serviceType at an OnChangeState jump-table landing) is not reproduced: in the original exe
// nothing jumps to that address, so it never ran.
#include "RoPlugin.h"

#include "Client/Mode/CLoginMode.h"
#include "Client/Mode/CMode.h"
#include "Client/Mode/ModeMessage.h"
#include "Client/Mode/ModeMgr.h"
#include "Client/Session/Session.h"
#include "Contents/Configuration/ServiceConfiguration.h"
#include "Contents/Configuration/ServiceEnums.h"
#include "Contents/Tables/MessageTable.h"
#include "File/File.h"
#include "Network/Connection/Connection.h"
#include "Network/Packets/ExecutableDigestPacket.h"
#include "Network/Packets/PacketId.h"
#include "Resource/MSGSTRINGID.h"
#include "Resource/Xml/XmlNode.h"
#include "UI/Buttons/UIButton.h"
#include "UI/Lists/UIListBox.h"
#include "UI/Login/UISelectServerWnd.h"
#include "UI/StylingShop/UIStylingShopWnd.h"
#include "UI/Window/UIEnums.h"
#include "UI/Window/UIWindowMgr.h"
#include "UI/Window/WindowId.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

extern bool g_clientInfoLoaded;
extern bool g_hideAccountList;
extern bool g_extendedSlot;
extern bool g_readFolder;

namespace {
  constexpr const char* kName = "ClientInfo";
  constexpr int kMaxServers = 8;
  constexpr int kSelectServerState = 6;
  constexpr int kAccountLoginState = 4;
  constexpr int kLoginWindowState = 3;
  constexpr int kQuitPromptState = 2;
  constexpr int kNoServiceType = -1;
  constexpr std::uint32_t kVersionFlag = 0x80000000u;

  const RoHostApi* g_host;
  bool g_serverList;
  bool g_versionFlag;

  XmlDocument* g_document;
  XmlNode* g_root;
  std::string g_address;
  std::string g_port;
  std::string g_domain;
  bool g_hasAddress;
  bool g_hasPort;
  bool g_hasDomain;
  std::vector<std::string> g_balloons;
  bool g_listRequested;
  bool g_listShown;
  bool g_accountLogin;
  int g_hiddenServiceType = kNoServiceType;

  decltype(&InitializeServiceStrings) g_initializeServiceStrings;
  int (CLoginMode::*g_loginSendMsg)(int, int, int, int, int);
  void (CLoginMode::*g_onChangeState)(int);
  bool (CConnection::*g_connect)(const ConnectionEndpoint&);
  void (CConnection::*g_send)(int, const void*);
  decltype(&SendExecutableDigest) g_sendExecutableDigest;
  void (UIButton::*g_buttonUp)(int, int);
  void (UIStylingShopButton::*g_stylingButtonUp)(int, int);
  bool (UIWindowMgr::*g_closeWindow)(int);

  const std::string* ChildText(XmlNode* node, const char* name) {
    XmlNode* child = node->FindChild(name);
    return child != nullptr ? &child->Text() : nullptr;
  }

  // Gravity's ReadClientInfo (0x00AD2A00) on <clientinfo>, without its error boxes for a missing or unknown
  // servicetype / servertype: a clientinfo.xml for a private server sets langtype per connection instead.
  void ReadClientInfoTags(XmlNode* clientInfo) {
    static const struct { const char* name; int type; } kServiceTypes[] = {
      {"korea", ServiceKorea},         {"america", ServiceAmerica},     {"japan", ServiceJapan},
      {"china", ServiceChina},         {"taiwan", ServiceTaiwan},       {"thai", ServiceThai},
      {"indonesia", ServiceIndonesia}, {"philippine", ServicePhilippine}, {"malaysia", ServiceMalaysia},
      {"singapore", ServiceSingapore}, {"germany", ServiceGermany},     {"india", ServiceIndia},
      {"brazil", ServiceBrazil},       {"australia", ServiceAustralia}, {"russia", ServiceRussia},
      {"vietnam", ServiceVietnam},     {"chile", ServiceChile},         {"france", ServiceFrance},
      {"uae", ServiceUae},             {"eu", ServiceEu},
    };
    if (const std::string* service = ChildText(clientInfo, "servicetype")) {
      for (const auto& entry : kServiceTypes) {
        if (*service == entry.name) {
          g_serviceType = entry.type;
          break;
        }
      }
    }
    if (const std::string* server = ChildText(clientInfo, "servertype")) {
      if (*server == "primary") {
        g_serviceMode = g_serviceMode == 13 ? 13 : ServerPrimary;
      } else if (*server == "sakray") {
        g_serviceMode = ServerSakray;
      } else if (*server == "local") {
        g_serviceMode = ServerLocal;
      } else if (*server == "pk") {
        g_serviceMode = ServerPk;
      }
    }
    g_hideAccountList = g_hideAccountList || clientInfo->FindChild("hideaccountlist") != nullptr;
    g_extendedSlot = g_extendedSlot || clientInfo->FindChild("extendedslot") != nullptr;
    g_readFolder = g_readFolder || clientInfo->FindChild("readfolder") != nullptr;
    if (XmlNode* loading = clientInfo->FindChild("loading")) {
      for (XmlNode* image = loading->FindChild("image"); image != nullptr; image = image->FindSibling("image")) {
        g_loadingScreens.push_back("유저인터페이스\\" + image->Text());
      }
    }
    if (const std::string* homepage = ChildText(clientInfo, "viewhomepage")) {
      sprintf_s(g_homepageUrl, sizeof(g_homepageUrl), "%s", homepage->c_str());
    }
    g_clientInfoLoaded = true;
  }

  XmlNode* Connection(int index) {
    if (g_root == nullptr) {
      return nullptr;
    }
    XmlNode* connection = g_root->FindChild("connection");
    for (; connection != nullptr && index > 0; --index) {
      connection = connection->FindSibling("connection");
    }
    return connection;
  }

  int ConnectionCount() {
    int count = 0;
    for (XmlNode* connection = Connection(0); connection != nullptr; connection = connection->FindSibling("connection")) {
      ++count;
    }
    return count;
  }

  void ReadIds(XmlNode* list, std::vector<int>& output) {
    output.clear();
    for (XmlNode* admin = list->FindChild("admin"); admin != nullptr; admin = admin->FindSibling("admin")) {
      output.push_back(std::atoi(admin->Text().c_str()));
    }
  }

  bool ReadText(XmlNode* connection, const char* name, std::string& output) {
    const std::string* text = ChildText(connection, name);
    if (text == nullptr) {
      return false;
    }
    output = *text;
    return true;
  }

  void ApplyServiceType(XmlNode* connection) {
    if (const std::string* langtype = ChildText(connection, "langtype")) {
      g_serviceType = std::atoi(langtype->c_str());
    }
  }

  // The kit's ApplyClientInfo(index) for one <connection>, minus the service type (ApplyServiceType).
  void ApplyConnection(XmlNode* connection) {
    g_hasAddress = ReadText(connection, "address", g_address) || g_hasAddress;
    g_hasPort = ReadText(connection, "port", g_port) || g_hasPort;
    g_hasDomain = ReadText(connection, "domain", g_domain) || g_hasDomain;
    // Gravity's InitializeServiceConfiguration (run by InitializeServiceStrings) has filled both lists with kRO's
    // built-in IDs; the kit nopped that call, so only clientinfo.xml's lists may count: <aid> or none, and <yellow>,
    // else the admin list (IsMonsterJob picks the GM sprite, IsAdminAccount the GM menus).
    g_adminAccountIds.clear();
    g_monsterJobs.clear();
    if (XmlNode* aid = connection->FindChild("aid")) {
      ReadIds(aid, g_adminAccountIds);
    }
    if (XmlNode* yellow = connection->FindChild("yellow")) {
      ReadIds(yellow, g_monsterJobs);
    }
    if (g_monsterJobs.empty()) {
      g_monsterJobs.assign(g_adminAccountIds.begin(), g_adminAccountIds.end());
    }
    if (XmlNode* loading = connection->FindChild("loading")) {
      g_loadingScreens.clear();
      for (XmlNode* image = loading->FindChild("image"); image != nullptr; image = image->FindSibling("image")) {
        if (image->Text().empty()) {
          break;
        }
        g_loadingScreens.push_back(std::string("유저인터페이스\\") + image->Text());
      }
    }
    if (g_loadingScreens.empty()) {
      g_loadingScreens.push_back(std::string("유저인터페이스\\loading00.jpg"));
    }
    if (g_versionFlag) {
      g_serviceFlag07 = static_cast<int>(kVersionFlag);
    } else if (const std::string* langtype = ChildText(connection, "langtype")) {
      g_serviceFlag07 = std::atoi(langtype->c_str());
    }
  }

  void LoadDocument() {
    CFile file;
    if (!file.Open("clientinfo.xml", 0, true, false)) {
      return;
    }
    g_document = new XmlDocument();
    const char* data = static_cast<const char*>(file.GetData());
    g_document->Parse(data, data + file.GetSize());
    file.Close();
    g_root = g_document->FindChild("clientinfo");
  }

  void InitializeServiceStringsDetour(char* protection) {
    if (g_document == nullptr) {
      LoadDocument();
    }
    XmlNode* connection = Connection(0);
    if (g_root != nullptr) {
      ReadClientInfoTags(g_root);
      if (connection != nullptr) {
        ApplyServiceType(connection);
      }
    }
    g_initializeServiceStrings(protection);
    if (connection != nullptr) {
      ApplyConnection(connection);
    } else if (g_versionFlag) {
      g_serviceFlag07 = static_cast<int>(kVersionFlag);
    }
    g_host->Log("%s: %s, langtype %d, address %s:%s, %u admin account(s), %u yellow-list account(s)", kName,
                g_root == nullptr ? "no clientinfo.xml" : "clientinfo.xml read", g_serviceType,
                g_hasAddress ? g_address.c_str() : "(game's)", g_hasPort ? g_port.c_str() : "(game's)",
                static_cast<unsigned>(g_adminAccountIds.size()), static_cast<unsigned>(g_monsterJobs.size()));
  }

  // Copies `text` without ^RRGGBB colour codes, at most capacity - 1 characters.
  void CopyWithoutColors(const std::string& text, char* output, std::size_t capacity) {
    std::size_t used = 0;
    for (std::size_t index = 0; index < text.size() && used + 1 < capacity; ++index) {
      if (text[index] == '^' && text.size() - index > 6) {
        index += 6;
        continue;
      }
      output[used++] = text[index];
    }
    output[used] = '\0';
  }

  // Dotted quad in the in-memory order of ServerAddress::ip (first octet in the low byte); 0 if malformed.
  std::uint32_t ParseAddress(const std::string& text) {
    std::uint32_t address = 0;
    unsigned int octet = 0;
    int dots = 0;
    for (const char c : text) {
      if (c == '.') {
        if (++dots > 3) {
          return 0;
        }
        address |= octet << (8 * (dots - 1));
        octet = 0;
      } else if (c < '0' || c > '9' || (octet = octet * 10 + (c - '0')) > 0xFF) {
        return 0;
      }
    }
    return dots == 3 ? address | (octet << 24) : 0;
  }

  int FillServerList(CLoginMode* mode) {
    g_balloons.clear();
    int count = 0;
    for (XmlNode* connection = Connection(0); connection != nullptr && count < kMaxServers;
         connection = connection->FindSibling("connection"), ++count) {
      ServerAddress2& server = mode->serverList[count];
      std::memset(&server, 0, sizeof(server));
      const std::string* address = ChildText(connection, "address");
      const std::string* display = ChildText(connection, "display");
      if (address != nullptr) {
        server.address.ip = ParseAddress(*address);
      }
      if (const std::string* port = ChildText(connection, "port")) {
        server.address.port = static_cast<std::int16_t>(std::atoi(port->c_str()));
      }
      server.address.userCount = 4;
      if (const std::string* name = display != nullptr ? display : address) {
        CopyWithoutColors(*name, server.address.name, sizeof(server.address.name));
        CopyWithoutColors(*name, server.domain, sizeof(server.domain));
      }
      const std::string* balloon = ChildText(connection, "balloon");
      g_balloons.push_back(balloon != nullptr ? *balloon : std::string());
    }
    mode->serverCount = count;
    mode->selectedServer = count != 0 ? 0 : -1;
    return count;
  }

  // GetClientType for service types 0, 12 and 18, which OnChangeState(4) asks while g_serviceType is hidden.
  int ClientType(int serviceType) {
    if (g_serviceMode == ServerSakray) {
      return serviceType == ServiceBrazil ? 22 : 2;
    }
    if (g_serviceMode == ServerPk) {
      return 0;
    }
    switch (serviceType) {
    case ServiceBrazil:
      return 22;
    case ServiceFrance:
      return 31;
    default:
      return 0;
    }
  }

  void RestoreServiceType() {
    if (g_hiddenServiceType != kNoServiceType) {
      g_serviceType = g_hiddenServiceType;
      g_hiddenServiceType = kNoServiceType;
    }
  }

  struct LoginModeHook : CLoginMode {
    int SendMsgDetour(int event, int argument1, int argument2, int argument3, int argument4) {
      if (g_serverList) {
        if (event == LMM_GOTOSELECTACCOUNT && argument2 != 201 && ConnectionCount() >= 2) {
          g_listRequested = true;
          this->pendingCursorType = kSelectServerState;
          return 0;
        }
        if (event == LMM_SELECTSVR && g_listShown && argument1 >= 0) {
          g_listShown = false;
          this->selectedServer = argument1;
          if (XmlNode* connection = Connection(argument1)) {
            ApplyServiceType(connection);
            ApplyConnection(connection);
          }
          this->pendingCursorType = kLoginWindowState;
          return 0;
        }
      }
      return (this->*g_loginSendMsg)(event, argument1, argument2, argument3, argument4);
    }

    void OnChangeStateDetour(int value) {
      g_listShown = value == kSelectServerState && g_serverList &&
                    (g_listRequested || this->serverCount < 1 || this->serverCount > kMaxServers) &&
                    FillServerList(this) != 0;
      g_listRequested = false;
      g_accountLogin = value == kAccountLoginState;
      (this->*g_onChangeState)(value);
      g_accountLogin = false;
      RestoreServiceType();
      if (!g_listShown) {
        return;
      }
      auto* window = static_cast<UISelectServerWnd*>(g_windowMgr.LookupWindow(WID_SELECTSERVERWND));
      if (window != nullptr && window->m_serverList != nullptr) {
        window->m_serverList->balloonTexts.assign(g_balloons.begin(), g_balloons.end());
      }
    }
  };

  struct ConnectionHook : CConnection {
    bool ConnectDetour(const ConnectionEndpoint& endpoint) {
      if (!g_accountLogin || !g_hasAddress) {
        return (this->*g_connect)(endpoint);
      }
      ConnectionEndpoint redirected = endpoint;
      std::strncpy(redirected.address, g_address.c_str(), sizeof(redirected.address) - 1);
      redirected.address[sizeof(redirected.address) - 1] = '\0';
      const std::string& host = g_hasDomain ? g_domain : g_address;
      std::strncpy(redirected.overrideAddress, host.c_str(), sizeof(redirected.overrideAddress) - 1);
      redirected.overrideAddress[sizeof(redirected.overrideAddress) - 1] = '\0';
      if (g_hasPort) {
        redirected.port = std::atoi(g_port.c_str());
      }
      return (this->*g_connect)(redirected);
    }

    void SendDetour(int bytes, const void* data) {
      if (g_hiddenServiceType == kNoServiceType || bytes < 0x37 || *static_cast<const std::int16_t*>(data) != ID_CA_LOGIN) {
        (this->*g_send)(bytes, data);
        return;
      }
      unsigned char packet[0x37];
      std::memcpy(packet, data, sizeof(packet));
      packet[0x36] = static_cast<unsigned char>(ClientType(g_hiddenServiceType));
      RestoreServiceType();
      (this->*g_send)(sizeof(packet), packet);
    }
  };

  void SendExecutableDigestDetour() {
    g_sendExecutableDigest();
    if (g_accountLogin && (g_serviceType == ServiceKorea || g_serviceType == ServiceBrazil || g_serviceType == ServiceFrance)) {
      g_hiddenServiceType = g_serviceType;
      g_serviceType = kNoServiceType;
    }
  }

  // The kit's quit prompt; true when it handled the click (the caller then skips the original).
  bool QuitPrompt(UIWindow* button, int x, int y) {
    if (g_windowMgr.Capture() != button || x < 0 || x >= button->geometry.width || y < 0 || y >= button->geometry.height ||
        button->parent == nullptr || button->geometry.id != ID_CANCEL || g_modeMgr.currentMode == nullptr ||
        g_modeMgr.currentMode->cursorType != kQuitPromptState || g_windowMgr.WindowPolicy(3, button->parent->geometry.id) == 0) {
      return false;
    }
    if (g_windowMgr.Confirm(LookupMessage(MSI_DO_YOU_REALLY_WANT_TO_QUIT), 2, true, false, 0, "Message", 280, 120, 0) == ID_YES) {
      g_modeMgr.currentMode->SendMsg(MM_QUIT, 0, 0, 0, 0);
    }
    button->Invalidate();
    g_windowMgr.ReleaseCapture();
    return true;
  }

  struct ButtonHook : UIButton {
    void OnLBtnUpDetour(int x, int y) {
      if (this->m_disabled == 0 && QuitPrompt(this, x, y)) {
        this->geometry.state = 1;
        return;
      }
      (this->*g_buttonUp)(x, y);
    }
  };

  struct StylingButtonHook : UIStylingShopButton {
    void OnLBtnUpDetour(int x, int y) {
      if (this->m_disabled == 0 && QuitPrompt(this, x, y)) {
        this->geometry.state = 2;
        return;
      }
      (this->*g_stylingButtonUp)(x, y);
    }
  };

  struct WindowMgrHook : UIWindowMgr {
    bool CloseWindowDetour(int id) {
      const auto pointer = reinterpret_cast<std::uintptr_t>(this->statusWnd);
      if (id != WID_STATUSWND || pointer == 0 || pointer > 8) {
        return (this->*g_closeWindow)(id);
      }
      this->statusWnd = nullptr;  // the original then skips Schedule but still runs its shared tail
      (this->*g_closeWindow)(id);
      this->statusWndInfo.visible = 0;
      return true;
    }
  };
}

RO_PLUGIN_EXPORT bool RoPlugin_Init(const RoHostApi* host) {
  g_host = host;
  if (host->version < 6) {
    host->Log("%s: needs RoPluginHost API 6 for settings, found %u", kName, host->version);
    return false;
  }
  g_serverList = host->SettingBool(kName, "ServerList");
  g_versionFlag = host->SettingBool(kName, "LoginVersionFlag");
  const bool alwaysSendLogin = host->SettingBool(kName, "AlwaysSendLogin");
  const bool quitPrompt = host->SettingBool(kName, "QuitPrompt");
  const bool statusGuard = host->SettingBool(kName, "StatusWindowGuard");

  if (!ro::Hook(host, &InitializeServiceStrings, &InitializeServiceStringsDetour, g_initializeServiceStrings, kName) ||
      !ro::HookVirtual(host, "?SendMsg@CLoginMode@@UAEHHHHHH@Z", &LoginModeHook::SendMsgDetour, g_loginSendMsg, kName) ||
      !ro::HookVirtual(host, "?OnChangeState@CLoginMode@@UAEXH@Z", &LoginModeHook::OnChangeStateDetour, g_onChangeState, kName) ||
      !ro::HookVirtual(host, "?Connect@CConnection@@UAE_NABUConnectionEndpoint@@@Z", &ConnectionHook::ConnectDetour, g_connect, kName)) {
    return false;
  }
  if (alwaysSendLogin &&
      (!ro::Hook(host, &SendExecutableDigest, &SendExecutableDigestDetour, g_sendExecutableDigest, kName) ||
       !ro::Hook(host, &CConnection::Send, &ConnectionHook::SendDetour, g_send, kName))) {
    return false;
  }
  if (quitPrompt) {
    if (!ro::HookVirtual(host, "?OnLBtnUp@UIButton@@UAEXHH@Z", &ButtonHook::OnLBtnUpDetour, g_buttonUp, kName) ||
        !ro::HookVirtual(host, "?OnLBtnUp@UIStylingShopButton@@UAEXHH@Z", &StylingButtonHook::OnLBtnUpDetour, g_stylingButtonUp, kName)) {
      return false;
    }
  }
  return !statusGuard || ro::Hook(host, &UIWindowMgr::CloseWindow, &WindowMgrHook::CloseWindowDetour, g_closeWindow, kName);
}
