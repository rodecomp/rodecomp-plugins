#include "RendererDX12.h"

#include "Client/Mode/CGameMode.h"
#include "Client/Mode/ModeMgr.h"
#include "Contents/World/World.h"
#include "Render/Primitives/LightmappedPrimitive.h"
#include "Render/Primitives/RenderPrimitive.h"
#include "Render/Primitives/TextureQuad.h"
#include "Render/Primitives/UIRPData.h"
#include "Render/Textures/TextureCache.h"
#include "Resource/Bitmap/BitmapResource.h"
#include "Resource/Bitmap/IJL15Runtime.h"
#include "Resource/Bitmap/TextureNames.h"
#include "UI/Window/UIWindowMgr.h"

#include <algorithm>
#include <bit>
#include <cstdio>
#include <functional>
#include <memory>
#include <new>
#include <timeapi.h>

namespace {
  DWORD FtoDW(float value) {
    return std::bit_cast<DWORD>(value);
  }

  class ScreenshotOverlay : public UIRPData {
  public:
    const std::list<RenderPrimitive*>& Primitives() const {
      return this->m_primitives;
    }
  };

  UINT PrimitiveCount(D3DPRIMITIVETYPE type, UINT count) {
    return count / (type == D3DPT_TRIANGLELIST ? 3 : 2);
  }

  // 24-bit bottom-up BMP of rows x rectangle, as D3DXSaveSurfaceToFile(D3DXIFF_BMP).
  bool WriteBitmap(const char* filename, const std::uint32_t* pixels, UINT pitch, const RECT& area) {
    const LONG width = area.right - area.left;
    const LONG height = area.bottom - area.top;
    const DWORD rowSize = (static_cast<DWORD>(width) * 3 + 3) & ~3u;
    std::FILE* file = nullptr;
    if (fopen_s(&file, filename, "wb") != 0 || !file) {
      return false;
    }

    BITMAPFILEHEADER header{};
    header.bfType = 0x4D42;
    header.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
    header.bfSize = header.bfOffBits + rowSize * height;
    BITMAPINFOHEADER information{};
    information.biSize = sizeof(information);
    information.biWidth = width;
    information.biHeight = height;
    information.biPlanes = 1;
    information.biBitCount = 24;
    information.biCompression = BI_RGB;
    information.biSizeImage = rowSize * height;
    bool written = std::fwrite(&header, sizeof(header), 1, file) == 1 && std::fwrite(&information, sizeof(information), 1, file) == 1;

    std::vector<std::uint8_t> row(rowSize, 0);
    for (LONG y = height - 1; written && y >= 0; --y) {
      const std::uint32_t* source = pixels + static_cast<std::size_t>(area.top + y) * pitch + area.left;
      for (LONG x = 0; x < width; ++x) {
        row[x * 3 + 0] = static_cast<std::uint8_t>(source[x]);
        row[x * 3 + 1] = static_cast<std::uint8_t>(source[x] >> 8);
        row[x * 3 + 2] = static_cast<std::uint8_t>(source[x] >> 16);
      }
      written = std::fwrite(row.data(), rowSize, 1, file) == 1;
    }

    return std::fclose(file) == 0 && written;
  }

  // JPEG through the exe's ijl15.dll, as the DX7 renderer saves screenshots.
  bool WriteJpeg(const char* filename, const std::uint32_t* pixels, UINT pitch, const RECT& area) {
    using Init = int(__stdcall*)(IJL15Properties*);
    using Write = int(__stdcall*)(IJL15Properties*, int);
    using Free = int(__stdcall*)(IJL15Properties*);
    HMODULE library = GetModuleHandleA("ijl15.dll");
    if (!library) {
      return false;
    }
    const auto init = reinterpret_cast<Init>(GetProcAddress(library, "ijlInit"));
    const auto write = reinterpret_cast<Write>(GetProcAddress(library, "ijlWrite"));
    const auto free = reinterpret_cast<Free>(GetProcAddress(library, "ijlFree"));
    if (!init || !write || !free) {
      return false;
    }

    const LONG width = area.right - area.left;
    const LONG height = area.bottom - area.top;
    const LONG rowSize = (width * 3 + 3) & ~3;
    std::vector<std::uint8_t> dib(static_cast<std::size_t>(rowSize) * height, 0);
    for (LONG y = 0; y < height; ++y) {
      const std::uint32_t* source = pixels + static_cast<std::size_t>(area.top + y) * pitch + area.left;
      std::uint8_t* output = dib.data() + static_cast<std::size_t>(y) * rowSize;
      for (LONG x = 0; x < width; ++x) {
        output[x * 3 + 0] = static_cast<std::uint8_t>(source[x]);
        output[x * 3 + 1] = static_cast<std::uint8_t>(source[x] >> 8);
        output[x * 3 + 2] = static_cast<std::uint8_t>(source[x] >> 16);
      }
    }

    auto properties = std::make_unique<IJL15Properties>();
    SecureZeroMemory(properties.get(), sizeof(IJL15Properties));
    if (init(properties.get()) != 0) {
      return false;
    }
    properties->dibBytes = dib.data();
    properties->dibWidth = width;
    properties->dibHeight = height;
    properties->dibPadding = rowSize - width * 3;
    properties->dibChannels = 3;
    properties->dibColor = 2;
    properties->jpegFile = filename;
    properties->jpegWidth = width;
    properties->jpegHeight = height;
    const bool written = write(properties.get(), 8) == 0;
    free(properties.get());
    return written;
  }
}

