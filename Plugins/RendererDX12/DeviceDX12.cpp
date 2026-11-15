#include "DeviceDX12.h"
#include "TextureDX12.h"

#include <d3dcompiler.h>

#include <algorithm>
#include <cstring>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

namespace {
  constexpr DXGI_FORMAT colorFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
  constexpr DXGI_FORMAT depthFormat = DXGI_FORMAT_D32_FLOAT;
  constexpr UINT cpuSrvCapacity = 8192;
  constexpr UINT rtvCapacity = 1024;
  constexpr UINT gpuSrvPerFrame = 8192;
  constexpr UINT64 uploadPageSize = 4u << 20;

  // Pipeline key layout (see DeviceDX12::Pipeline).
  constexpr std::uint32_t keySrcShift = 0;          // 4 bits D3DBLEND
  constexpr std::uint32_t keyDstShift = 4;          // 4 bits D3DBLEND
  constexpr std::uint32_t keyOpShift = 8;           // 3 bits D3DBLENDOP
  constexpr std::uint32_t keyBlend = 1u << 11;
  constexpr std::uint32_t keyZEnable = 1u << 12;
  constexpr std::uint32_t keyZWrite = 1u << 13;
  constexpr std::uint32_t keyZFuncShift = 14;       // 4 bits D3DCMPFUNC
  constexpr std::uint32_t keyCullShift = 18;        // 2 bits D3DCULL
  constexpr std::uint32_t keyTopologyShift = 20;    // 2 bits: 0 triangle, 1 line, 2 point
  constexpr std::uint32_t keyLightmapped = 1u << 22;
  constexpr std::uint32_t keyTextureTarget = 1u << 24;  // render-target texture: no depth buffer

  const char shaderSource[] = R"(
cbuffer DrawConstants : register(b0) {
  float2 targetSize; uint2 stage0a;
  uint4 stage0b;
  uint4 stage1a;
  uint4 stage1b;
  uint4 misc0;
  uint4 misc1;
};
Texture2D texture0 : register(t0);
Texture2D texture1 : register(t1);
SamplerState pointWrap : register(s0);
SamplerState pointClamp : register(s1);
SamplerState pointBorder : register(s2);
SamplerState linearWrap : register(s3);
SamplerState linearClamp : register(s4);
SamplerState linearBorder : register(s5);

struct VSIn {
  float4 position : POSITION;
  float4 diffuse : COLOR0;
  float4 specular : COLOR1;
  float2 uv0 : TEXCOORD0;
#ifdef LIGHTMAPPED
  float2 uv1 : TEXCOORD1;
#endif
};

struct PSIn {
  float4 position : SV_Position;
  float4 diffuse : COLOR0;
  float4 specular : COLOR1;
  float2 uv0 : TEXCOORD0;
  float2 uv1 : TEXCOORD1;
};

PSIn VSMain(VSIn input) {
  PSIn output;
  // D3D9 pixel centers sit on integer coordinates, D3D12's on half-integers.
  float x = (input.position.x + 0.5) / targetSize.x * 2.0 - 1.0;
  float y = 1.0 - (input.position.y + 0.5) / targetSize.y * 2.0;
  float w = input.position.w > 0.0 ? 1.0 / input.position.w : 1.0;
  output.position = float4(x * w, y * w, input.position.z * w, w);
  output.diffuse = input.diffuse;
  output.specular = input.specular;
  output.uv0 = input.uv0;
#ifdef LIGHTMAPPED
  output.uv1 = input.uv1;
#else
  output.uv1 = input.uv0;
#endif
  return output;
}

float4 SampleStage(Texture2D source, uint address, float2 uv) {
  uint mode = address & 0xFF;
  bool linearFilter = (address & 0x100) != 0;
  if (mode == 3) return linearFilter ? source.Sample(linearClamp, uv) : source.Sample(pointClamp, uv);
  if (mode == 4) return linearFilter ? source.Sample(linearBorder, uv) : source.Sample(pointBorder, uv);
  return linearFilter ? source.Sample(linearWrap, uv) : source.Sample(pointWrap, uv);
}

float4 Unpack(uint c) {
  return float4((c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF, c >> 24) / 255.0;
}

float4 Argument(uint arg, float4 diffuse, float4 current, float4 tex, float4 factor, float4 specular) {
  uint source = arg & 0xF;
  float4 v = current;
  if (source == 0) v = diffuse;
  else if (source == 2) v = tex;
  else if (source == 3) v = factor;
  else if (source == 4) v = specular;
  if (arg & 0x10) v = 1.0 - v;
  if (arg & 0x20) v = v.aaaa;
  return v;
}

float4 Operation(uint op, float4 a1, float4 a2, float4 diffuse, float4 current, float4 tex, float4 factor) {
  float4 r = a1;
  if (op == 3) r = a2;
  else if (op == 4) r = a1 * a2;
  else if (op == 5) r = a1 * a2 * 2.0;
  else if (op == 6) r = a1 * a2 * 4.0;
  else if (op == 7) r = a1 + a2;
  else if (op == 8) r = a1 + a2 - 0.5;
  else if (op == 9) r = (a1 + a2 - 0.5) * 2.0;
  else if (op == 10) r = a1 - a2;
  else if (op == 11) r = a1 + a2 - a1 * a2;
  else if (op == 12) r = lerp(a2, a1, diffuse.a);
  else if (op == 13) r = lerp(a2, a1, tex.a);
  else if (op == 14) r = lerp(a2, a1, factor.a);
  else if (op == 15) r = a1 + a2 * (1.0 - tex.a);
  else if (op == 16) r = lerp(a2, a1, current.a);
  return saturate(r);
}

float4 Stage(uint colorOp, uint colorArg1, uint colorArg2, uint alphaOp, uint alphaArg1, uint alphaArg2, float4 tex, float4 diffuse,
             float4 current, float4 factor, float4 specular) {
  float4 c1 = Argument(colorArg1, diffuse, current, tex, factor, specular);
  float4 c2 = Argument(colorArg2, diffuse, current, tex, factor, specular);
  float3 rgb = Operation(colorOp, c1, c2, diffuse, current, tex, factor).rgb;
  float a = current.a;
  if (alphaOp > 1) {
    float4 x1 = Argument(alphaArg1, diffuse, current, tex, factor, specular);
    float4 x2 = Argument(alphaArg2, diffuse, current, tex, factor, specular);
    a = Operation(alphaOp, x1.aaaa, x2.aaaa, diffuse, current, tex, factor).a;
  }
  return float4(rgb, a);
}

bool AlphaPasses(uint func, uint value, uint reference) {
  if (func == 1) return false;
  if (func == 2) return value < reference;
  if (func == 3) return value == reference;
  if (func == 4) return value <= reference;
  if (func == 5) return value > reference;
  if (func == 6) return value != reference;
  if (func == 7) return value >= reference;
  return true;
}

float4 PSMain(PSIn input) : SV_Target {
  float4 factor = Unpack(misc1.w);
  float4 current = input.diffuse;
  if (stage0a.x > 1) {
    float4 tex0 = SampleStage(texture0, stage1b.z, misc0.x == 1 ? input.uv1 : input.uv0);
    current = Stage(stage0a.x, stage0a.y, stage0b.x, stage0b.y, stage0b.z, stage0b.w, tex0, input.diffuse, input.diffuse, factor, input.specular);
    if (stage1a.x > 1) {
      float4 tex1 = SampleStage(texture1, stage1b.w, misc0.y == 1 ? input.uv1 : input.uv0);
      current = Stage(stage1a.x, stage1a.y, stage1a.z, stage1a.w, stage1b.x, stage1b.y, tex1, input.diffuse, current, factor, input.specular);
    }
  }
  if (misc0.z != 0 && !AlphaPasses(misc0.w, (uint)round(saturate(current.a) * 255.0), misc1.x)) discard;
  if (misc1.y & 2) current.rgb = saturate(current.rgb + input.specular.rgb);
  if (misc1.y & 1) current.rgb = lerp(Unpack(misc1.z).rgb, current.rgb, input.specular.a);
  return current;
}
)";

