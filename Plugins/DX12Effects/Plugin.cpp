// A soft, slowly pulsing glow along the screen edges, drawn by RendererDX12's frame callback: four pretransformed
// quads that fade from the edge inwards, alpha blended over the finished frame with depth off.
#include "RoPlugin.h"

#include "RendererDX12Api.h"

#include <cmath>
#include <cstdint>

#include <timeapi.h>

namespace {
  // D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_SPECULAR | D3DFVF_TEX1, the layout DeviceDX12::Draw reads.
  struct Vertex {
    float x, y, z, rhw;
    std::uint32_t diffuse;
    std::uint32_t specular;
    float u, v;
  };

  constexpr std::uint32_t glowRgb = 0x40A8FF;  // soft sky blue
  constexpr float bandFraction = 0.09f;        // of the shorter screen side

  void Corner(Vertex*& out, float x, float y, std::uint32_t alpha) {
    *out++ = {x, y, 0.0f, 1.0f, (alpha << 24) | glowRgb, 0, 0.0f, 0.0f};
  }

  // Two triangles; (x0, y0)-(x1, y0) is the edge side at `alpha`, the opposite side fades to 0.
  void Band(Vertex*& out, float ax, float ay, float bx, float by, float cx, float cy, float dx, float dy, std::uint32_t alpha) {
    Corner(out, ax, ay, alpha);
    Corner(out, bx, by, alpha);
    Corner(out, cx, cy, 0);
    Corner(out, ax, ay, alpha);
    Corner(out, cx, cy, 0);
    Corner(out, dx, dy, 0);
  }

  void DrawPulse(DeviceDX12& device, DeviceStateDX12& state, void*) {
    const float width = static_cast<float>(device.width);
    const float height = static_cast<float>(device.height);
    const float band = (width < height ? width : height) * bandFraction;
    const float seconds = static_cast<float>(timeGetTime() % 600000u) / 1000.0f;
    const float pulse = 0.5f + 0.5f * std::sin(seconds * 2.0f);  // ~3 s period
    const auto alpha = static_cast<std::uint32_t>(70.0f + 90.0f * pulse);

    Vertex vertices[24];
    Vertex* out = vertices;
    Band(out, 0, 0, width, 0, width, band, 0, band, alpha);                                // top
    Band(out, width, height, 0, height, 0, height - band, width, height - band, alpha);    // bottom
    Band(out, 0, height, 0, 0, band, 0, band, height, alpha);                              // left
    Band(out, width, 0, width, height, width - band, height, width - band, 0, alpha);      // right

    state.SetTexture(0, nullptr);
    state.SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
    state.SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    state.SetRenderState(D3DRS_FOGENABLE, FALSE);
    state.SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    state.SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    state.SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    state.SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    state.SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    state.SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
    state.SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    state.SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
    state.SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    state.SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
    state.SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    state.SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    device.Draw(D3DPT_TRIANGLELIST, 8, vertices, 24, sizeof(Vertex), nullptr, 0, false);
  }
}

RO_PLUGIN_EXPORT bool RoPlugin_Init(const RoHostApi* host) {
  if (host->version < 2) {
    host->Log("DX12Effects: host API %u is older than 2", host->version);
    return false;
  }
  if (!RendererDX12_AddFrameCallback(DrawPulse, nullptr)) {
    host->Log("DX12Effects: could not register the frame callback");
    return false;
  }
  host->Log("DX12Effects: edge pulse registered (RendererDX12 %s)", host->PluginVersion("RendererDX12"));
  return true;
}
