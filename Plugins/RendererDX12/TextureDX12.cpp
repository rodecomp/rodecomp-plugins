#include "TextureDX12.h"

#include "Render/Renderer.h"
#include "Render/Primitives/TextureQuad.h"
#include "Render/Textures/SpritePixels.h"

#include <algorithm>
#include <cstring>

namespace {
  constexpr UINT MaxDimension = D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION;

  D3DFORMAT NativeFormat(int format) {
    switch (format) {
    case TextureFormat::A1R5G5B5:
      return D3DFMT_A1R5G5B5;
    case TextureFormat::A4R4G4B4:
      return D3DFMT_A4R4G4B4;
    case TextureFormat::R5G6B5:
      return D3DFMT_R5G6B5;
    case TextureFormat::X1R5G5B5:
      return D3DFMT_X1R5G5B5;
    case TextureFormat::A8R8G8B8:
      return D3DFMT_A8R8G8B8;
    default:
      return D3DFMT_UNKNOWN;
    }
  }

  int FormatBitsPerPixel(int format) {
    switch (format) {
    case TextureFormat::A1R5G5B5:
    case TextureFormat::A4R4G4B4:
    case TextureFormat::R5G6B5:
    case TextureFormat::X1R5G5B5:
      return 16;
    case TextureFormat::A8R8G8B8:
      return 32;
    default:
      return 0;
    }
  }

  UINT RoundUp(UINT value) {
    --value;
    value |= value >> 1;
    value |= value >> 2;
    value |= value >> 4;
    value |= value >> 8;
    value |= value >> 16;
    ++value;
    return std::min<UINT>(value + (value == 0), MaxDimension);
  }

  std::uint16_t PackA1R5G5B5(std::uint32_t color) {
    return ((color >> 16) & 0x8000) | ((color & 255) >> 3) | ((color >> 6) & 0x3E0) | ((color >> 9) & 0x7C00);
  }

  std::uint16_t PackA4R4G4B4(std::uint32_t color) {
    return ((color >> 16) & 0xF000) | ((color >> 12) & 0xF00) | ((color >> 8) & 0xF0) | ((color >> 4) & 0xF);
  }

  std::uint16_t PackR5G6B5(std::uint32_t color) {
    return ((color >> 8) & 0xF800) | ((color >> 5) & 0x7E0) | ((color >> 3) & 0x1F);
  }

  // A8R8G8B8 -> the texture's native pixel.
  std::uint32_t FromArgb(std::uint32_t color, int format) {
    switch (format) {
    case TextureFormat::A1R5G5B5:
      return PackA1R5G5B5(color);
    case TextureFormat::X1R5G5B5:
      return PackA1R5G5B5(color) | 0x8000;
    case TextureFormat::A4R4G4B4:
      return PackA4R4G4B4(color);
    case TextureFormat::R5G6B5:
      return PackR5G6B5(color);
    case TextureFormat::A8R8G8B8:
      return color;
    default:
      return 0;
    }
  }

  std::uint32_t Expand5(std::uint32_t value) {
    return (value << 3) | (value >> 2);
  }

  std::uint32_t Expand6(std::uint32_t value) {
    return (value << 2) | (value >> 4);
  }

  // The texture's native pixel -> A8R8G8B8 (which is BGRA8 in memory).
  std::uint32_t ToArgb(std::uint32_t pixel, int format) {
    switch (format) {
    case TextureFormat::A1R5G5B5:
    case TextureFormat::X1R5G5B5: {
      const std::uint32_t alpha = format == TextureFormat::X1R5G5B5 || (pixel & 0x8000) ? 0xFF000000u : 0;
      return alpha | Expand5((pixel >> 10) & 31) << 16 | Expand5((pixel >> 5) & 31) << 8 | Expand5(pixel & 31);
    }
    case TextureFormat::A4R4G4B4:
      return ((pixel >> 12) & 15) * 0x11000000u | ((pixel >> 8) & 15) * 0x110000u | ((pixel >> 4) & 15) * 0x1100u | (pixel & 15) * 0x11u;
    case TextureFormat::R5G6B5:
      return 0xFF000000u | Expand5((pixel >> 11) & 31) << 16 | Expand6((pixel >> 5) & 63) << 8 | Expand5(pixel & 31);
    case TextureFormat::A8R8G8B8:
      return pixel;
    default:
      return 0;
    }
  }

