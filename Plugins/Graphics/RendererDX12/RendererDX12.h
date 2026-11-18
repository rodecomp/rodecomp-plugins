#ifndef RAGEXE_PLUGIN_RENDERERDX12_RENDERERDX12_H
#define RAGEXE_PLUGIN_RENDERERDX12_RENDERERDX12_H

#include "Render/Renderer.h"

#include "DeviceDX12.h"
#include "TextureDX12.h"

#include <list>
#include <vector>

// CRendererDX9 on DeviceDX12. The CRenderer base is untouched: the game calls
// static_cast<CRendererDX9*>(g_renderer)->Submit(...), which only uses base queues and fog.
class CRendererDX12 : public CRenderer {
public:
  explicit CRendererDX12(DeviceDX12& device);
  ~CRendererDX12() override;
  void SetCamera(const float* eye, const float* target, const float* up) override;
  void SetLighting(const float* direction, const float* diffuse, const float* ambient) override;
  void SetClearColor(unsigned color) override;
  void SetFog(int enabled) override;
  void ClearDepth(int clearColor) override;
  void ClearWorld() override;
  bool Draw() override;
  void Present(bool option) override;
  float FPS() const override;
  void CaptureBackBuffer() override;
  void RestoreBackBuffer() override;
  void DrawAuxiliaryPrimitive(LightmappedCommand* primitive) override;
  void DrawLightmappedPrimitive(LightmappedPrimitive* primitive) override;
  void DrawPrimitive(RenderCommand* primitive) override;
  bool RenderToTexture(int width, int height, const std::list<RenderCommand*>& primitives, CTexture*& texture) override;
  int SaveBitmap(const char* filename, CBitmapRes* overlay) override;
  int SaveJpeg(const char* filename, CBitmapRes* overlay) override;
  void CleanupTextures() override;
  bool SaveLogo(std::string filename) override;
  int LockBackBuffer() override;
  HRESULT UnlockBackBuffer() override;

  void SetTextureMode(int mode);
  void DrawDepthQueue(bool sort, std::vector<DepthPrimitive>& queue);
  void DrawOpaqueQueues();
  void DrawGroundQueues();
  void DrawAlphaQueues();
  void DrawAuxiliaryQueue();
  void DrawBaseQueue();
  void DrawQueues();
  void ApplyFog();
  bool SaveScreenshot(const char* filename, int format, CTexture* overlay, const tagRECT* rectangle);

  DeviceDX12* device;
  unsigned clearColor = 0;
  unsigned lastTick = 0;
  unsigned frames = 0;
  float fps = 0.0f;

private:
  bool SaveBitmapResource(const char* filename, int format, CBitmapRes* overlay);
  void Draw(D3DPRIMITIVETYPE type, const void* vertices, int vertexCount, UINT stride, const std::uint16_t* indices, UINT indexCount, bool lightmapped);

  std::vector<std::uint32_t> m_captured;
  std::vector<std::uint32_t> m_lockedBackBuffer;
};

#endif
