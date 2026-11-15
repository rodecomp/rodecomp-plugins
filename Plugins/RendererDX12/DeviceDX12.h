#ifndef RAGEXE_PLUGIN_RENDERERDX12_DEVICEDX12_H
#define RAGEXE_PLUGIN_RENDERERDX12_DEVICEDX12_H

// The D3D9 enums (D3DRS_*, D3DTSS_*, D3DSAMP_*, D3DBLEND_*, ...) are the vocabulary of the game's DX9 queue code;
// DeviceStateDX12 keeps them so that code ports onto DX12 line for line.
#include <d3d9.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <cstdint>
#include <unordered_map>
#include <vector>

using Microsoft::WRL::ComPtr;

class CTextureDX12;

struct GpuResource {
  ComPtr<ID3D12Resource> resource;
  D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
  UINT width = 0;
  UINT height = 0;
  UINT srv = UINT(-1);
  UINT rtv = UINT(-1);
};

// DeviceStateDX9's state block and API, recorded instead of sent to a device: DeviceDX12::Draw turns it into a
// pipeline state and shader constants.
struct DeviceStateDX12 {
  DWORD render[210];
  DWORD textureStage[2][33];
  DWORD sampler[2][14];
  CTextureDX12* textures[2];

  DeviceStateDX12();
  void Begin();
  void End();
  void SetRenderState(D3DRENDERSTATETYPE state, DWORD value);
  void SetSamplerState(UINT stage, D3DSAMPLERSTATETYPE state, DWORD value);
  void SetTexture(UINT stage, CTextureDX12* texture);
  void SetTextureStageState(UINT stage, D3DTEXTURESTAGESTATETYPE state, DWORD value);
};

class ScopedDeviceStateDX12 {
public:
  explicit ScopedDeviceStateDX12(DeviceStateDX12& state);
  ~ScopedDeviceStateDX12();
  ScopedDeviceStateDX12(const ScopedDeviceStateDX12&) = delete;
  ScopedDeviceStateDX12& operator=(const ScopedDeviceStateDX12&) = delete;

private:
  DeviceStateDX12* m_state;
  DWORD m_render[210];
  DWORD m_textureStage[2][33];
  DWORD m_sampler[2][14];
};

class DeviceDX12 {
public:
  static constexpr UINT FrameCount = 2;

  DeviceDX12() = default;
  ~DeviceDX12();
  DeviceDX12(const DeviceDX12&) = delete;
  DeviceDX12& operator=(const DeviceDX12&) = delete;

  HRESULT Initialize(HWND window, UINT width, UINT height);
  void Close();

  // Copies the scene into the swap chain, presents, and starts recording the next frame.
  HRESULT Present();
  // Executes everything recorded so far and waits for the GPU.
  void Flush();

  // nullptr: the scene (the game's back buffer, with depth); otherwise a render-target texture, without depth.
  void SetRenderTarget(GpuResource* target);
  void Clear(bool color, bool depth, DWORD argb);

  // DrawPrimitiveUP / DrawIndexedPrimitiveUP with the current state.
  void Draw(D3DPRIMITIVETYPE type, UINT primitiveCount, const void* vertices, UINT vertexCount, UINT stride, const std::uint16_t* indices,
            UINT indexCount, bool lightmapped);
  // StretchRect onto the scene: no blending, depth, fog or alpha test.
  void Blit(GpuResource& source, const RECT& from, const RECT& to);

  bool CreateTexture(UINT width, UINT height, bool renderTarget, GpuResource& output);
  void ReleaseTexture(GpuResource& texture);
  // 32-bit BGRA rows into texture rectangle (x, y, width, height).
  void UploadTexture(GpuResource& texture, const std::uint32_t* pixels, UINT pitchPixels, UINT x, UINT y, UINT width, UINT height);
  void CopyTexture(GpuResource& destination, GpuResource& source);

  std::vector<std::uint32_t> ReadScene();
  void WriteScene(const std::uint32_t* pixels);

  GpuResource& Scene() {
    return m_scene;
  }