  D3D12_RESOURCE_BARRIER TransitionBarrier(ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    return barrier;
  }

  D3D12_RESOURCE_DESC TextureDesc(UINT width, UINT height, DXGI_FORMAT format, D3D12_RESOURCE_FLAGS flags) {
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = flags;
    return desc;
  }

  D3D12_RESOURCE_DESC BufferDesc(UINT64 size) {
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = size;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    return desc;
  }

  D3D12_HEAP_PROPERTIES HeapProperties(D3D12_HEAP_TYPE type) {
    D3D12_HEAP_PROPERTIES properties = {};
    properties.Type = type;
    return properties;
  }

  HRESULT Compile(const char* entry, const char* target, const D3D_SHADER_MACRO* defines, ComPtr<ID3DBlob>& output) {
    ComPtr<ID3DBlob> errors;
    HRESULT result = D3DCompile(shaderSource, sizeof(shaderSource) - 1, "DeviceDX12", defines, nullptr, entry, target,
                                D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &output, &errors);
    if (errors) {
      OutputDebugStringA(static_cast<const char*>(errors->GetBufferPointer()));
    }
    return result;
  }

  D3D12_BLEND ColorBlend(std::uint32_t blend, bool source) {
    switch (blend) {
      case D3DBLEND_BOTHSRCALPHA:
        return source ? D3D12_BLEND_SRC_ALPHA : D3D12_BLEND_INV_SRC_ALPHA;
      case D3DBLEND_BOTHINVSRCALPHA:
        return source ? D3D12_BLEND_INV_SRC_ALPHA : D3D12_BLEND_SRC_ALPHA;
      case 0:
        return source ? D3D12_BLEND_ONE : D3D12_BLEND_ZERO;
      default:
        return static_cast<D3D12_BLEND>(blend);  // D3DBLEND 1-11, 14, 15 share D3D12_BLEND values
    }
  }

  D3D12_BLEND AlphaBlend(D3D12_BLEND blend) {
    switch (blend) {
      case D3D12_BLEND_SRC_COLOR:
        return D3D12_BLEND_SRC_ALPHA;
      case D3D12_BLEND_INV_SRC_COLOR:
        return D3D12_BLEND_INV_SRC_ALPHA;
      case D3D12_BLEND_DEST_COLOR:
        return D3D12_BLEND_DEST_ALPHA;
      case D3D12_BLEND_INV_DEST_COLOR:
        return D3D12_BLEND_INV_DEST_ALPHA;
      default:
        return blend;
    }
  }

  float FloatBits(DWORD value) {
    float result;
    std::memcpy(&result, &value, sizeof(result));
    return result;
  }

  DWORD DwordBits(float value) {
    DWORD result;
    std::memcpy(&result, &value, sizeof(result));
    return result;
  }
}

