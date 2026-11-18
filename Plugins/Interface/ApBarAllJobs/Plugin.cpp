// The AP bar is laid out where the basic info window and the player gauge ask CSession::IsTreeGroupThree (4th job)
// for the displayed job. While one of those runs it answers true:
// - UIWindowMgr::EnsureWindow(WID_BASICINFOWND): the window's constructor and its first size;
// - UIBasicInfoWnd::SetHeight / RefreshLayout / Paint: bar placement and drawing;
// - CGameMode::SendMsg(MM_TOGGLE_PLAYERGAGE / MM_REFRESH_PLAYERGAGE): the player gauge's AP values and size.
// CSession::GetMaxBaseLevel / GetMaxJobLevel, which those also call, still see the real answer (the job's level caps).
#include "RoPlugin.h"

#include "Client/Mode/CGameMode.h"
#include "Client/Mode/ModeMessage.h"
#include "Client/Session/Session.h"
#include "UI/Status/UIBasicInfoWnd.h"
#include "UI/Window/UIWindowMgr.h"
#include "UI/Window/WindowId.h"

namespace {
  constexpr const char* kName = "ApBarAllJobs";

  thread_local int g_force;

  struct Force {
    Force() { ++g_force; }
    ~Force() { --g_force; }
  };

  struct Suspend {
    int saved = g_force;
    Suspend() { g_force = 0; }
    ~Suspend() { g_force = saved; }
  };

  bool (CSession::*g_isTreeGroupThree)(int) const;
  int (CSession::*g_getMaxBaseLevel)(int) const;
  int (CSession::*g_getMaxJobLevel)(int) const;
  UIWindow* (UIWindowMgr::*g_ensureWindow)(int);
  void (UIBasicInfoWnd::*g_setHeight)(int);
  void (UIBasicInfoWnd::*g_refreshLayout)();
  void (UIBasicInfoWnd::*g_paint)();
  int (CGameMode::*g_sendMsg)(int, int, int, int, int);

  struct SessionHook : CSession {
    bool IsTreeGroupThree(int job) const {
      return g_force > 0 || (this->*g_isTreeGroupThree)(job);
    }
    int GetMaxBaseLevel(int job) const {
      Suspend suspend;
      return (this->*g_getMaxBaseLevel)(job);
    }
    int GetMaxJobLevel(int job) const {
      Suspend suspend;
      return (this->*g_getMaxJobLevel)(job);
    }
  };

  struct WindowMgrHook : UIWindowMgr {
    UIWindow* EnsureWindow(int id) {
      if (id != WID_BASICINFOWND) {
        return (this->*g_ensureWindow)(id);
      }
      Force force;
      return (this->*g_ensureWindow)(id);
    }
  };

  struct BasicInfoHook : UIBasicInfoWnd {
    void SetHeight(int height) {
      Force force;
      (this->*g_setHeight)(height);
    }
    void RefreshLayout() {
      Force force;
      (this->*g_refreshLayout)();
    }
    void Paint() {
      Force force;
      (this->*g_paint)();
    }
  };

  struct GameModeHook : CGameMode {
    int SendMsg(int event, int argument1, int argument2, int argument3, int argument4) {
      if (event != MM_TOGGLE_PLAYERGAGE && event != MM_REFRESH_PLAYERGAGE) {
        return (this->*g_sendMsg)(event, argument1, argument2, argument3, argument4);
      }
      Force force;
      return (this->*g_sendMsg)(event, argument1, argument2, argument3, argument4);
    }
  };
}

RO_PLUGIN_EXPORT bool RoPlugin_Init(const RoHostApi* host) {
  return ro::Hook(host, &CSession::IsTreeGroupThree, &SessionHook::IsTreeGroupThree, g_isTreeGroupThree, kName) &&
         ro::Hook(host, &CSession::GetMaxBaseLevel, &SessionHook::GetMaxBaseLevel, g_getMaxBaseLevel, kName) &&
         ro::Hook(host, &CSession::GetMaxJobLevel, &SessionHook::GetMaxJobLevel, g_getMaxJobLevel, kName) &&
         ro::Hook(host, &UIWindowMgr::EnsureWindow, &WindowMgrHook::EnsureWindow, g_ensureWindow, kName) &&
         ro::Hook(host, &UIBasicInfoWnd::SetHeight, &BasicInfoHook::SetHeight, g_setHeight, kName) &&
         ro::Hook(host, &UIBasicInfoWnd::RefreshLayout, &BasicInfoHook::RefreshLayout, g_refreshLayout, kName) &&
         ro::HookVirtual(host, "?Paint@UIBasicInfoWnd@@UAEXXZ", &BasicInfoHook::Paint, g_paint, kName) &&
         ro::HookVirtual(host, "?SendMsg@CGameMode@@UAEHHHHHH@Z", &GameModeHook::SendMsg, g_sendMsg, kName);
}