  std::uint32_t ReadPixel(const std::uint8_t* row, UINT x, int bitsPerPixel) {
    if (bitsPerPixel == 16) {
      std::uint16_t value;
      std::memcpy(&value, row + x * 2, 2);
      return value;
    }
    std::uint32_t value;
    std::memcpy(&value, row + x * 4, 4);
    return value;
  }

  void WritePixel(std::uint8_t* row, UINT x, int bitsPerPixel, std::uint32_t value) {
    if (bitsPerPixel == 16) {
      const auto narrow = static_cast<std::uint16_t>(value);
      std::memcpy(row + x * 2, &narrow, 2);
    } else {
      std::memcpy(row + x * 4, &value, 4);
    }
  }

  std::uint32_t Lerp(std::uint32_t a, std::uint32_t b, float t) {
    std::uint32_t result = 0;
    for (int shift = 0; shift < 32; shift += 8) {
      const float first = static_cast<float>((a >> shift) & 255);
      const float second = static_cast<float>((b >> shift) & 255);
      result |= static_cast<std::uint32_t>(first + (second - first) * t + 0.5f) << shift;
    }
    return result;
  }
}

CTextureDX12::CTextureDX12(DeviceDX12& device) : m_device(&device) {
  this->m_textureWidth = this->m_width = 1;
  this->m_textureHeight = this->m_height = 1;
  this->m_format = TextureFormat::A1R5G5B5;
}

CTextureDX12::CTextureDX12(DeviceDX12& device, int width, int height, int format, bool dynamic, bool renderTarget) : CTextureDX12(device) {
  this->m_allowDynamic = dynamic;
  this->m_renderTarget = renderTarget;
  this->Create(width, height, format);
  this->SetImageSize(width, height);
}

CTextureDX12::~CTextureDX12() {
  this->FreeDib();
  this->Release();
}

void CTextureDX12::Release() {
  if (this->m_gpu.resource) {
    this->m_device->ReleaseTexture(this->m_gpu);
  }
  this->m_gpu = GpuResource{};
  this->m_pixels.clear();
  this->m_pixels.shrink_to_fit();
  this->m_pitch = 0;
}

void CTextureDX12::FreeDib() {
  if (this->m_dc) {
    SelectObject(this->m_dc, this->m_previousBitmap);
    DeleteDC(this->m_dc);
    this->m_dc = nullptr;
  }
  if (this->m_dib) {
    DeleteObject(this->m_dib);
    this->m_dib = nullptr;
  }
  this->m_dibBits = nullptr;
  this->m_previousBitmap = nullptr;
}

int CTextureDX12::BitsPerPixel() const {
  return FormatBitsPerPixel(this->m_format);
}

std::uint8_t* CTextureDX12::Row(UINT y) {
  return this->m_pixels.data() + static_cast<std::size_t>(y) * this->m_pitch;
}

bool CTextureDX12::Clip(RECT& rectangle) const {
  rectangle.left = std::clamp<LONG>(rectangle.left, 0, this->m_textureWidth);
  rectangle.top = std::clamp<LONG>(rectangle.top, 0, this->m_textureHeight);
  rectangle.right = std::clamp<LONG>(rectangle.right, rectangle.left, this->m_textureWidth);
  rectangle.bottom = std::clamp<LONG>(rectangle.bottom, rectangle.top, this->m_textureHeight);
  return rectangle.right > rectangle.left && rectangle.bottom > rectangle.top;
}

bool CTextureDX12::Create(int width, int height, int format) {
  this->FreeDib();
  this->Release();

  this->m_textureWidth = RoundUp(width);
  this->m_textureHeight = RoundUp(height);
  this->m_format = format;
  const int bitsPerPixel = FormatBitsPerPixel(format);
  if (!bitsPerPixel) {
    return false;
  }

  this->m_pitch = this->m_textureWidth * (bitsPerPixel / 8);
  this->m_pixels.assign(static_cast<std::size_t>(this->m_pitch) * this->m_textureHeight, 0);

  if (!this->m_device->CreateTexture(this->m_textureWidth, this->m_textureHeight, this->m_renderTarget, this->m_gpu)) {
    this->Release();
    return false;
  }

  this->CommitAll();
  return true;
}

bool CTextureDX12::Resize(int width, int height) {
  return this->Create(width, height, this->m_format);
}