  DeviceStateDX12 state;
  HWND window = nullptr;
  UINT width = 0;
  UINT height = 0;

private:
  struct UploadPage {
    ComPtr<ID3D12Resource> buffer;
    std::uint8_t* cpu = nullptr;
    D3D12_GPU_VIRTUAL_ADDRESS gpu = 0;
    UINT64 size = 0;
    UINT64 used = 0;
  };

  struct Allocation {
    std::uint8_t* cpu;
    D3D12_GPU_VIRTUAL_ADDRESS gpu;
    ID3D12Resource* buffer;
    UINT64 offset;
  };

  struct Frame {
    ComPtr<ID3D12CommandAllocator> allocator;
    UINT64 fence = 0;
    std::vector<UploadPage> pages;
    std::vector<ComPtr<ID3D12Resource>> retired;
  };

  struct DrawConstants {
    float targetWidth;
    float targetHeight;
    std::uint32_t stage[2][6];
    std::uint32_t address[2];
    std::uint32_t coordinate[2];
    std::uint32_t alphaTest;
    std::uint32_t alphaFunction;
    std::uint32_t alphaReference;
    std::uint32_t fog;
    std::uint32_t fogColor;
    std::uint32_t textureFactor;
  };
  static_assert(sizeof(DrawConstants) == 24 * 4);

  HRESULT CreatePipelineObjects();
  ID3D12PipelineState* Pipeline(std::uint32_t key);
  Allocation Allocate(UINT64 size, UINT64 alignment);
  void Transition(GpuResource& resource, D3D12_RESOURCE_STATES state);
  void BeginRecording();
  void Submit();
  void WaitForFence(UINT64 value);
  void BindTarget();
  GpuResource* Bound(UINT stage);
  void DrawRaw(D3D_PRIMITIVE_TOPOLOGY topology, UINT count, const void* vertices, UINT vertexCount, UINT stride, const std::uint16_t* indices,
               std::uint32_t key, const DrawConstants& constants, GpuResource* texture0, GpuResource* texture1);
  UINT AllocateDescriptor(std::vector<UINT>& free, UINT& next, UINT capacity);

  ComPtr<IDXGIFactory4> m_factory;
  ComPtr<ID3D12Device> m_device;
  ComPtr<ID3D12CommandQueue> m_queue;
  ComPtr<IDXGISwapChain3> m_swapChain;
  ComPtr<ID3D12GraphicsCommandList> m_list;
  ComPtr<ID3D12Fence> m_fence;
  HANDLE m_fenceEvent = nullptr;
  UINT64 m_fenceValue = 0;
  Frame m_frames[FrameCount];
  UINT m_frame = 0;

  GpuResource m_backBuffers[FrameCount];
  GpuResource m_scene;
  GpuResource m_depth;
  GpuResource m_white;
  GpuResource* m_target = nullptr;

  ComPtr<ID3D12DescriptorHeap> m_rtvHeap;
  ComPtr<ID3D12DescriptorHeap> m_dsvHeap;
  ComPtr<ID3D12DescriptorHeap> m_cpuSrvHeap;
  ComPtr<ID3D12DescriptorHeap> m_gpuSrvHeap;
  UINT m_rtvSize = 0;
  UINT m_srvSize = 0;
  std::vector<UINT> m_freeSrv;
  std::vector<UINT> m_freeRtv;
  UINT m_nextSrv = 0;
  UINT m_nextRtv = 0;
  UINT m_gpuSrvUsed = 0;
  GpuResource* m_tableTextures[2] = {};
  bool m_targetBound = false;
  ID3D12PipelineState* m_pipeline = nullptr;

  ComPtr<ID3D12RootSignature> m_rootSignature;
  ComPtr<ID3DBlob> m_vertexShader;
  ComPtr<ID3DBlob> m_vertexShaderLightmapped;
  ComPtr<ID3DBlob> m_pixelShader;
  std::unordered_map<std::uint32_t, ComPtr<ID3D12PipelineState>> m_pipelines;
  std::vector<std::uint16_t> m_fanIndices;
};

#endif
