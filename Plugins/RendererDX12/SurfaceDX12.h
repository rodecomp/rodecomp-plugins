#ifndef RAGEXE_PLUGIN_RENDERERDX12_SURFACEDX12_H
#define RAGEXE_PLUGIN_RENDERERDX12_SURFACEDX12_H

#include "Render/Textures/Surface.h"

#include "DeviceDX12.h"

#include <cstdint>
#include <vector>

// CSurfaceDX9's two kinds: an off-screen X8R8G8B8 surface (CPU pixels mirrored in a GPU texture so Draw/Stretch can
// blit it onto the scene), or the back buffer itself (the scene, read back for Lock/GetDC and written on release).
class CSurfaceDX12 : public CSurface {
public:
  CSurfaceDX12(DeviceDX12& device, int width, int height);
  // The back buffer.
  explicit CSurfaceDX12(DeviceDX12& device);
  ~CSurfaceDX12() override;
  std::uint32_t Width() override;
  std::uint32_t Height() override;
  bool Resize(int width, int height) override;
  SurfaceLock Lock() override;
  void Unlock() override;
  bool GetDC(HDC* output) override;
  void ReleaseDC(HDC dc) override;
  void Upload(int x, int y, int width, int height, const std::uint32_t* pixels, int option, int sourceWidth) override;
  void Fill(const RECT* rectangle, std::uint32_t color) override;
  void Draw(int x, int y, int width, int height, std::uint32_t color) override;
  void Stretch(int x, int y, int width, int height) override;

private:
  bool Allocate();
  void Release();
  // Makes m_pixels current (reads the scene for the back buffer).
  bool Acquire();
  // Publishes rectangle of m_pixels to the GPU.
  void Commit(const RECT& rectangle);
  RECT Bounds() const;
  RECT ClipToScene(const RECT& rectangle) const;
  void FreeDib();

  DeviceDX12* m_device;
  bool m_backBuffer;
  std::uint32_t m_width = 0;
  std::uint32_t m_height = 0;
  GpuResource m_gpu;
  std::vector<std::uint32_t> m_pixels;
  bool m_locked = false;
  HDC m_dc = nullptr;
  HBITMAP m_dib = nullptr;
  HGDIOBJ m_previousBitmap = nullptr;
  std::uint32_t* m_dibBits = nullptr;
};

#endif