// DeviceStateDX9::Initialize reads these from a fresh D3D9 device; they are the documented D3D9 defaults.
DeviceStateDX12::DeviceStateDX12() : render{}, textureStage{}, sampler{}, textures{} {
  render[D3DRS_ZENABLE] = D3DZB_TRUE;
  render[D3DRS_FILLMODE] = D3DFILL_SOLID;
  render[D3DRS_SHADEMODE] = D3DSHADE_GOURAUD;
  render[D3DRS_ZWRITEENABLE] = TRUE;
  render[D3DRS_LASTPIXEL] = TRUE;
  render[D3DRS_SRCBLEND] = D3DBLEND_ONE;
  render[D3DRS_DESTBLEND] = D3DBLEND_ZERO;
  render[D3DRS_CULLMODE] = D3DCULL_CCW;
  render[D3DRS_ZFUNC] = D3DCMP_LESSEQUAL;
  render[D3DRS_ALPHAFUNC] = D3DCMP_ALWAYS;
  render[D3DRS_FOGEND] = DwordBits(1.0f);
  render[D3DRS_FOGDENSITY] = DwordBits(1.0f);
  render[D3DRS_STENCILFUNC] = D3DCMP_ALWAYS;
  render[D3DRS_STENCILFAIL] = D3DSTENCILOP_KEEP;
  render[D3DRS_STENCILZFAIL] = D3DSTENCILOP_KEEP;
  render[D3DRS_STENCILPASS] = D3DSTENCILOP_KEEP;
  render[D3DRS_STENCILMASK] = 0xFFFFFFFFu;
  render[D3DRS_STENCILWRITEMASK] = 0xFFFFFFFFu;
  render[D3DRS_TEXTUREFACTOR] = 0xFFFFFFFFu;
  render[D3DRS_CLIPPING] = TRUE;
  render[D3DRS_LIGHTING] = TRUE;
  render[D3DRS_COLORVERTEX] = TRUE;
  render[D3DRS_LOCALVIEWER] = TRUE;
  render[D3DRS_DIFFUSEMATERIALSOURCE] = D3DMCS_COLOR1;
  render[D3DRS_SPECULARMATERIALSOURCE] = D3DMCS_COLOR2;
  render[D3DRS_POINTSIZE] = DwordBits(1.0f);
  render[D3DRS_POINTSIZE_MIN] = DwordBits(1.0f);
  render[D3DRS_POINTSCALE_A] = DwordBits(1.0f);
  render[D3DRS_POINTSIZE_MAX] = DwordBits(64.0f);
  render[D3DRS_MULTISAMPLEANTIALIAS] = TRUE;
  render[D3DRS_MULTISAMPLEMASK] = 0xFFFFFFFFu;
  render[D3DRS_COLORWRITEENABLE] = 0x0000000Fu;
  render[D3DRS_BLENDOP] = D3DBLENDOP_ADD;
  render[D3DRS_POSITIONDEGREE] = D3DDEGREE_CUBIC;
  render[D3DRS_NORMALDEGREE] = D3DDEGREE_LINEAR;
  render[D3DRS_MINTESSELLATIONLEVEL] = DwordBits(1.0f);
  render[D3DRS_MAXTESSELLATIONLEVEL] = DwordBits(1.0f);
  render[D3DRS_CCW_STENCILFAIL] = D3DSTENCILOP_KEEP;
  render[D3DRS_CCW_STENCILZFAIL] = D3DSTENCILOP_KEEP;
  render[D3DRS_CCW_STENCILPASS] = D3DSTENCILOP_KEEP;
  render[D3DRS_CCW_STENCILFUNC] = D3DCMP_ALWAYS;
  render[D3DRS_COLORWRITEENABLE1] = 0x0000000Fu;
  render[D3DRS_COLORWRITEENABLE2] = 0x0000000Fu;
  render[D3DRS_COLORWRITEENABLE3] = 0x0000000Fu;
  render[D3DRS_BLENDFACTOR] = 0xFFFFFFFFu;
  render[D3DRS_SRCBLENDALPHA] = D3DBLEND_ONE;
  render[D3DRS_DESTBLENDALPHA] = D3DBLEND_ZERO;
  render[D3DRS_BLENDOPALPHA] = D3DBLENDOP_ADD;

  for (UINT stage = 0; stage < 2; ++stage) {
    DWORD* ts = textureStage[stage];
    ts[D3DTSS_COLOROP] = stage == 0 ? D3DTOP_MODULATE : D3DTOP_DISABLE;
    ts[D3DTSS_COLORARG1] = D3DTA_TEXTURE;
    ts[D3DTSS_COLORARG2] = D3DTA_CURRENT;
    ts[D3DTSS_ALPHAOP] = stage == 0 ? D3DTOP_SELECTARG1 : D3DTOP_DISABLE;
    ts[D3DTSS_ALPHAARG1] = D3DTA_TEXTURE;
    ts[D3DTSS_ALPHAARG2] = D3DTA_CURRENT;
    ts[D3DTSS_TEXCOORDINDEX] = stage;
    ts[D3DTSS_COLORARG0] = D3DTA_CURRENT;
    ts[D3DTSS_ALPHAARG0] = D3DTA_CURRENT;
    ts[D3DTSS_RESULTARG] = D3DTA_CURRENT;

    DWORD* ss = sampler[stage];
    ss[D3DSAMP_ADDRESSU] = D3DTADDRESS_WRAP;
    ss[D3DSAMP_ADDRESSV] = D3DTADDRESS_WRAP;
    ss[D3DSAMP_ADDRESSW] = D3DTADDRESS_WRAP;
    ss[D3DSAMP_MAGFILTER] = D3DTEXF_POINT;
    ss[D3DSAMP_MINFILTER] = D3DTEXF_POINT;
    ss[D3DSAMP_MIPFILTER] = D3DTEXF_NONE;
    ss[D3DSAMP_MAXANISOTROPY] = 1;
  }
}

void DeviceStateDX12::Begin() {
  textures[0] = nullptr;
  textures[1] = nullptr;
}

void DeviceStateDX12::End() {
  textures[0] = nullptr;
  textures[1] = nullptr;
}

void DeviceStateDX12::SetRenderState(D3DRENDERSTATETYPE state, DWORD value) {
  if (static_cast<UINT>(state) < 210) {
    render[state] = value;
  }
}

void DeviceStateDX12::SetSamplerState(UINT stage, D3DSAMPLERSTATETYPE state, DWORD value) {
  if (stage < 2 && static_cast<UINT>(state) < 14) {
    sampler[stage][state] = value;
  }
}

void DeviceStateDX12::SetTexture(UINT stage, CTextureDX12* texture) {
  if (stage < 2) {
    textures[stage] = texture;
  }
}

void DeviceStateDX12::SetTextureStageState(UINT stage, D3DTEXTURESTAGESTATETYPE state, DWORD value) {
  if (stage < 2 && static_cast<UINT>(state) < 33) {
    textureStage[stage][state] = value;
  }
}

ScopedDeviceStateDX12::ScopedDeviceStateDX12(DeviceStateDX12& state) : m_state(&state) {
  std::memcpy(m_render, state.render, sizeof(m_render));
  std::memcpy(m_textureStage, state.textureStage, sizeof(m_textureStage));
  std::memcpy(m_sampler, state.sampler, sizeof(m_sampler));
}

ScopedDeviceStateDX12::~ScopedDeviceStateDX12() {
  std::memcpy(m_state->render, m_render, sizeof(m_render));
  std::memcpy(m_state->textureStage, m_textureStage, sizeof(m_textureStage));
  std::memcpy(m_state->sampler, m_sampler, sizeof(m_sampler));
}

DeviceDX12::~DeviceDX12() {
  Close();
}