bool CTextureDX12::CreateRenderTarget(int width, int height) {
  this->FreeDib();
  this->Release();
  this->m_renderTarget = true;
  this->m_format = TextureFormat::A8R8G8B8;
  this->m_textureWidth = this->m_width = width;
  this->m_textureHeight = this->m_height = height;
  return this->m_device->CreateTexture(width, height, true, this->m_gpu);
}

void CTextureDX12::Commit(const RECT& area) {
  RECT rectangle = area;
  if (!this->m_gpu.resource || this->m_pixels.empty() || !this->Clip(rectangle)) {
    return;
  }

  const UINT width = rectangle.right - rectangle.left;
  const UINT height = rectangle.bottom - rectangle.top;
  const int bitsPerPixel = this->BitsPerPixel();

  if (this->m_format == TextureFormat::A8R8G8B8) {
    const auto* pixels = reinterpret_cast<const std::uint32_t*>(this->Row(rectangle.top)) + rectangle.left;
    this->m_device->UploadTexture(this->m_gpu, pixels, this->m_textureWidth, rectangle.left, rectangle.top, width, height);
    return;
  }

  this->m_scratch.resize(static_cast<std::size_t>(width) * height);
  for (UINT y = 0; y < height; ++y) {
    const std::uint8_t* row = this->Row(rectangle.top + y);
    std::uint32_t* output = this->m_scratch.data() + static_cast<std::size_t>(y) * width;
    for (UINT x = 0; x < width; ++x) {
      output[x] = ToArgb(ReadPixel(row, rectangle.left + x, bitsPerPixel), this->m_format);
    }
  }
  this->m_device->UploadTexture(this->m_gpu, this->m_scratch.data(), width, rectangle.left, rectangle.top, width, height);
}

void CTextureDX12::CommitAll() {
  this->Commit(RECT{0, 0, static_cast<LONG>(this->m_textureWidth), static_cast<LONG>(this->m_textureHeight)});
}

SurfaceLock CTextureDX12::Lock() {
  SurfaceLock output{};
  // Render targets live in video memory only, as D3DPOOL_DEFAULT render targets do.
  if (this->m_renderTarget || this->m_pixels.empty() || this->m_locked || this->m_dc) {
    return output;
  }

  this->m_locked = true;
  output.pixels = this->m_pixels.data();
  output.pitch = static_cast<int>(this->m_pitch);
  output.width = this->m_textureWidth;
  output.height = this->m_textureHeight;
  output.success = true;
  output.bitsPerPixel = this->BitsPerPixel();
  return output;
}

void CTextureDX12::Unlock() {
  if (this->m_locked) {
    this->m_locked = false;
    this->CommitAll();
  }
}

bool CTextureDX12::GetDC(HDC* output) {
  if (this->m_renderTarget || this->m_pixels.empty() || this->m_locked || this->m_dc) {
    return false;
  }

  BITMAPINFO information{};
  information.bmiHeader.biSize = sizeof(information.bmiHeader);
  information.bmiHeader.biWidth = static_cast<LONG>(this->m_textureWidth);
  information.bmiHeader.biHeight = -static_cast<LONG>(this->m_textureHeight);
  information.bmiHeader.biPlanes = 1;
  information.bmiHeader.biBitCount = 32;
  information.bmiHeader.biCompression = BI_RGB;
  void* bits = nullptr;
  this->m_dib = CreateDIBSection(nullptr, &information, DIB_RGB_COLORS, &bits, nullptr, 0);
  if (!this->m_dib) {
    return false;
  }
  this->m_dc = CreateCompatibleDC(nullptr);
  if (!this->m_dc) {
    this->FreeDib();
    return false;
  }
  this->m_dibBits = static_cast<std::uint32_t*>(bits);
  this->m_previousBitmap = SelectObject(this->m_dc, this->m_dib);

  // GDI only writes RGB; the snapshot tells ReleaseDC which pixels it touched.
  const int bitsPerPixel = this->BitsPerPixel();
  const std::size_t count = static_cast<std::size_t>(this->m_textureWidth) * this->m_textureHeight;
  this->m_scratch.resize(count);
  for (UINT y = 0; y < this->m_textureHeight; ++y) {
    const std::uint8_t* row = this->Row(y);
    std::uint32_t* output = this->m_dibBits + static_cast<std::size_t>(y) * this->m_textureWidth;
    for (UINT x = 0; x < this->m_textureWidth; ++x) {
      output[x] = ToArgb(ReadPixel(row, x, bitsPerPixel), this->m_format) & 0xFFFFFFu;
    }
  }
  std::memcpy(this->m_scratch.data(), this->m_dibBits, count * sizeof(std::uint32_t));

  *output = this->m_dc;
  return true;
}