CRendererDX12::CRendererDX12(DeviceDX12& owner) : device(&owner) {
}

CRendererDX12::~CRendererDX12() = default;

void CRendererDX12::SetCamera(const float*, const float*, const float*) {
}

// DX9 sets fixed-function light 0, which pre-transformed (XYZRHW) vertices never use.
void CRendererDX12::SetLighting(const float*, const float*, const float*) {
}

void CRendererDX12::SetClearColor(unsigned color) {
  this->clearColor = color;
}

float CRendererDX12::FPS() const {
  return this->fps;
}

void CRendererDX12::ClearDepth(int clearColor) {
  this->device->Clear(clearColor != 0, true, 0xFF000000u);
}

void CRendererDX12::ClearWorld() {
  unsigned color = this->clearColor;
  if (this->fog.enabled == 1 && static_cast<CGameMode*>(g_modeMgr.GetGameMode())->scene->backgroundFog == 1) {
    color = this->BlendColor(this->clearColor, g_worldOverlayColor);
  }

  this->device->Clear(true, true, color);
}

void CRendererDX12::Draw(
  D3DPRIMITIVETYPE type, const void* vertices, int vertexCount, UINT stride, const std::uint16_t* indices, UINT indexCount, bool lightmapped
) {
  if (indexCount) {
    this->device->Draw(type, PrimitiveCount(type, indexCount), vertices, vertexCount, stride, indices, indexCount, lightmapped);
  } else {
    this->device->Draw(type, PrimitiveCount(type, vertexCount), vertices, vertexCount, stride, nullptr, 0, lightmapped);
  }
}

void CRendererDX12::DrawPrimitive(RenderCommand* primitive) {
  if (primitive->vertexCount <= 0) {
    return;
  }

  this->Draw(
    static_cast<D3DPRIMITIVETYPE>(primitive->primitiveType), primitive->vertices, primitive->vertexCount, sizeof(RenderVertex), primitive->indices,
    primitive->indexCount, false
  );
}

void CRendererDX12::DrawLightmappedPrimitive(LightmappedPrimitive* primitive) {
  if (primitive->vertexCount <= 0) {
    return;
  }

  this->Draw(
    static_cast<D3DPRIMITIVETYPE>(primitive->primitiveType), primitive->vertices, primitive->vertexCount, sizeof(LightmappedVertex),
    primitive->indices, primitive->indexCount, true
  );
}

void CRendererDX12::DrawAuxiliaryPrimitive(LightmappedCommand* primitive) {
  if (primitive->vertexCount <= 0) {
    return;
  }

  this->Draw(
    static_cast<D3DPRIMITIVETYPE>(primitive->primitiveType), primitive->vertices, primitive->vertexCount, sizeof(LightmappedVertex),
    primitive->indices, primitive->indexCount, true
  );
}

static bool operator<(const DepthPrimitive& left, const DepthPrimitive& right) {
  if (left.depth < right.depth) {
    return true;
  }

  if (right.depth < left.depth) {
    return false;
  }

  return left.depth == right.depth && std::less<RenderCommand*>{}(left.primitive, right.primitive);
}

