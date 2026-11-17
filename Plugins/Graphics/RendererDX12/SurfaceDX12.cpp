#include "SurfaceDX12.h"

#include <algorithm>
#include <cstring>

namespace {
  UINT AlignDimension(int value) {
    const std::uint32_t dimension = value;
    return dimension + ((0u - dimension) & 3);
  }

  RECT ClipTo(const RECT& rectangle, LONG width, LONG height) {
    return {
      std::clamp<LONG>(rectangle.left, 0, width), std::clamp<LONG>(rectangle.top, 0, height), std::clamp<LONG>(rectangle.right, 0, width),
      std::clamp<LONG>(rectangle.bottom, 0, height)
    };
  }
}

CSurfaceDX12::CSurfaceDX12(DeviceDX12& device, int width, int height)
    : m_device(&device), m_backBuffer(false), m_width(AlignDimension(width)), m_height(AlignDimension(height)) {
  this->Allocate();
}

CSurfaceDX12::CSurfaceDX12(DeviceDX12& device) : m_device(&device), m_backBuffer(true), m_width(device.width), m_height(device.height) {
}

CSurfaceDX12::~CSurfaceDX12() {
  this->FreeDib();
  this->Release();
}

bool CSurfaceDX12::Allocate() {
  if (!this->m_width || !this->m_height) {
    return false;
  }
  this->m_pixels.assign(static_cast<std::size_t>(this->m_width) * this->m_height, 0);
  if (!this->m_device->CreateTexture(this->m_width, this->m_height, false, this->m_gpu)) {
    this->Release();
    return false;
  }
  this->Commit(this->Bounds());
  return true;
}

void CSurfaceDX12::Release() {
  if (this->m_gpu.resource) {
    this->m_device->ReleaseTexture(this->m_gpu);
  }
  this->m_gpu = GpuResource{};
  if (!this->m_backBuffer) {
    this->m_pixels.clear();
    this->m_pixels.shrink_to_fit();
  }
}

