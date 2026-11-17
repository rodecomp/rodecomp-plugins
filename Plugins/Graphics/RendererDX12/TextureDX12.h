#ifndef RAGEXE_PLUGIN_RENDERERDX12_TEXTUREDX12_H
#define RAGEXE_PLUGIN_RENDERERDX12_TEXTUREDX12_H

#include "Render/Textures/Texture.h"

#include "DeviceDX12.h"

#include <cstdint>
#include <vector>

// A CPU copy of the texture in its native D3D9 format (16 or 32 bits per pixel) is the image Lock, GetDC and the
// upload paths edit; every edit converts the touched rectangle to BGRA8 and sends it to the GPU texture right away.
// Render targets (RenderToTexture) live on the GPU only and, like DX9 default-pool render targets, cannot be locked.
class CTextureDX12 : public CTexture {
public:
  explicit CTextureDX12(DeviceDX12& device);
  CTextureDX12(DeviceDX12& device, int width, int height, int format, bool dynamic, bool renderTarget);
  ~CTextureDX12() override;
  bool Resize(int width, int height) override;
  SurfaceLock Lock() override;
  void Unlock() override;
  bool GetDC(HDC* output) override;
  void ReleaseDC(HDC dc) override;
  void Upload(int x, int y, int width, int height, const std::uint32_t* pixels, int option, int sourceWidth) override;
  void Fill(const RECT* rectangle, std::uint32_t color) override;
  void Update(const RECT* rectangle) override;
  void UploadSprite(int x, int y, int width, int height, const SpriteRleImage* image, const std::uint32_t* palette) override;
  bool CopyTexture(CTexture* source, int sourceX, int sourceY, int sourceWidth, int sourceHeight, int x, int y, int width, int height) override;
  bool Create(int width, int height, int format) override;
  // An exact-size A8R8G8B8 render target, as RenderToTexture's D3DUSAGE_RENDERTARGET texture.
  bool CreateRenderTarget(int width, int height);

  // The GPU texture, or nullptr when none exists (DeviceDX12 then samples white).
  GpuResource* Resource() {
    return this->m_gpu.resource ? &this->m_gpu : nullptr;
  }

  bool IsRenderTarget() const {
    return this->m_renderTarget;
  }

private:
  void Release();
  int BitsPerPixel() const;
  std::uint8_t* Row(UINT y);
  bool Clip(RECT& rectangle) const;
  // Converts rectangle of the CPU copy to BGRA8 and uploads it.
  void Commit(const RECT& rectangle);
  void CommitAll();
  bool UploadCopy(int x, int y, int width, int height, const std::uint32_t* pixels, std::uint8_t blackTransparent, int sourceWidth);
  void FreeDib();

  DeviceDX12* m_device;
  bool m_allowDynamic = false;
  bool m_renderTarget = false;
  bool m_locked = false;
  GpuResource m_gpu;
  std::vector<std::uint8_t> m_pixels;
  UINT m_pitch = 0;
  std::vector<std::uint32_t> m_scratch;
  HDC m_dc = nullptr;
  HBITMAP m_dib = nullptr;
  HGDIOBJ m_previousBitmap = nullptr;
  std::uint32_t* m_dibBits = nullptr;
};

#endif