void CTextureDX12::ReleaseDC(HDC dc) {
  if (!this->m_dc || dc != this->m_dc) {
    return;
  }

  GdiFlush();
  const int bitsPerPixel = this->BitsPerPixel();
  RECT dirty{LONG(this->m_textureWidth), LONG(this->m_textureHeight), 0, 0};
  for (UINT y = 0; y < this->m_textureHeight; ++y) {
    std::uint8_t* row = this->Row(y);
    const std::size_t offset = static_cast<std::size_t>(y) * this->m_textureWidth;
    for (UINT x = 0; x < this->m_textureWidth; ++x) {
      const std::uint32_t color = this->m_dibBits[offset + x] & 0xFFFFFFu;
      if (color != this->m_scratch[offset + x]) {
        WritePixel(row, x, bitsPerPixel, FromArgb(0xFF000000u | color, this->m_format));
        dirty.left = std::min<LONG>(dirty.left, x);
        dirty.top = std::min<LONG>(dirty.top, y);
        dirty.right = std::max<LONG>(dirty.right, x + 1);
        dirty.bottom = std::max<LONG>(dirty.bottom, y + 1);
      }
    }
  }

  this->FreeDib();
  this->Commit(dirty);
}

void CTextureDX12::Update(const RECT*) {
}

void CTextureDX12::Fill(const RECT* rectangle, std::uint32_t color) {
  if (this->m_pixels.empty()) {
    return;
  }

  RECT area = rectangle ? *rectangle : RECT{0, 0, LONG(this->m_textureWidth), LONG(this->m_textureHeight)};
  if (!this->Clip(area)) {
    return;
  }

  const int bitsPerPixel = this->BitsPerPixel();
  const std::uint32_t value = FromArgb(color, this->m_format);
  for (LONG y = area.top; y < area.bottom; ++y) {
    std::uint8_t* row = this->Row(y);
    for (LONG x = area.left; x < area.right; ++x) {
      WritePixel(row, x, bitsPerPixel, value);
    }
  }
  this->Commit(area);
}

bool CTextureDX12::UploadCopy(int x, int y, int width, int height, const std::uint32_t* pixels, std::uint8_t blackTransparent, int sourceWidth) {
  const int stride = sourceWidth ? sourceWidth : width;
  int stepX = 1, stepY = 1;
  const int extentX = x + width;
  const int extentY = y + height;

  while (static_cast<UINT>(extentX / stepX) > this->m_width) {
    ++stepX;
  }

  while (static_cast<UINT>(extentY / stepY) > this->m_height) {
    ++stepY;
  }

  const int w = width / stepX, h = height / stepY;
  if (!w || !h) {
    return false;
  }

  RECT area{x / stepX, y / stepY, x / stepX + w, y / stepY + h};
  RECT clipped = area;
  if (!this->Clip(clipped)) {
    return false;
  }

  const int bitsPerPixel = this->BitsPerPixel();
  for (LONG row = clipped.top; row < clipped.bottom; ++row) {
    std::uint8_t* output = this->Row(row);
    const std::uint32_t* input = pixels + (row - area.top) * stepY * stride;

    for (LONG column = clipped.left; column < clipped.right; ++column) {
      const std::uint32_t color = input[(column - area.left) * stepX];
      std::uint32_t converted;

      switch (this->m_format) {
      case TextureFormat::A1R5G5B5:
      case TextureFormat::X1R5G5B5: {
        converted = PackA1R5G5B5(color);
        const std::uint32_t rgb = converted & 0x7FFF;
        if (rgb == 0x7C1F || (blackTransparent && !rgb)) {
          converted = 0;
        } else if (this->m_format == TextureFormat::X1R5G5B5) {
          converted |= 0x8000;
        }
        break;
      }
      case TextureFormat::A4R4G4B4: {
        converted = PackA4R4G4B4(color);
        const std::uint32_t rgb = converted & 0x0FFF;
        if (rgb == 0xF0F || (blackTransparent && !rgb)) {
          converted = 0;
        }
        break;
      }
      case TextureFormat::R5G6B5:
        converted = PackR5G6B5(color);
        break;
      default:
        converted = color;
        break;
      }

      WritePixel(output, column, bitsPerPixel, converted);
    }
  }

  this->Commit(clipped);
  return true;
}

