// Adds Direct3D 12 as render backend 3 ("DirectX 12" in Settings > Graphics > Graphics API) next to the game's DX7 (1)
// and DX9 (2), which keep running the game's own render systems. Backend 3 falls back to DX9 when D3D12 cannot start.
//
// Replaced exe functions (none is inlined into its callers in the built exe; each caller calls it):
//   EnumerateRenderBackends   lists DirectX 12; called by CUIGroupGraphicsAPI::LoadBackends.
//   EnumerateRenderAdapters   backend 3 reuses the DX9 adapter list (one entry per display output, keyed by its GDI
//                             device name, which DeviceDX12 maps to the DXGI adapter driving that output).
//   EnumerateDisplayModes     backend 3 reuses the DX9 modes of the adapter.
//   RenderAdapter::Matches    backends 2 and 3 share adapter identities.
//   SelectedRenderAdapter     backend 3 reads the DX9 device globals.
//   RenderSystemHolder::Initialize
//   CUIGameSettingsUI::UpdateApplyButton
//                             CUIGraphicsPage::ApplySettings stores the adapter only for backends 1 and 2; its one caller
//                             (the Apply button handler) calls UpdateApplyButton right after it, which stores it for 3.
//   CSession::GetOptionInteger / WriteOptionInteger
//                             RENDERSYSTEM 3 is kept in plugins\RendererDX12.ini; the game's own option file says 2, so
//                             without this plugin the game starts on its DX9 renderer instead of failing on backend 3.
#include "RoPlugin.h"

#include "RenderSystemDX12.h"

#include "Client/Session/Session.h"
#include "File/File.h"
#include "Render/DX7/RenderSystemDX7.h"
#include "Render/DX9/RenderSystemDX9.h"
#include "Render/DeviceContext/DCSurface.h"
#include "Render/Device/DisplayMode.h"
#include "Render/Device/DeviceSelection.h"
#include "Render/Device/RenderAdapter.h"
#include "Render/Device/RenderBackends.h"
#include "Script/LuaScript.h"
#include "UI/Buttons/UIRadioButton.h"
#include "UI/Events/UIRoulletteWnd.h"
#include "UI/GameSettings/GameSettingsUI.h"
#include "UI/Lists/UIVScrollBar.h"
#include "UI/Window/UIControl.h"

#include <cstdio>
#include <cstring>
#include <new>

namespace {
  const RoHostApi* g_host;

  constexpr int BackendDX12 = 3;

  bool IsDX9Family(int backend) {
    return backend == 2 || backend == BackendDX12;
  }