void CSurfaceDX12::FreeDib() {
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

RECT CSurfaceDX12::Bounds() const {
  return {0, 0, static_cast<LONG>(this->m_width), static_cast<LONG>(this->m_height)};
}

RECT CSurfaceDX12::ClipToScene(const RECT& rectangle) const {
  return ClipTo(rectangle, static_cast<LONG>(this->m_device->width), static_cast<LONG>(this->m_device->height));
}

bool CSurfaceDX12::Acquire() {
  if (this->m_backBuffer) {
    this->m_width = this->m_device->width;
    this->m_height = this->m_device->height;
    this->m_pixels = this->m_device->ReadScene();
    return this->m_pixels.size() == static_cast<std::size_t>(this->m_width) * this->m_height && !this->m_pixels.empty();
  }
  return !this->m_pixels.empty();
}

void CSurfaceDX12::Commit(const RECT& area) {
  const RECT rectangle = ClipTo(area, this->m_width, this->m_height);
  if (rectangle.right <= rectangle.left || rectangle.bottom <= rectangle.top || this->m_pixels.empty()) {
    return;
  }

  if (this->m_backBuffer) {
    this->m_device->WriteScene(this->m_pixels.data());
  } else if (this->m_gpu.resource) {
    this->m_device->UploadTexture(
      this->m_gpu, this->m_pixels.data() + static_cast<std::size_t>(rectangle.top) * this->m_width + rectangle.left, this->m_width, rectangle.left,
      rectangle.top, rectangle.right - rectangle.left, rectangle.bottom - rectangle.top
    );
  }
}

std::uint32_t CSurfaceDX12::Width() {
  return this->m_backBuffer ? this->m_device->width : this->m_width;
}

std::uint32_t CSurfaceDX12::Height() {
  return this->m_backBuffer ? this->m_device->height : this->m_height;
}

bool CSurfaceDX12::Resize(int width, int height) {
  if (this->m_backBuffer) {
    return false;
  }
  this->FreeDib();
  this->Release();
  this->m_width = AlignDimension(width);
  this->m_height = AlignDimension(height);
  return this->Allocate();
}

SurfaceLock CSurfaceDX12::Lock() {
  SurfaceLock output{};
  if (this->m_locked || this->m_dc || !this->Acquire()) {
    return output;
  }

  this->m_locked = true;
  output.success = true;
  output.pixels = this->m_pixels.data();
  output.pitch = static_cast<int>(this->m_width * 4);
  output.width = this->m_width;
  output.height = this->m_height;
  output.bitsPerPixel = 32;
  return output;
}

void CSurfaceDX12::Unlock() {
  if (this->m_locked) {
    this->m_locked = false;
    this->Commit(this->Bounds());
  }
}

bool CSurfaceDX12::GetDC(HDC* output) {
  if (this->m_locked || this->m_dc || !this->Acquire()) {
    return false;
  }

  BITMAPINFO information{};
  information.bmiHeader.biSize = sizeof(information.bmiHeader);
  information.bmiHeader.biWidth = static_cast<LONG>(this->m_width);
  information.bmiHeader.biHeight = -static_cast<LONG>(this->m_height);
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
  std::memcpy(this->m_dibBits, this->m_pixels.data(), this->m_pixels.size() * sizeof(std::uint32_t));
  *output = this->m_dc;
  return true;
}

void CSurfaceDX12::ReleaseDC(HDC dc) {
  if (!this->m_dc || dc != this->m_dc) {
    return;
  }

  GdiFlush();
  for (std::size_t i = 0; i < this->m_pixels.size(); ++i) {
    this->m_pixels[i] = this->m_dibBits[i] | 0xFF000000u;
  }
  this->FreeDib();
  this->Commit(this->Bounds());
}

void CSurfaceDX12::Upload(int x, int y, int width, int height, const std::uint32_t* pixels, int, int sourceWidth) {
  if (!width || !height || !this->Acquire()) {
    return;
  }

  const int stride = sourceWidth ? sourceWidth : width;
  const RECT area{x, y, x + width, y + height};
  const RECT clipped = ClipTo(area, this->m_width, this->m_height);
  for (LONG row = clipped.top; row < clipped.bottom; ++row) {
    std::uint32_t* output = this->m_pixels.data() + static_cast<std::size_t>(row) * this->m_width;
    const std::uint32_t* source = pixels + (row - y) * stride;
    for (LONG column = clipped.left; column < clipped.right; ++column) {
      const std::uint32_t color = source[column - x];
      output[column] = (color & 0xFFFFFF) == 0xFF00FF ? 0 : color;
    }
  }
  this->Commit(clipped);
}

void CSurfaceDX12::Fill(const RECT* rectangle, std::uint32_t color) {
  if (!this->Acquire()) {
    return;
  }

  const RECT clipped = rectangle ? ClipTo(*rectangle, this->m_width, this->m_height) : this->Bounds();
  for (LONG row = clipped.top; row < clipped.bottom; ++row) {
    std::uint32_t* output = this->m_pixels.data() + static_cast<std::size_t>(row) * this->m_width;
    std::fill(output + clipped.left, output + clipped.right, color);
  }
  this->Commit(clipped);
}

void CSurfaceDX12::Draw(int x, int y, int width, int height, std::uint32_t) {
  if (this->m_backBuffer || !this->m_gpu.resource) {
    return;
  }

  const RECT destination = this->ClipToScene(RECT{x, y, x + width, y + height});
  const RECT original{0, 0, destination.right - destination.left, destination.bottom - destination.top};
  const RECT source = ClipTo(original, this->m_width, this->m_height);
  if (source.right <= source.left || source.bottom <= source.top) {
    return;
  }
  // StretchRect fails unless both rectangles are valid; equal sizes keep it a copy.
  const RECT to{destination.left, destination.top, destination.left + (source.right - source.left), destination.top + (source.bottom - source.top)};
  this->m_device->Blit(this->m_gpu, source, to);
}

void CSurfaceDX12::Stretch(int x, int y, int width, int height) {
  if (this->m_backBuffer || !this->m_gpu.resource) {
    return;
  }

  const RECT destination = this->ClipToScene(RECT{x, y, x + width, y + height});
  if (destination.right <= destination.left || destination.bottom <= destination.top) {
    return;
  }
  this->m_device->Blit(this->m_gpu, this->Bounds(), destination);
}