HRESULT DeviceDX12::Initialize(HWND windowHandle, UINT targetWidth, UINT targetHeight) {
  window = windowHandle;
  width = targetWidth;
  height = targetHeight;

  HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&m_factory));
  if (FAILED(hr)) return hr;
  hr = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&m_device));
  if (FAILED(hr)) return hr;

  D3D12_COMMAND_QUEUE_DESC queueDesc = {};
  queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
  hr = m_device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&m_queue));
  if (FAILED(hr)) return hr;

  DXGI_SWAP_CHAIN_DESC1 swapDesc = {};
  swapDesc.Width = width;
  swapDesc.Height = height;
  swapDesc.Format = colorFormat;
  swapDesc.SampleDesc.Count = 1;
  swapDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  swapDesc.BufferCount = FrameCount;
  swapDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
  ComPtr<IDXGISwapChain1> swapChain;
  hr = m_factory->CreateSwapChainForHwnd(m_queue.Get(), window, &swapDesc, nullptr, nullptr, &swapChain);
  if (FAILED(hr)) return hr;
  m_factory->MakeWindowAssociation(window, DXGI_MWA_NO_ALT_ENTER);
  hr = swapChain.As(&m_swapChain);
  if (FAILED(hr)) return hr;

  for (Frame& frame : m_frames) {
    hr = m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&frame.allocator));
    if (FAILED(hr)) return hr;
  }
  hr = m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_frames[0].allocator.Get(), nullptr, IID_PPV_ARGS(&m_list));
  if (FAILED(hr)) return hr;
  m_list->Close();

  hr = m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence));
  if (FAILED(hr)) return hr;
  m_fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  if (!m_fenceEvent) return HRESULT_FROM_WIN32(GetLastError());

  D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
  heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
  heapDesc.NumDescriptors = rtvCapacity;
  hr = m_device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&m_rtvHeap));
  if (FAILED(hr)) return hr;
  heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
  heapDesc.NumDescriptors = 1;
  hr = m_device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&m_dsvHeap));
  if (FAILED(hr)) return hr;
  heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
  heapDesc.NumDescriptors = cpuSrvCapacity;
  hr = m_device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&m_cpuSrvHeap));
  if (FAILED(hr)) return hr;
  heapDesc.NumDescriptors = gpuSrvPerFrame * FrameCount;
  heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
  hr = m_device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&m_gpuSrvHeap));
  if (FAILED(hr)) return hr;
  m_rtvSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
  m_srvSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

  for (UINT i = 0; i < FrameCount; ++i) {
    GpuResource& buffer = m_backBuffers[i];
    hr = m_swapChain->GetBuffer(i, IID_PPV_ARGS(&buffer.resource));
    if (FAILED(hr)) return hr;
    buffer.state = D3D12_RESOURCE_STATE_PRESENT;
    buffer.width = width;
    buffer.height = height;
  }

  if (!CreateTexture(width, height, true, m_scene)) return E_FAIL;

  D3D12_HEAP_PROPERTIES defaultHeap = HeapProperties(D3D12_HEAP_TYPE_DEFAULT);
  D3D12_RESOURCE_DESC depthDesc = TextureDesc(width, height, depthFormat, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL | D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE);
  D3D12_CLEAR_VALUE depthClear = {};
  depthClear.Format = depthFormat;
  depthClear.DepthStencil.Depth = 1.0f;
  hr = m_device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &depthDesc, D3D12_RESOURCE_STATE_DEPTH_WRITE, &depthClear,
                                         IID_PPV_ARGS(&m_depth.resource));
  if (FAILED(hr)) return hr;
  m_depth.state = D3D12_RESOURCE_STATE_DEPTH_WRITE;
  m_depth.width = width;
  m_depth.height = height;
  m_device->CreateDepthStencilView(m_depth.resource.Get(), nullptr, m_dsvHeap->GetCPUDescriptorHandleForHeapStart());

  if (!CreateTexture(1, 1, false, m_white)) return E_FAIL;

  hr = CreatePipelineObjects();
  if (FAILED(hr)) return hr;

  m_frame = 0;
  BeginRecording();
  const std::uint32_t white = 0xFFFFFFFFu;
  UploadTexture(m_white, &white, 1, 0, 0, 1, 1);
  Clear(true, true, 0xFF000000u);
  return S_OK;
}

void DeviceDX12::Close() {
  if (m_device && m_queue && m_fence) {
    // The list is open (BeginRecording) after a successful Initialize; drain it before teardown.
    m_list->Close();
    ++m_fenceValue;
    m_queue->Signal(m_fence.Get(), m_fenceValue);
    WaitForFence(m_fenceValue);
  }

  m_pipelines.clear();
  m_pixelShader.Reset();
  m_vertexShaderLightmapped.Reset();
  m_vertexShader.Reset();
  m_rootSignature.Reset();
  m_pipeline = nullptr;
  m_tableTextures[0] = m_tableTextures[1] = nullptr;
  m_target = nullptr;
  m_white = GpuResource();
  m_depth = GpuResource();
  m_scene = GpuResource();
  for (GpuResource& buffer : m_backBuffers) buffer = GpuResource();
  for (Frame& frame : m_frames) frame = Frame();
  m_freeSrv.clear();
  m_freeRtv.clear();
  m_nextSrv = m_nextRtv = m_gpuSrvUsed = 0;
  m_gpuSrvHeap.Reset();
  m_cpuSrvHeap.Reset();
  m_dsvHeap.Reset();
  m_rtvHeap.Reset();
  if (m_fenceEvent) {
    CloseHandle(m_fenceEvent);
    m_fenceEvent = nullptr;
  }
  m_fence.Reset();
  m_list.Reset();
  m_swapChain.Reset();
  m_queue.Reset();
  m_device.Reset();
  m_factory.Reset();
  m_fenceValue = 0;
}

HRESULT DeviceDX12::CreatePipelineObjects() {
  D3D12_ROOT_PARAMETER parameters[2] = {};
  parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  parameters[0].Constants.ShaderRegister = 0;
  parameters[0].Constants.Num32BitValues = sizeof(DrawConstants) / 4;
  parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

  D3D12_DESCRIPTOR_RANGE range = {};
  range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
  range.NumDescriptors = 2;
  range.BaseShaderRegister = 0;
  parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  parameters[1].DescriptorTable.NumDescriptorRanges = 1;
  parameters[1].DescriptorTable.pDescriptorRanges = &range;
  parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

  // s0-s5: {point, linear} x {wrap, clamp, border}; the shader picks one from DrawConstants::address.
  D3D12_STATIC_SAMPLER_DESC samplers[6] = {};
  const D3D12_TEXTURE_ADDRESS_MODE modes[3] = {D3D12_TEXTURE_ADDRESS_MODE_WRAP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_BORDER};
  for (UINT i = 0; i < 6; ++i) {
    D3D12_STATIC_SAMPLER_DESC& s = samplers[i];
    s.Filter = i < 3 ? D3D12_FILTER_MIN_MAG_MIP_POINT : D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    s.AddressU = s.AddressV = s.AddressW = modes[i % 3];
    s.MaxAnisotropy = 1;
    s.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    s.BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK;
    s.MaxLOD = D3D12_FLOAT32_MAX;
    s.ShaderRegister = i;
    s.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
  }

  D3D12_ROOT_SIGNATURE_DESC rootDesc = {};
  rootDesc.NumParameters = 2;
  rootDesc.pParameters = parameters;
  rootDesc.NumStaticSamplers = 6;
  rootDesc.pStaticSamplers = samplers;
  rootDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
  ComPtr<ID3DBlob> serialized;
  ComPtr<ID3DBlob> errors;
  HRESULT hr = D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors);
  if (FAILED(hr)) return hr;
  hr = m_device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(), IID_PPV_ARGS(&m_rootSignature));
  if (FAILED(hr)) return hr;

  const D3D_SHADER_MACRO lightmapped[] = {{"LIGHTMAPPED", "1"}, {nullptr, nullptr}};
  hr = Compile("VSMain", "vs_5_0", nullptr, m_vertexShader);
  if (FAILED(hr)) return hr;
  hr = Compile("VSMain", "vs_5_0", lightmapped, m_vertexShaderLightmapped);
  if (FAILED(hr)) return hr;
  return Compile("PSMain", "ps_5_0", nullptr, m_pixelShader);
}

