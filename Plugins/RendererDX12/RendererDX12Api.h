// RendererDX12's public API for dependent plugins ("dependencies": {"RendererDX12": "^1.0.0"}). scripts/build.py
// compiles RendererDX12 with RENDERERDX12_EXPORTS and gives dependents its directory and import library, so they
// include this header and call these functions plus the DeviceDX12.h members marked RENDERERDX12_API
// (DeviceDX12::Draw, DeviceDX12::SetRenderTarget, DeviceStateDX12, ScopedDeviceStateDX12) directly.
#ifndef RAGEXE_PLUGIN_RENDERERDX12_RENDERERDX12API_H
#define RAGEXE_PLUGIN_RENDERERDX12_RENDERERDX12API_H

#ifdef RENDERERDX12_EXPORTS
#define RENDERERDX12_API __declspec(dllexport)
#else
#define RENDERERDX12_API __declspec(dllimport)
#endif

class DeviceDX12;
struct DeviceStateDX12;

// Runs once per frame after the scene and the HUD are drawn and before Present, with the scene bound as the render
// target. Draw through device.Draw with `state` (device.state); state and textures are restored after each callback.
using RendererDX12FrameCallback = void (*)(DeviceDX12& device, DeviceStateDX12& state, void* user);

// The device of the running Direct3D 12 render system, or nullptr while the game is not rendering on it.
RENDERERDX12_API DeviceDX12* RendererDX12_Device();
// Callbacks run in registration order; a null callback or a (callback, user) pair already added returns false.
RENDERERDX12_API bool RendererDX12_AddFrameCallback(RendererDX12FrameCallback callback, void* user);
RENDERERDX12_API void RendererDX12_RemoveFrameCallback(RendererDX12FrameCallback callback, void* user);

#ifdef RENDERERDX12_EXPORTS
// RendererDX12 internals: RenderSystemDX12 publishes its device, DeviceDX12::Present runs the callbacks.
void SetActiveDeviceDX12(DeviceDX12* device);
void RunFrameCallbacksDX12(DeviceDX12& device);
#endif

// DeviceDX12.h includes this header first for the macro; included here, it completes the API.
#include "DeviceDX12.h"

#endif