void CTextureDX12::Upload(int x, int y, int width, int height, const std::uint32_t* pixels, int option, int sourceWidth) {
  this->SetImageSize(width, height);
  this->transparent = static_cast<std::uint8_t>(option);
  if (!pixels || this->m_pixels.empty()) {
    return;
  }

  if (this->m_renderTarget) {
    // DX9 stages render-target uploads through a cleared system-memory texture and replaces the whole level.
    std::fill(this->m_pixels.begin(), this->m_pixels.end(), std::uint8_t(0));
    this->CommitAll();
  }

  this->UploadCopy(x, y, width, height, pixels, static_cast<std::uint8_t>(option), sourceWidth);
}

void CTextureDX12::UploadSprite(int x, int y, int width, int height, const SpriteRleImage* image, const std::uint32_t* palette) {
  RECT area{x, y, x + width, y + height};
  if (this->m_pixels.empty() || !this->Clip(area) || area.left != x || area.top != y || area.right != x + width || area.bottom != y + height) {
    return;
  }

  const int bytesPerPixel = this->BitsPerPixel() / 8;
  std::uint8_t* origin = this->Row(y) + x * bytesPerPixel;
  if (this->m_format == TextureFormat::A1R5G5B5 || this->m_format == TextureFormat::A8R8G8B8) {
    for (int row = 0; row < height; ++row) {
      std::memset(origin + row * this->m_pitch, 0, width * bytesPerPixel);
    }
  }

  DecodeSprite(*image, palette, origin, static_cast<int>(this->m_pitch));
  this->Commit(area);
}

bool CTextureDX12::CopyTexture(CTexture* source, int sourceX, int sourceY, int sourceWidth, int sourceHeight, int x, int y, int width, int height) {
  auto* other = static_cast<CTextureDX12*>(source);
  RECT from{sourceX, sourceY, sourceX + sourceWidth, sourceY + sourceHeight};
  RECT to{x, y, x + width, y + height};
  if (this->m_pixels.empty() || other->m_pixels.empty() || !other->Clip(from) || !this->Clip(to)) {
    return false;
  }

  const int sourceBits = other->BitsPerPixel();
  const int targetBits = this->BitsPerPixel();
  const UINT fromWidth = from.right - from.left, fromHeight = from.bottom - from.top;
  const UINT toWidth = to.right - to.left, toHeight = to.bottom - to.top;
  const auto sample = [&](UINT sx, UINT sy) {
    return ToArgb(ReadPixel(other->Row(from.top + sy), from.left + sx, sourceBits), other->m_format);
  };

  for (UINT row = 0; row < toHeight; ++row) {
    std::uint8_t* output = this->Row(to.top + row);
    const float v = std::max(0.0f, (row + 0.5f) * fromHeight / toHeight - 0.5f);
    const UINT y0 = std::min<UINT>(static_cast<UINT>(v), fromHeight - 1);
    const UINT y1 = std::min<UINT>(y0 + 1, fromHeight - 1);
    const float ty = v - y0;

    for (UINT column = 0; column < toWidth; ++column) {
      std::uint32_t color;
      if (fromWidth == toWidth && fromHeight == toHeight) {
        color = sample(column, row);
      } else {
        const float u = std::max(0.0f, (column + 0.5f) * fromWidth / toWidth - 0.5f);
        const UINT x0 = std::min<UINT>(static_cast<UINT>(u), fromWidth - 1);
        const UINT x1 = std::min<UINT>(x0 + 1, fromWidth - 1);
        const float tx = u - x0;
        color = Lerp(Lerp(sample(x0, y0), sample(x1, y0), tx), Lerp(sample(x0, y1), sample(x1, y1), tx), ty);
      }
      WritePixel(output, to.left + column, targetBits, FromArgb(color, this->m_format));
    }
  }

  this->Commit(to);
  return true;
}

void CTextureDX12::Draw(int x, int y, int width, int height, std::uint32_t color) {
  this->DrawTo(
    g_renderer->primitives, g_renderer->queues, x, y, width, height, color, g_renderer->fog.supported != 0, g_depthStart, g_depthEnd
  );
}

void CTextureDX12::Stretch(int x, int y, int width, int height) {
  this->StretchTo(
    g_renderer->primitives, g_renderer->queues, x, y, width, height, g_renderer->fog.supported != 0, g_depthStart, g_depthEnd
  );
}
