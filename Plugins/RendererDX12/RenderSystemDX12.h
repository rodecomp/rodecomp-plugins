#ifndef RAGEXE_PLUGIN_RENDERERDX12_RENDERSYSTEMDX12_H
#define RAGEXE_PLUGIN_RENDERERDX12_RENDERSYSTEMDX12_H

#include "Render/RenderSystemBase.h"

#include "DeviceDX12.h"
#include "SurfaceDX12.h"
#include "TextureDX12.h"

#include <memory>

// CRenderSystemDX9 on DeviceDX12; reports backend 2 so the game treats it as its DX9 backend.
class RenderSystemDX12 : public CRenderSystemBase {
public:
  RenderSystemDX12() = default;
  ~RenderSystemDX12() override;
  std::int32_t Backend() override;
  HRESULT Initialize(HWND window) override;
  HRESULT Close() override;
  bool IsWindowed() override;
  std::uint32_t Width() override;
  std::uint32_t Height() override;
  CRenderer* CreateRenderer() override;
  void DestroyRenderer(CRenderer* renderer) override;
  CSurface* CreateSurface(std::uint32_t width, std::uint32_t height) override;
  CTexture* CreateTexture(std::uint32_t width, std::uint32_t height, std::int32_t format, bool dynamic, bool renderTarget) override;
  CSurface* CreateSurfaceOrTexture(std::uint32_t width, std::uint32_t height) override;
  CTexture* BackBufferTexture() override;
  std::shared_ptr<CSurface> BackBufferSurface() override;
  void UpdateOption() override;
  void PrepareErrorDialog() override;
  void RestoreDisplayMode() override;
  HRESULT TestDevice() override;
  HRESULT Present(bool option) override;

private:
  DeviceDX12 m_device;
  bool m_fullscreen = false;
  std::unique_ptr<CTextureDX12> m_defaultTexture;
  std::shared_ptr<CSurface> m_backBuffer;
};

#endif