void CRendererDX12::DrawDepthQueue(bool sort, std::vector<DepthPrimitive>& queue) {
  if (sort) {
    std::sort(queue.begin(), queue.end());
  }

  for (const DepthPrimitive& entry : queue) {
    RenderCommand* command = entry.primitive;
    this->device->state.SetTexture(0, static_cast<CTextureDX12*>(command->texture));
    this->DrawPrimitive(command);
  }
}

void CRendererDX12::SetFog(int enabled) {
  // Fog is computed per vertex from the specular alpha in the shader, which D3DPRASTERCAPS_FOGVERTEX reports.
  this->fog.dirty = 1;
  this->fog.supported = 1;
  this->fog.enabled = enabled;
  g_fogRange = 1;
  g_fogVertexMode = D3DFOG_LINEAR;
}

// DX9 also sets world/view/projection transforms, which have no effect on pre-transformed vertices.
void CRendererDX12::ApplyFog() {
  if (this->fog.dirty == 1) {
    const float start = g_depthStart * 1490.0f + 10.0f;
    const float end = g_depthEnd * 1490.0f + 10.0f;
    this->device->state.SetRenderState(D3DRS_FOGCOLOR, g_worldOverlayColor);
    this->device->state.SetRenderState(D3DRS_RANGEFOGENABLE, g_fogRange);
    this->device->state.SetRenderState(D3DRS_FOGSTART, FtoDW(start));
    this->device->state.SetRenderState(D3DRS_FOGEND, FtoDW(end));
    this->device->state.SetRenderState(D3DRS_FOGDENSITY, FtoDW(g_fogDensity));
    this->device->state.SetRenderState(D3DRS_FOGTABLEMODE, D3DFOG_NONE);
    this->device->state.SetRenderState(D3DRS_FOGVERTEXMODE, g_fogVertexMode);
    this->fog.dirty = 0;
  }

  this->device->state.SetRenderState(D3DRS_FOGENABLE, this->fog.enabled);
}

bool CRendererDX12::Draw() {
  this->device->state.Begin();
  this->DrawQueues();
  this->ClearFrame();
  this->device->state.End();

  return true;
}

HRESULT CRendererDX12::Present(int) {
  ++this->frame;
  ++this->frames;
  const unsigned now = timeGetTime();
  const unsigned elapsed = now - this->lastTick;

  if (elapsed >= 1000) {
    const unsigned count = this->frames;
    this->frames = 0;
    this->lastTick = now;
    this->fps = static_cast<float>(count) / (static_cast<float>(elapsed) / 1000.0f);
  }

  static constexpr char source[] = "RendererDX12.cpp";
  g_renderDiagnosticLine = __LINE__;
  g_renderDiagnosticFile = source;
  const HRESULT result = this->device->Present();
  g_renderDiagnosticLine = __LINE__;
  g_renderDiagnosticFile = source;

  return result;
}

bool CRendererDX12::CaptureBackBuffer() {
  this->m_captured = this->device->ReadScene();
  return !this->m_captured.empty();
}

bool CRendererDX12::RestoreBackBuffer() {
  if (this->m_captured.size() != static_cast<std::size_t>(this->device->width) * this->device->height) {
    this->m_captured.clear();
    return false;
  }

  this->device->WriteScene(this->m_captured.data());
  return true;
}

bool CRendererDX12::RenderToTexture(int width, int height, const std::list<RenderPrimitive*>& primitives, CTexture*& texture) {
  CTextureDX12* target;
  if (texture) {
    target = static_cast<CTextureDX12*>(texture);
  } else {
    target = new (std::nothrow) CTextureDX12(*this->device);
    if (!target) {
      return false;
    }

    if (!target->CreateRenderTarget(width, height)) {
      delete target;
      return false;
    }

    texture = target;
  }

  GpuResource* resource = target->Resource();
  if (!resource) {
    return false;
  }

  this->device->SetRenderTarget(resource);
  {
    ScopedDeviceStateDX12 restore(this->device->state);
    this->device->state.SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
    this->device->state.SetRenderState(D3DRS_FOGENABLE, 0);
    this->device->state.SetRenderState(D3DRS_ALPHAREF, 1);
    this->device->state.SetRenderState(D3DRS_ALPHATESTENABLE, 1);
    this->device->state.SetRenderState(D3DRS_ALPHABLENDENABLE, 1);
    this->device->Clear(true, false, 0);

    for (const auto& primitive : primitives) {
      this->device->state.SetTexture(0, static_cast<CTextureDX12*>(primitive->texture));
      this->DrawPrimitive(primitive);
    }
  }
  this->device->SetRenderTarget(nullptr);

  return true;
}

