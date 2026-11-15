// Replaces RenderSystemHolder::Initialize so the DX9 backend selection runs on D3D12, falling back to the game's
// own DX9 render system when D3D12 cannot start.
#include "RoPlugin.h"

#include "RenderSystemDX12.h"

#include "Client/Session/Session.h"
#include "Render/DX7/RenderSystemDX7.h"
#include "Render/DX9/RenderSystemDX9.h"
#include "Render/DeviceContext/DCSurface.h"

#include <new>

namespace {
  const RoHostApi* g_host;

  // Stands in for RenderSystemHolder::Initialize (a __thiscall member): same layout, same calling convention.
  struct RenderSystemHolderDX12 : RenderSystemHolder {
    HRESULT Initialize(int backend, HWND window) {
      if (this->system != nullptr) {
        return 0x8000000e;
      }

      if (backend == 0) {
        switch (g_session.DefaultRenderBackend()) {
        case 1:
          backend = 1;
          break;
        default:
          backend = 2;
          break;
        }
      }

      CRenderSystemBase* renderSystem;
      switch (backend) {
      case 1:
        renderSystem = new CRenderSystemDX7();
        break;
      case 2: {
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
}

RO_PLUGIN_EXPORT bool RoPlugin_Init(const RoHostApi* host) {
  g_host = host;
  return host->Replace(ro::Address(&RenderSystemHolder::Initialize), ro::Address(&RenderSystemHolderDX12::Initialize), "RendererDX12");
}