  // plugins\RendererDX12.ini, next to this DLL.
  const char* ConfigPath() {
    static char path[MAX_PATH];
    if (!path[0]) {
      HMODULE self = nullptr;
      GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         reinterpret_cast<LPCSTR>(&ConfigPath), &self);
      const DWORD length = GetModuleFileNameA(self, path, MAX_PATH);
      char* dot = std::strrchr(path, '.');
      if (length == 0 || length >= MAX_PATH || !dot) {
        path[0] = '\0';
        return nullptr;
      }
      strcpy_s(dot, path + MAX_PATH - dot, ".ini");
    }
    return path;
  }

  std::vector<RenderBackend> EnumerateRenderBackendsDX12() {
    std::vector<RenderBackend> backends;
    backends.push_back({1, "DirectX 7"});
    backends.push_back({2, "DirectX 9"});
    backends.push_back({BackendDX12, "DirectX 12"});
    return backends;
  }

  std::vector<RenderAdapter> EnumerateRenderAdaptersDX12(int backend) {
    switch (backend) {
    case 1:
      return EnumerateAdaptersDX7();
    case 2:
      return EnumerateAdaptersDX9();
    case BackendDX12: {
      std::vector<RenderAdapter> adapters = EnumerateAdaptersDX9();
      for (RenderAdapter& adapter : adapters) {
        adapter.backend = BackendDX12;
      }
      return adapters;
    }
    default:
      return {};
    }
  }

  std::vector<DisplayMode> EnumerateDisplayModesDX12(int backend, unsigned deviceIndex) {
    switch (backend) {
    case 1:
      return EnumerateDisplayModesDX7(deviceIndex);
    case 2:
    case BackendDX12:
      return EnumerateDisplayModesDX9(deviceIndex);
    default:
      return {};
    }
  }

  RenderAdapter SelectedRenderAdapterDX12() {
    RenderAdapter selected{};
    selected.backend = g_configuredRenderBackend;
    if (selected.backend == 0) {
      selected.backend = g_session.DefaultRenderBackend() == 1 ? 1 : 2;
    }

    switch (selected.backend) {
    case 1:
      std::memcpy(&selected.drawGuid, g_guidDriver, sizeof(GUID));
      std::memcpy(&selected.renderGuid, g_guidDevice, sizeof(GUID));
      break;
    case 2:
    case BackendDX12:
      selected.deviceGuid = g_configuredDeviceGuid;
      selected.deviceName = g_configuredDeviceName;
      break;
    }
    return selected;
  }

  // The replacements below stand in for __thiscall members: same layout, same calling convention.
  struct RenderAdapterDX12 : RenderAdapter {
    // A DX9 and a DX12 entry for the same output match, so the fallback DX9 system and the settings UI find the
    // adapter picked under the other backend.
    bool Matches(const RenderAdapter& selected) const {
      if (this->backend != selected.backend && !(IsDX9Family(this->backend) && IsDX9Family(selected.backend))) {
        return false;
      }

      switch (this->backend) {
      case 1:
        return std::memcmp(&this->drawGuid, &selected.drawGuid, sizeof(GUID)) == 0 && std::memcmp(&this->renderGuid, &selected.renderGuid, sizeof(GUID)) == 0;
      case 2:
      case BackendDX12:
        return std::memcmp(&this->deviceGuid, &selected.deviceGuid, sizeof(GUID)) == 0 && this->deviceName == selected.deviceName;
      default:
        return false;
      }
    }
  };

  struct RenderSystemHolderDX12 : RenderSystemHolder {
    HRESULT Initialize(int backend, HWND window) {
      if (this->system != nullptr) {
        return 0x8000000e;
      }

      if (backend == 0) {
        backend = g_session.DefaultRenderBackend() == 1 ? 1 : 2;
      }

      CRenderSystemBase* renderSystem;
      switch (backend) {
      case 1:
        renderSystem = new CRenderSystemDX7();
        break;
      case 2:
        renderSystem = new CRenderSystemDX9();
        break;
      case BackendDX12: {
        auto* dx12 = new (std::nothrow) RenderSystemDX12();
        if (dx12) {
          const HRESULT result = dx12->Initialize(window);
          if (SUCCEEDED(result)) {
            g_host->Log("RendererDX12: using Direct3D 12");
            this->system = dx12;
            return result;
          }
          g_host->Log("RendererDX12: Direct3D 12 initialization failed (0x%08X), using Direct3D 9", static_cast<unsigned>(result));
          delete dx12;
        }
        renderSystem = new CRenderSystemDX9();
        break;
      }
      default:
        return 0x80070057;
      }

      this->system = renderSystem;
      return renderSystem->Initialize(window);
    }
  };

  struct GameSettingsUIDX12 : CUIGameSettingsUI {
    void UpdateApplyButton() {
      GameSettingsUI::CUIGraphicsPage* page = this->m_graphicsPage;
      // After ApplySettings, m_adapter is the applied adapter; store it as ApplySettings does for backend 2.
      if (g_configuredRenderBackend == BackendDX12 && page->m_graphicsApi->m_backend == BackendDX12) {
        const RenderAdapter& adapter = page->m_graphics->m_adapter;
        g_configuredDeviceGuid = adapter.deviceGuid;
        strncpy_s(g_configuredDeviceName, sizeof(g_configuredDeviceName), adapter.deviceName.c_str(), _TRUNCATE);
      }

      const bool visible = this->m_tab->current && this->m_tab->current->geometry.id == 3 &&
                           (page->IsModified() || page->m_spriteDetailLevel->m_slider->position != g_effectDetailLevel ||
                            page->m_textureDetailLevel->m_slider->position != g_textureDetailLevel ||
                            page->m_trilinear->m_onButton->checked != static_cast<bool>(g_textureFiltering));
      this->m_applyButton->Show(visible);
    }
  };

  bool IsRenderSystemKey(const char* key) {
    return std::strcmp(key, "RENDERSYSTEM") == 0;
  }

  struct SessionDX12 : CSession {
    int GetOptionInteger(const char* key, int fallback) {
      int value = -1;
      this->m_hotkeyScript->Call("l_GetOptionValue", "s>d", key, &value);
      if (value == -1) {
        value = fallback;
      }
      // The game's file says 2 when the plugin's says 3; any other game value means it was changed without the plugin.
      if (value == 2 && IsRenderSystemKey(key)) {
        if (const char* path = ConfigPath(); path && GetPrivateProfileIntA("RendererDX12", "RENDERSYSTEM", 0, path) == BackendDX12) {
          return BackendDX12;
        }
      }
      return value;
    }

    void WriteOptionInteger(CFile& file, const char* key, int value) {
      if (IsRenderSystemKey(key)) {
        if (const char* path = ConfigPath()) {
          WritePrivateProfileStringA("RendererDX12", "RENDERSYSTEM", value == BackendDX12 ? "3" : nullptr, path);
        }
        if (value == BackendDX12) {
          value = 2;
        }
      }
      char line[200];
      const int length = std::snprintf(line, sizeof(line), "OptionInfoList[\"%s\"] = %d\r\n", key, value);
      file.Write(line, static_cast<unsigned>(length));
    }
  };

  template <class Function>
  void* FunctionAddress(Function* function) {
    return reinterpret_cast<void*>(function);
  }
}

RO_PLUGIN_EXPORT bool RoPlugin_Init(const RoHostApi* host) {
  g_host = host;
  constexpr const char* name = "RendererDX12";
  return host->Replace(FunctionAddress(&EnumerateRenderBackends), FunctionAddress(&EnumerateRenderBackendsDX12), name) &&
         host->Replace(FunctionAddress(&EnumerateRenderAdapters), FunctionAddress(&EnumerateRenderAdaptersDX12), name) &&
         host->Replace(FunctionAddress(&EnumerateDisplayModes), FunctionAddress(&EnumerateDisplayModesDX12), name) &&
         host->Replace(FunctionAddress(&SelectedRenderAdapter), FunctionAddress(&SelectedRenderAdapterDX12), name) &&
         host->Replace(ro::Address(&RenderAdapter::Matches), ro::Address(&RenderAdapterDX12::Matches), name) &&
         host->Replace(ro::Address(&RenderSystemHolder::Initialize), ro::Address(&RenderSystemHolderDX12::Initialize), name) &&
         host->Replace(ro::Address(&CUIGameSettingsUI::UpdateApplyButton), ro::Address(&GameSettingsUIDX12::UpdateApplyButton), name) &&
         host->Replace(ro::Address(&CSession::GetOptionInteger), ro::Address(&SessionDX12::GetOptionInteger), name) &&
         host->Replace(ro::Address(&CSession::WriteOptionInteger), ro::Address(&SessionDX12::WriteOptionInteger), name);
}