bool CRendererDX12::SaveScreenshot(const char* filename, int format, CTexture* overlay, const tagRECT* rectangle) {
  if (overlay) {
    unsigned x = rectangle ? static_cast<unsigned>(rectangle->right) : this->device->width;
    unsigned y = rectangle ? static_cast<unsigned>(rectangle->bottom) : this->device->height;
    x -= overlay->Width();
    y -= overlay->Height();
    ScreenshotOverlay drawing;
    drawing.Draw(x, y, overlay, 0xFFFFFFFFu, 0, 0, false);

    this->device->state.Begin();
    {
      ScopedDeviceStateDX12 restore(this->device->state);
      this->device->state.SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
      this->device->state.SetRenderState(D3DRS_FOGENABLE, 0);
      this->device->state.SetRenderState(D3DRS_ALPHAREF, 1);
      this->device->state.SetRenderState(D3DRS_ALPHATESTENABLE, 1);
      this->device->state.SetRenderState(D3DRS_ALPHABLENDENABLE, 1);

      for (RenderPrimitive* command : drawing.Primitives()) {
        this->device->state.SetTexture(0, static_cast<CTextureDX12*>(command->texture));
        this->DrawPrimitive(command);
      }
    }
    this->device->state.End();
  }

  const std::vector<std::uint32_t> pixels = this->device->ReadScene();
  const LONG width = static_cast<LONG>(this->device->width);
  const LONG height = static_cast<LONG>(this->device->height);
  if (pixels.size() != static_cast<std::size_t>(width) * height) {
    return false;
  }

  RECT area = rectangle ? *rectangle : RECT{0, 0, width, height};
  area.left = std::clamp<LONG>(area.left, 0, width);
  area.top = std::clamp<LONG>(area.top, 0, height);
  area.right = std::clamp<LONG>(area.right, area.left, width);
  area.bottom = std::clamp<LONG>(area.bottom, area.top, height);
  if (area.right == area.left || area.bottom == area.top) {
    return false;
  }

  // D3DXIFF_BMP = 0, D3DXIFF_JPG = 1.
  return format == 1 ? WriteJpeg(filename, pixels.data(), width, area) : WriteBitmap(filename, pixels.data(), width, area);
}

bool CRendererDX12::SaveBitmapResource(const char* filename, int format, CBitmapRes* bitmap) {
  CTexture* overlay = nullptr;

  if (bitmap) {
    const auto& image = bitmap->image;
    overlay = g_texMgr.Upload(image.width, image.height, image.pixels, image.alpha != 0, 0, false, false, false);
  }

  const bool saved = this->SaveScreenshot(filename, format, overlay, nullptr);
  delete overlay;
  return saved;
}

int CRendererDX12::SaveBitmap(const char* filename, CBitmapRes* overlay) {
  return this->SaveBitmapResource(filename, 0, overlay);
}

int CRendererDX12::SaveJpeg(const char* filename, CBitmapRes* overlay) {
  return this->SaveBitmapResource(filename, 1, overlay);
}

void CRendererDX12::CleanupTextures() {
}

bool CRendererDX12::SaveLogo(std::string filename) {
  CTexture* overlay = g_texMgr.Load(Resolve("scr_logo.bmp"), 0, false, true, false);

  return this->SaveScreenshot(filename.c_str(), 1, overlay, nullptr);
}

int CRendererDX12::LockBackBuffer() {
  this->m_lockedBackBuffer = this->device->ReadScene();
  if (this->m_lockedBackBuffer.empty()) {
    return 0;
  }

  this->backBuffer.pixels = this->m_lockedBackBuffer.data();
  this->backBuffer.pitch = static_cast<int>(this->device->width * 4);
  this->backBuffer.bitsPerPixel = 32;
  return 1;
}

HRESULT CRendererDX12::UnlockBackBuffer() {
  if (this->m_lockedBackBuffer.empty()) {
    return D3DERR_INVALIDCALL;
  }

  this->device->WriteScene(this->m_lockedBackBuffer.data());
  this->m_lockedBackBuffer.clear();
  this->backBuffer.pixels = nullptr;
  return D3D_OK;
}
