#include "RendererDX12Api.h"

#include <algorithm>
#include <utility>
#include <vector>

namespace {
  DeviceDX12* g_device;
  std::vector<std::pair<RendererDX12FrameCallback, void*>> g_frameCallbacks;
}

DeviceDX12* RendererDX12_Device() {
  return g_device;
}

bool RendererDX12_AddFrameCallback(RendererDX12FrameCallback callback, void* user) {
  const std::pair<RendererDX12FrameCallback, void*> entry(callback, user);
  if (!callback || std::find(g_frameCallbacks.begin(), g_frameCallbacks.end(), entry) != g_frameCallbacks.end()) {
    return false;
  }
  g_frameCallbacks.push_back(entry);
  return true;
}

void RendererDX12_RemoveFrameCallback(RendererDX12FrameCallback callback, void* user) {
  const auto found = std::find(g_frameCallbacks.begin(), g_frameCallbacks.end(), std::make_pair(callback, user));
  if (found != g_frameCallbacks.end()) {
    g_frameCallbacks.erase(found);
  }
}

void SetActiveDeviceDX12(DeviceDX12* device) {
  g_device = device;
}

void RunFrameCallbacksDX12(DeviceDX12& device) {
  if (g_frameCallbacks.empty()) {
    return;
  }
  device.SetRenderTarget(nullptr);
  CTextureDX12* const textures[2] = {device.state.textures[0], device.state.textures[1]};
  for (std::size_t i = 0; i < g_frameCallbacks.size(); ++i) {
    ScopedDeviceStateDX12 restore(device.state);
    g_frameCallbacks[i].first(device, device.state, g_frameCallbacks[i].second);
    device.state.textures[0] = textures[0];
    device.state.textures[1] = textures[1];
  }
}