ID3D12PipelineState* DeviceDX12::Pipeline(std::uint32_t key) {
  auto found = m_pipelines.find(key);
  if (found != m_pipelines.end()) {
    return found->second.Get();
  }

  const bool lightmapped = (key & keyLightmapped) != 0;
  const D3D12_INPUT_ELEMENT_DESC elements[] = {
      {"POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
      {"COLOR", 0, DXGI_FORMAT_B8G8R8A8_UNORM, 0, 16, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
      {"COLOR", 1, DXGI_FORMAT_B8G8R8A8_UNORM, 0, 20, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
      {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
      {"TEXCOORD", 1, DXGI_FORMAT_R32G32_FLOAT, 0, 32, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
  };
  ID3DBlob* vertexShader = lightmapped ? m_vertexShaderLightmapped.Get() : m_vertexShader.Get();

  D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = {};
  desc.pRootSignature = m_rootSignature.Get();
  desc.VS = {vertexShader->GetBufferPointer(), vertexShader->GetBufferSize()};
  desc.PS = {m_pixelShader->GetBufferPointer(), m_pixelShader->GetBufferSize()};
  desc.InputLayout = {elements, lightmapped ? 5u : 4u};

  D3D12_RENDER_TARGET_BLEND_DESC& blend = desc.BlendState.RenderTarget[0];
  blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  blend.LogicOp = D3D12_LOGIC_OP_NOOP;
  if (key & keyBlend) {
    const std::uint32_t op = (key >> keyOpShift) & 7;
    blend.BlendEnable = TRUE;
    blend.SrcBlend = ColorBlend((key >> keySrcShift) & 15, true);
    blend.DestBlend = ColorBlend((key >> keyDstShift) & 15, false);
    blend.BlendOp = op ? static_cast<D3D12_BLEND_OP>(op) : D3D12_BLEND_OP_ADD;
    blend.SrcBlendAlpha = AlphaBlend(blend.SrcBlend);
    blend.DestBlendAlpha = AlphaBlend(blend.DestBlend);
    blend.BlendOpAlpha = blend.BlendOp;
  } else {
    blend.SrcBlend = blend.SrcBlendAlpha = D3D12_BLEND_ONE;
    blend.DestBlend = blend.DestBlendAlpha = D3D12_BLEND_ZERO;
    blend.BlendOp = blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
  }

  desc.SampleMask = UINT_MAX;
  desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
  switch ((key >> keyCullShift) & 3) {
    case D3DCULL_CW:
      desc.RasterizerState.CullMode = D3D12_CULL_MODE_FRONT;
      break;
    case D3DCULL_CCW:
      desc.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
      break;
    default:
      desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
      break;
  }
  desc.RasterizerState.DepthClipEnable = TRUE;

  const bool textureTarget = (key & keyTextureTarget) != 0;
  if (!textureTarget && (key & keyZEnable)) {
    const std::uint32_t func = (key >> keyZFuncShift) & 15;
    desc.DepthStencilState.DepthEnable = TRUE;
    desc.DepthStencilState.DepthWriteMask = (key & keyZWrite) ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
    desc.DepthStencilState.DepthFunc = func ? static_cast<D3D12_COMPARISON_FUNC>(func) : D3D12_COMPARISON_FUNC_LESS_EQUAL;
  }

  switch ((key >> keyTopologyShift) & 3) {
    case 1:
      desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
      break;
    case 2:
      desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
      break;
    default:
      desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
      break;
  }
  desc.NumRenderTargets = 1;
  desc.RTVFormats[0] = colorFormat;
  desc.DSVFormat = textureTarget ? DXGI_FORMAT_UNKNOWN : depthFormat;
  desc.SampleDesc.Count = 1;

  ComPtr<ID3D12PipelineState> pipeline;
  if (FAILED(m_device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pipeline)))) {
    return nullptr;
  }
  ID3D12PipelineState* result = pipeline.Get();
  m_pipelines.emplace(key, std::move(pipeline));
  return result;
}

DeviceDX12::Allocation DeviceDX12::Allocate(UINT64 size, UINT64 alignment) {
  Frame& frame = m_frames[m_frame];
  for (UploadPage& page : frame.pages) {
    const UINT64 offset = (page.used + alignment - 1) & ~(alignment - 1);
    if (offset + size <= page.size) {
      page.used = offset + size;
      return {page.cpu + offset, page.gpu + offset, page.buffer.Get(), offset};
    }
  }

  UploadPage page;
  page.size = std::max(uploadPageSize, (size + alignment + 0xFFFF) & ~UINT64(0xFFFF));
  D3D12_HEAP_PROPERTIES uploadHeap = HeapProperties(D3D12_HEAP_TYPE_UPLOAD);
  D3D12_RESOURCE_DESC desc = BufferDesc(page.size);
  m_device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&page.buffer));
  D3D12_RANGE noRead = {0, 0};
  void* cpu = nullptr;
  page.buffer->Map(0, &noRead, &cpu);
  page.cpu = static_cast<std::uint8_t*>(cpu);
  page.gpu = page.buffer->GetGPUVirtualAddress();
  page.used = size;  // a fresh page starts at offset 0, which satisfies every alignment
  frame.pages.push_back(std::move(page));
  UploadPage& added = frame.pages.back();
  return {added.cpu, added.gpu, added.buffer.Get(), 0};
}

void DeviceDX12::Transition(GpuResource& resource, D3D12_RESOURCE_STATES next) {
  if (resource.state == next) {
    return;
  }
  const D3D12_RESOURCE_BARRIER barrier = TransitionBarrier(resource.resource.Get(), resource.state, next);
  m_list->ResourceBarrier(1, &barrier);
  resource.state = next;
  GpuResource* target = m_target ? m_target : &m_scene;
  if (&resource == target && next != D3D12_RESOURCE_STATE_RENDER_TARGET) {
    m_targetBound = false;
  }
}

void DeviceDX12::BeginRecording() {
  Frame& frame = m_frames[m_frame];
  WaitForFence(frame.fence);
  frame.allocator->Reset();
  m_list->Reset(frame.allocator.Get(), nullptr);
  for (UploadPage& page : frame.pages) {
    page.used = 0;
  }
  frame.retired.clear();
  m_gpuSrvUsed = 0;
  m_tableTextures[0] = m_tableTextures[1] = nullptr;
  m_targetBound = false;
  m_pipeline = nullptr;

  ID3D12DescriptorHeap* heaps[] = {m_gpuSrvHeap.Get()};
  m_list->SetDescriptorHeaps(1, heaps);
  m_list->SetGraphicsRootSignature(m_rootSignature.Get());
}

void DeviceDX12::Submit() {
  m_list->Close();
  ID3D12CommandList* lists[] = {m_list.Get()};
  m_queue->ExecuteCommandLists(1, lists);
  ++m_fenceValue;
  m_queue->Signal(m_fence.Get(), m_fenceValue);
  m_frames[m_frame].fence = m_fenceValue;
}

void DeviceDX12::WaitForFence(UINT64 value) {
  if (m_fence->GetCompletedValue() < value) {
    m_fence->SetEventOnCompletion(value, m_fenceEvent);
    WaitForSingleObject(m_fenceEvent, INFINITE);
  }
}

void DeviceDX12::Flush() {
  Submit();
  WaitForFence(m_fenceValue);
  BeginRecording();
}

HRESULT DeviceDX12::Present() {
  const UINT index = m_swapChain->GetCurrentBackBufferIndex();
  GpuResource& backBuffer = m_backBuffers[index];
  Transition(m_scene, D3D12_RESOURCE_STATE_COPY_SOURCE);
  Transition(backBuffer, D3D12_RESOURCE_STATE_COPY_DEST);
  m_list->CopyResource(backBuffer.resource.Get(), m_scene.resource.Get());
  Transition(backBuffer, D3D12_RESOURCE_STATE_PRESENT);
  Submit();

  const HRESULT hr = m_swapChain->Present(1, 0);
  m_frame = (m_frame + 1) % FrameCount;
  BeginRecording();
  return hr;
}

void DeviceDX12::SetRenderTarget(GpuResource* target) {
  if (target == &m_scene) {
    target = nullptr;
  }
  if (target != m_target) {
    m_target = target;
    m_targetBound = false;
  }
}

void DeviceDX12::BindTarget() {
  GpuResource& target = m_target ? *m_target : m_scene;
  Transition(target, D3D12_RESOURCE_STATE_RENDER_TARGET);
  if (m_targetBound) {
    return;
  }

  D3D12_CPU_DESCRIPTOR_HANDLE rtv = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
  rtv.ptr += SIZE_T(target.rtv) * m_rtvSize;
  if (m_target) {
    m_list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
  } else {
    Transition(m_depth, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    const D3D12_CPU_DESCRIPTOR_HANDLE dsv = m_dsvHeap->GetCPUDescriptorHandleForHeapStart();
    m_list->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
  }

  const D3D12_VIEWPORT viewport = {0.0f, 0.0f, float(target.width), float(target.height), 0.0f, 1.0f};
  const D3D12_RECT scissor = {0, 0, LONG(target.width), LONG(target.height)};
  m_list->RSSetViewports(1, &viewport);
  m_list->RSSetScissorRects(1, &scissor);
  m_targetBound = true;
}

void DeviceDX12::Clear(bool color, bool depth, DWORD argb) {
  BindTarget();
  if (color) {
    GpuResource& target = m_target ? *m_target : m_scene;
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += SIZE_T(target.rtv) * m_rtvSize;
    const float rgba[4] = {((argb >> 16) & 0xFF) / 255.0f, ((argb >> 8) & 0xFF) / 255.0f, (argb & 0xFF) / 255.0f, (argb >> 24) / 255.0f};
    m_list->ClearRenderTargetView(rtv, rgba, 0, nullptr);
  }
  if (depth && !m_target) {
    m_list->ClearDepthStencilView(m_dsvHeap->GetCPUDescriptorHandleForHeapStart(), D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
  }
}

GpuResource* DeviceDX12::Bound(UINT stage) {
  CTextureDX12* texture = state.textures[stage];
  GpuResource* resource = texture ? texture->Resource() : nullptr;
  return resource && resource->resource ? resource : &m_white;
}

void DeviceDX12::Draw(D3DPRIMITIVETYPE type, UINT primitiveCount, const void* vertices, UINT vertexCount, UINT stride, const std::uint16_t* indices,
                      UINT indexCount, bool lightmapped) {
  if (primitiveCount == 0 || vertexCount == 0) {
    return;
  }

  D3D_PRIMITIVE_TOPOLOGY topology;
  std::uint32_t topologyType = 0;
  UINT count;
  switch (type) {
    case D3DPT_POINTLIST:
      topology = D3D_PRIMITIVE_TOPOLOGY_POINTLIST;
      topologyType = 2;
      count = primitiveCount;
      break;
    case D3DPT_LINELIST:
      topology = D3D_PRIMITIVE_TOPOLOGY_LINELIST;
      topologyType = 1;
      count = primitiveCount * 2;
      break;
    case D3DPT_LINESTRIP:
      topology = D3D_PRIMITIVE_TOPOLOGY_LINESTRIP;
      topologyType = 1;
      count = primitiveCount + 1;
      break;
    case D3DPT_TRIANGLESTRIP:
      topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
      count = primitiveCount + 2;
      break;
    case D3DPT_TRIANGLEFAN:
      topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
      count = primitiveCount * 3;
      break;
    default:
      topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
      count = primitiveCount * 3;
      break;
  }

  if (type == D3DPT_TRIANGLEFAN) {
    m_fanIndices.resize(count);
    for (UINT i = 0; i < primitiveCount; ++i) {
      std::uint16_t* triangle = &m_fanIndices[i * 3];
      if (indices) {
        triangle[0] = indices[0];
        triangle[1] = indices[i + 1];
        triangle[2] = indices[i + 2];
      } else {
        triangle[0] = 0;
        triangle[1] = std::uint16_t(i + 1);
        triangle[2] = std::uint16_t(i + 2);
      }
    }
    indices = m_fanIndices.data();
  } else if (indices) {
    count = std::min(count, indexCount);
  }

  const DWORD* rs = state.render;
  const bool textureTarget = m_target != nullptr;
  std::uint32_t key = (topologyType << keyTopologyShift) | ((rs[D3DRS_CULLMODE] & 3) << keyCullShift);
  if (rs[D3DRS_ALPHABLENDENABLE]) {
    key |= keyBlend | ((rs[D3DRS_SRCBLEND] & 15) << keySrcShift) | ((rs[D3DRS_DESTBLEND] & 15) << keyDstShift) | ((rs[D3DRS_BLENDOP] & 7) << keyOpShift);
  }
  if (!textureTarget && rs[D3DRS_ZENABLE]) {
    key |= keyZEnable | ((rs[D3DRS_ZFUNC] & 15) << keyZFuncShift);
    if (rs[D3DRS_ZWRITEENABLE]) {
      key |= keyZWrite;
    }
  }
  if (lightmapped) key |= keyLightmapped;
  if (textureTarget) key |= keyTextureTarget;

  const GpuResource& target = textureTarget ? *m_target : m_scene;
  DrawConstants constants;
  constants.targetWidth = float(target.width);
  constants.targetHeight = float(target.height);
  for (UINT stage = 0; stage < 2; ++stage) {
    const DWORD* ts = state.textureStage[stage];
    const DWORD* ss = state.sampler[stage];
    constants.stage[stage][0] = ts[D3DTSS_COLOROP];
    constants.stage[stage][1] = ts[D3DTSS_COLORARG1];
    constants.stage[stage][2] = ts[D3DTSS_COLORARG2];
    constants.stage[stage][3] = ts[D3DTSS_ALPHAOP];
    constants.stage[stage][4] = ts[D3DTSS_ALPHAARG1];
    constants.stage[stage][5] = ts[D3DTSS_ALPHAARG2];
    const bool linearFilter = ss[D3DSAMP_MAGFILTER] >= D3DTEXF_LINEAR || ss[D3DSAMP_MINFILTER] >= D3DTEXF_LINEAR;
    constants.address[stage] = (ss[D3DSAMP_ADDRESSU] & 0xFF) | (linearFilter ? 0x100u : 0u);
    constants.coordinate[stage] = lightmapped ? (ts[D3DTSS_TEXCOORDINDEX] & 1) : 0;
  }
  constants.alphaTest = rs[D3DRS_ALPHATESTENABLE] ? 1 : 0;
  constants.alphaFunction = rs[D3DRS_ALPHAFUNC];
  constants.alphaReference = rs[D3DRS_ALPHAREF] & 0xFF;
  constants.fog = ((rs[D3DRS_FOGENABLE] && rs[D3DRS_FOGTABLEMODE] == D3DFOG_NONE) ? 1u : 0u) | (rs[D3DRS_SPECULARENABLE] ? 2u : 0u);
  constants.fogColor = rs[D3DRS_FOGCOLOR];
  constants.textureFactor = rs[D3DRS_TEXTUREFACTOR];

  DrawRaw(topology, count, vertices, vertexCount, stride, indices, key, constants, Bound(0), Bound(1));
}

void DeviceDX12::DrawRaw(D3D_PRIMITIVE_TOPOLOGY topology, UINT count, const void* vertices, UINT vertexCount, UINT stride,
                         const std::uint16_t* indices, std::uint32_t key, const DrawConstants& constants, GpuResource* texture0,
                         GpuResource* texture1) {
  ID3D12PipelineState* pipeline = Pipeline(key);
  if (!pipeline) {
    return;
  }

  // Descriptors first: running out flushes, which recycles this frame's upload memory.
  if (m_gpuSrvUsed < 2 || m_tableTextures[0] != texture0 || m_tableTextures[1] != texture1) {
    if (m_gpuSrvUsed + 2 > gpuSrvPerFrame) {
      Flush();
    }
    const UINT base = m_frame * gpuSrvPerFrame + m_gpuSrvUsed;
    D3D12_CPU_DESCRIPTOR_HANDLE destination = m_gpuSrvHeap->GetCPUDescriptorHandleForHeapStart();
    destination.ptr += SIZE_T(base) * m_srvSize;
    GpuResource* textures[2] = {texture0, texture1};
    for (GpuResource* texture : textures) {
      D3D12_CPU_DESCRIPTOR_HANDLE source = m_cpuSrvHeap->GetCPUDescriptorHandleForHeapStart();
      source.ptr += SIZE_T(texture->srv) * m_srvSize;
      m_device->CopyDescriptorsSimple(1, destination, source, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
      destination.ptr += m_srvSize;
    }
    m_gpuSrvUsed += 2;
    m_tableTextures[0] = texture0;
    m_tableTextures[1] = texture1;
    D3D12_GPU_DESCRIPTOR_HANDLE table = m_gpuSrvHeap->GetGPUDescriptorHandleForHeapStart();
    table.ptr += UINT64(base) * m_srvSize;
    m_list->SetGraphicsRootDescriptorTable(1, table);
  }

  Transition(*texture0, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
  Transition(*texture1, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
  BindTarget();

  if (pipeline != m_pipeline) {
    m_list->SetPipelineState(pipeline);
    m_pipeline = pipeline;
  }
  m_list->SetGraphicsRoot32BitConstants(0, sizeof(DrawConstants) / 4, &constants, 0);
  m_list->IASetPrimitiveTopology(topology);

  const UINT vertexBytes = vertexCount * stride;
  const Allocation vertexMemory = Allocate(vertexBytes, 16);
  std::memcpy(vertexMemory.cpu, vertices, vertexBytes);
  const D3D12_VERTEX_BUFFER_VIEW vertexView = {vertexMemory.gpu, vertexBytes, stride};
  m_list->IASetVertexBuffers(0, 1, &vertexView);

  if (indices) {
    const UINT indexBytes = count * sizeof(std::uint16_t);
    const Allocation indexMemory = Allocate(indexBytes, 4);
    std::memcpy(indexMemory.cpu, indices, indexBytes);
    const D3D12_INDEX_BUFFER_VIEW indexView = {indexMemory.gpu, (indexBytes + 3) & ~3u, DXGI_FORMAT_R16_UINT};
    m_list->IASetIndexBuffer(&indexView);
    m_list->DrawIndexedInstanced(count, 1, 0, 0, 0);
  } else {
    m_list->DrawInstanced(count, 1, 0, 0);
  }
}

void DeviceDX12::Blit(GpuResource& source, const RECT& from, const RECT& to) {
  struct Vertex {
    float x, y, z, rhw;
    DWORD diffuse, specular;
    float u, v;
  };
  const float sw = float(source.width);
  const float sh = float(source.height);
  // D3D9 pixel-edge coordinates: the shader's +0.5 lands these on the rectangle borders.
  const float left = float(to.left) - 0.5f;
  const float top = float(to.top) - 0.5f;
  const float right = float(to.right) - 0.5f;
  const float bottom = float(to.bottom) - 0.5f;
  const Vertex quad[4] = {
      {left, top, 0.0f, 1.0f, 0xFFFFFFFFu, 0, from.left / sw, from.top / sh},
      {right, top, 0.0f, 1.0f, 0xFFFFFFFFu, 0, from.right / sw, from.top / sh},
      {left, bottom, 0.0f, 1.0f, 0xFFFFFFFFu, 0, from.left / sw, from.bottom / sh},
      {right, bottom, 0.0f, 1.0f, 0xFFFFFFFFu, 0, from.right / sw, from.bottom / sh},
  };

  const bool stretched = (from.right - from.left) != (to.right - to.left) || (from.bottom - from.top) != (to.bottom - to.top);
  DrawConstants constants = {};
  constants.targetWidth = float(m_scene.width);
  constants.targetHeight = float(m_scene.height);
  constants.stage[0][0] = D3DTOP_SELECTARG1;
  constants.stage[0][1] = D3DTA_TEXTURE;
  constants.stage[0][2] = D3DTA_DIFFUSE;
  constants.stage[0][3] = D3DTOP_SELECTARG1;
  constants.stage[0][4] = D3DTA_TEXTURE;
  constants.stage[0][5] = D3DTA_DIFFUSE;
  constants.stage[1][0] = D3DTOP_DISABLE;
  constants.stage[1][3] = D3DTOP_DISABLE;
  constants.address[0] = D3DTADDRESS_CLAMP | (stretched ? 0x100u : 0u);
  constants.address[1] = D3DTADDRESS_CLAMP;
  constants.alphaFunction = D3DCMP_ALWAYS;
  constants.textureFactor = 0xFFFFFFFFu;

  GpuResource* previous = m_target;
  SetRenderTarget(nullptr);
  DrawRaw(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP, 4, quad, 4, sizeof(Vertex), nullptr, std::uint32_t(D3DCULL_NONE) << keyCullShift, constants,
          &source, &m_white);
  SetRenderTarget(previous);
}

UINT DeviceDX12::AllocateDescriptor(std::vector<UINT>& free, UINT& next, UINT capacity) {
  if (!free.empty()) {
    const UINT index = free.back();
    free.pop_back();
    return index;
  }
  return next < capacity ? next++ : UINT(-1);
}

bool DeviceDX12::CreateTexture(UINT textureWidth, UINT textureHeight, bool renderTarget, GpuResource& output) {
  D3D12_HEAP_PROPERTIES defaultHeap = HeapProperties(D3D12_HEAP_TYPE_DEFAULT);
  D3D12_RESOURCE_DESC desc = TextureDesc(textureWidth, textureHeight, colorFormat,
                                         renderTarget ? D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET : D3D12_RESOURCE_FLAG_NONE);
  D3D12_CLEAR_VALUE clear = {};
  clear.Format = colorFormat;
  ComPtr<ID3D12Resource> resource;
  if (FAILED(m_device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON,
                                               renderTarget ? &clear : nullptr, IID_PPV_ARGS(&resource)))) {
    return false;
  }

  const UINT srv = AllocateDescriptor(m_freeSrv, m_nextSrv, cpuSrvCapacity);
  if (srv == UINT(-1)) {
    return false;
  }
  UINT rtv = UINT(-1);
  if (renderTarget) {
    rtv = AllocateDescriptor(m_freeRtv, m_nextRtv, rtvCapacity);
    if (rtv == UINT(-1)) {
      m_freeSrv.push_back(srv);
      return false;
    }
    D3D12_CPU_DESCRIPTOR_HANDLE handle = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += SIZE_T(rtv) * m_rtvSize;
    m_device->CreateRenderTargetView(resource.Get(), nullptr, handle);
  }
  D3D12_CPU_DESCRIPTOR_HANDLE handle = m_cpuSrvHeap->GetCPUDescriptorHandleForHeapStart();
  handle.ptr += SIZE_T(srv) * m_srvSize;
  m_device->CreateShaderResourceView(resource.Get(), nullptr, handle);

  output.resource = std::move(resource);
  output.state = D3D12_RESOURCE_STATE_COMMON;
  output.width = textureWidth;
  output.height = textureHeight;
  output.srv = srv;
  output.rtv = rtv;
  return true;
}

void DeviceDX12::ReleaseTexture(GpuResource& texture) {
  if (&texture == m_target) {
    SetRenderTarget(nullptr);
  }
  if (m_tableTextures[0] == &texture || m_tableTextures[1] == &texture) {
    m_tableTextures[0] = m_tableTextures[1] = nullptr;
  }
  if (texture.resource) {
    m_frames[m_frame].retired.push_back(std::move(texture.resource));
  }
  if (texture.srv != UINT(-1)) m_freeSrv.push_back(texture.srv);
  if (texture.rtv != UINT(-1)) m_freeRtv.push_back(texture.rtv);
  texture = GpuResource();
}

void DeviceDX12::UploadTexture(GpuResource& texture, const std::uint32_t* pixels, UINT pitchPixels, UINT x, UINT y, UINT copyWidth, UINT copyHeight) {
  if (!texture.resource || copyWidth == 0 || copyHeight == 0) {
    return;
  }
  const UINT rowPitch = (copyWidth * 4 + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) & ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);
  const Allocation memory = Allocate(UINT64(rowPitch) * copyHeight, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
  for (UINT row = 0; row < copyHeight; ++row) {
    std::memcpy(memory.cpu + SIZE_T(row) * rowPitch, pixels + SIZE_T(row) * pitchPixels, copyWidth * 4);
  }

  D3D12_TEXTURE_COPY_LOCATION destination = {};
  destination.pResource = texture.resource.Get();
  destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  D3D12_TEXTURE_COPY_LOCATION source = {};
  source.pResource = memory.buffer;
  source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  source.PlacedFootprint.Offset = memory.offset;
  source.PlacedFootprint.Footprint = {colorFormat, copyWidth, copyHeight, 1, rowPitch};
  Transition(texture, D3D12_RESOURCE_STATE_COPY_DEST);
  m_list->CopyTextureRegion(&destination, x, y, 0, &source, nullptr);
}

void DeviceDX12::CopyTexture(GpuResource& destination, GpuResource& source) {
  if (!destination.resource || !source.resource || &destination == &source) {
    return;
  }
  Transition(source, D3D12_RESOURCE_STATE_COPY_SOURCE);
  Transition(destination, D3D12_RESOURCE_STATE_COPY_DEST);
  if (destination.width == source.width && destination.height == source.height) {
    m_list->CopyResource(destination.resource.Get(), source.resource.Get());
    return;
  }
  D3D12_TEXTURE_COPY_LOCATION to = {};
  to.pResource = destination.resource.Get();
  to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  D3D12_TEXTURE_COPY_LOCATION from = {};
  from.pResource = source.resource.Get();
  from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  const D3D12_BOX box = {0, 0, 0, std::min(destination.width, source.width), std::min(destination.height, source.height), 1};
  m_list->CopyTextureRegion(&to, 0, 0, 0, &from, &box);
}

std::vector<std::uint32_t> DeviceDX12::ReadScene() {
  const UINT rowPitch = (width * 4 + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) & ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);
  D3D12_HEAP_PROPERTIES readbackHeap = HeapProperties(D3D12_HEAP_TYPE_READBACK);
  D3D12_RESOURCE_DESC desc = BufferDesc(UINT64(rowPitch) * height);
  ComPtr<ID3D12Resource> readback;
  if (FAILED(m_device->CreateCommittedResource(&readbackHeap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                               IID_PPV_ARGS(&readback)))) {
    return {};
  }

  D3D12_TEXTURE_COPY_LOCATION destination = {};
  destination.pResource = readback.Get();
  destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  destination.PlacedFootprint.Footprint = {colorFormat, width, height, 1, rowPitch};
  D3D12_TEXTURE_COPY_LOCATION source = {};
  source.pResource = m_scene.resource.Get();
  source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  Transition(m_scene, D3D12_RESOURCE_STATE_COPY_SOURCE);
  m_list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
  Flush();

  std::vector<std::uint32_t> pixels(SIZE_T(width) * height);
  const D3D12_RANGE range = {0, SIZE_T(desc.Width)};
  void* mapped = nullptr;
  if (SUCCEEDED(readback->Map(0, &range, &mapped))) {
    const std::uint8_t* rows = static_cast<const std::uint8_t*>(mapped);
    for (UINT row = 0; row < height; ++row) {
      std::memcpy(&pixels[SIZE_T(row) * width], rows + SIZE_T(row) * rowPitch, SIZE_T(width) * 4);
    }
    const D3D12_RANGE written = {0, 0};
    readback->Unmap(0, &written);
  }
  return pixels;
}

void DeviceDX12::WriteScene(const std::uint32_t* pixels) {
  UploadTexture(m_scene, pixels, width, 0, 0, width, height);
}
