// Port of the game's DX9 queue drawing (TextureModesDX9, QueuedDrawingDX9, Opaque/Alpha/GroundQueuesDX9): the same
// DeviceStateDX9 calls, recorded by DeviceStateDX12.
#include "RendererDX12.h"

#include "Contents/Configuration/ServiceOptions.h"
#include "Render/Primitives/RenderQueues.h"
#include "Render/Primitives/TextureQuad.h"
#include "UI/GameSettings/TalkType.h"

#include <algorithm>
#include <cstdint>

namespace {
  bool Before(const DepthPrimitive& left, const DepthPrimitive& right) {
    if (left.depth < right.depth) {
      return true;
    }

    if (right.depth < left.depth) {
      return false;
    }

    return left.depth == right.depth && reinterpret_cast<std::uintptr_t>(left.primitive) < reinterpret_cast<std::uintptr_t>(right.primitive);
  }

  bool WritesDepth(const RenderCommand& command) {
    return (command.depthFlags & 0xFFu) != 0 && (command.depthFlags & 0xFF00u) != 0;
  }
}


void CRendererDX12::SetTextureMode(int mode) {
  switch (mode) {
    case 0:
      this->device->state.SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0);
      this->device->state.SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
      this->device->state.SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
      this->device->state.SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
      this->device->state.SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
      this->device->state.SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
      this->device->state.SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
      this->device->state.SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
      this->device->state.SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
      break;
    case 1:
    case 2:
      this->device->state.SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0);
      this->device->state.SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
      this->device->state.SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
      this->device->state.SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
      this->device->state.SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
      this->device->state.SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
      this->device->state.SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
      this->device->state.SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
      break;
    case 3:
      this->device->state.SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0);
      this->device->state.SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
      this->device->state.SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_ADD);
      this->device->state.SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
      this->device->state.SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG2);
      this->device->state.SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
      this->device->state.SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
      this->device->state.SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
      break;
    case 4:
      this->device->state.SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0);
      this->device->state.SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
      this->device->state.SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_BLENDTEXTUREALPHA);
      this->device->state.SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
      this->device->state.SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG2);
      this->device->state.SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
      this->device->state.SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
      this->device->state.SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
      break;
    case 5:
      this->device->state.SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0);
      this->device->state.SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
      this->device->state.SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
      this->device->state.SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
      this->device->state.SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG2);
      this->device->state.SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
      this->device->state.SetTextureStageState(1, D3DTSS_TEXCOORDINDEX, 1);
      this->device->state.SetTextureStageState(1, D3DTSS_COLORARG1, D3DTA_TEXTURE);
      this->device->state.SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_MODULATE);
      this->device->state.SetTextureStageState(1, D3DTSS_COLORARG2, D3DTA_CURRENT);
      this->device->state.SetTextureStageState(1, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
      this->device->state.SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
      break;
    case 6:
      this->device->state.SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0);
      this->device->state.SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE | D3DTA_COMPLEMENT);
      this->device->state.SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
      this->device->state.SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
      this->device->state.SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG2);
      this->device->state.SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
      this->device->state.SetTextureStageState(1, D3DTSS_TEXCOORDINDEX, 1);
      this->device->state.SetTextureStageState(1, D3DTSS_COLORARG1, D3DTA_TEXTURE | D3DTA_COMPLEMENT);
      this->device->state.SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_MODULATE);
      this->device->state.SetTextureStageState(1, D3DTSS_COLORARG2, D3DTA_CURRENT);
      this->device->state.SetTextureStageState(1, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
      this->device->state.SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
      break;
    case 7:
      this->device->state.SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0);
      this->device->state.SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
      this->device->state.SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
      this->device->state.SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
      this->device->state.SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
      this->device->state.SetTextureStageState(1, D3DTSS_TEXCOORDINDEX, 1);
      this->device->state.SetTextureStageState(1, D3DTSS_COLORARG1, D3DTA_TEXTURE | D3DTA_ALPHAREPLICATE);
      this->device->state.SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_MODULATE);
      this->device->state.SetTextureStageState(1, D3DTSS_COLORARG2, D3DTA_CURRENT);
      this->device->state.SetTextureStageState(1, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
      this->device->state.SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
      break;
    case 8:
      this->device->state.SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0);
      this->device->state.SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
      this->device->state.SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
      this->device->state.SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
      this->device->state.SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
      this->device->state.SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
      this->device->state.SetTextureStageState(1, D3DTSS_TEXCOORDINDEX, 1);
      this->device->state.SetTextureStageState(1, D3DTSS_COLORARG1, D3DTA_TEXTURE);
      this->device->state.SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_ADD);
      this->device->state.SetTextureStageState(1, D3DTSS_COLORARG2, D3DTA_CURRENT);
      this->device->state.SetTextureStageState(1, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
      this->device->state.SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
      break;
    case 9:
      this->device->state.SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0);
      this->device->state.SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
      this->device->state.SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
      this->device->state.SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
      this->device->state.SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG2);
      this->device->state.SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
      this->device->state.SetTextureStageState(1, D3DTSS_TEXCOORDINDEX, 1);
      this->device->state.SetTextureStageState(1, D3DTSS_COLORARG1, D3DTA_TEXTURE);
      this->device->state.SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_BLENDTEXTUREALPHA);
      this->device->state.SetTextureStageState(1, D3DTSS_COLORARG2, D3DTA_CURRENT);
      this->device->state.SetTextureStageState(1, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
      this->device->state.SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
      break;
    case 10:
      this->device->state.SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0);
      this->device->state.SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
      this->device->state.SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
      this->device->state.SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
      this->device->state.SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG2);
      this->device->state.SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
      this->device->state.SetTextureStageState(1, D3DTSS_TEXCOORDINDEX, 1);
      this->device->state.SetTextureStageState(1, D3DTSS_COLORARG1, D3DTA_TEXTURE);
      this->device->state.SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_BLENDDIFFUSEALPHA);
      this->device->state.SetTextureStageState(1, D3DTSS_COLORARG2, D3DTA_CURRENT);
      this->device->state.SetTextureStageState(1, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
      this->device->state.SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
      break;
    case 11:
      this->device->state.SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0);
      this->device->state.SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
      this->device->state.SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
      this->device->state.SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
      this->device->state.SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG2);
      this->device->state.SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
      this->device->state.SetTextureStageState(1, D3DTSS_TEXCOORDINDEX, 1);
      this->device->state.SetTextureStageState(1, D3DTSS_COLORARG1, D3DTA_TEXTURE);
      this->device->state.SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_ADD);
      this->device->state.SetTextureStageState(1, D3DTSS_COLORARG2, D3DTA_CURRENT);
      this->device->state.SetTextureStageState(1, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
      this->device->state.SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
      break;
    case 12:
      this->device->state.SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0);
      this->device->state.SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
      this->device->state.SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
      this->device->state.SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
      this->device->state.SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG2);
      this->device->state.SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
      this->device->state.SetTextureStageState(1, D3DTSS_TEXCOORDINDEX, 1);
      this->device->state.SetTextureStageState(1, D3DTSS_COLORARG1, D3DTA_TEXTURE | D3DTA_COMPLEMENT);
      this->device->state.SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_ADD);
      this->device->state.SetTextureStageState(1, D3DTSS_COLORARG2, D3DTA_CURRENT);
      this->device->state.SetTextureStageState(1, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
      this->device->state.SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
      break;
    case 13:
      this->device->state.SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0);
      this->device->state.SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
      this->device->state.SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_ADD);
      this->device->state.SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
      this->device->state.SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
      this->device->state.SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
      this->device->state.SetTextureStageState(1, D3DTSS_TEXCOORDINDEX, 1);
      this->device->state.SetTextureStageState(1, D3DTSS_COLORARG1, D3DTA_TEXTURE);
      this->device->state.SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_MODULATE);
      this->device->state.SetTextureStageState(1, D3DTSS_COLORARG2, D3DTA_CURRENT);
      this->device->state.SetTextureStageState(1, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
      this->device->state.SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
      break;
    case 14:
      this->device->state.SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0);
      this->device->state.SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
      this->device->state.SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
      this->device->state.SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
      this->device->state.SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG2);
      this->device->state.SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
      this->device->state.SetTextureStageState(1, D3DTSS_TEXCOORDINDEX, 1);
      this->device->state.SetTextureStageState(1, D3DTSS_COLORARG1, D3DTA_TEXTURE);
      this->device->state.SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_MODULATE2X);
      this->device->state.SetTextureStageState(1, D3DTSS_COLORARG2, D3DTA_CURRENT);
      this->device->state.SetTextureStageState(1, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
      this->device->state.SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
      break;
    case 15:
      this->device->state.SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0);
      this->device->state.SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
      this->device->state.SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
      this->device->state.SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
      this->device->state.SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG2);
      this->device->state.SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
      this->device->state.SetTextureStageState(1, D3DTSS_TEXCOORDINDEX, 1);
      this->device->state.SetTextureStageState(1, D3DTSS_COLORARG1, D3DTA_TEXTURE);
      this->device->state.SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_ADDSIGNED);
      this->device->state.SetTextureStageState(1, D3DTSS_COLORARG2, D3DTA_CURRENT);
      this->device->state.SetTextureStageState(1, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
      this->device->state.SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
      break;
  }
}

void CRendererDX12::DrawAuxiliaryQueue() {
  auto& queue = this->queue_0240;

  if (queue.empty()) {
    return;
  }

  ScopedDeviceStateDX12 restore(this->device->state);
  std::sort(queue.begin(), queue.end(), [](const LightmappedCommand* left, const LightmappedCommand* right) {
    const auto first = reinterpret_cast<std::uintptr_t>(left->texture);
    const auto second = reinterpret_cast<std::uintptr_t>(right->texture);

    if (first != second) {
      return first < second;
    }

    return reinterpret_cast<std::uintptr_t>(left->lightmap) < reinterpret_cast<std::uintptr_t>(right->lightmap);
  });
  this->device->state.SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
  this->device->state.SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
  this->device->state.SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
  this->device->state.SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
  this->device->state.SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_MODULATE);
  this->device->state.SetTextureStageState(1, D3DTSS_COLORARG1, D3DTA_TEXTURE);
  this->device->state.SetTextureStageState(1, D3DTSS_COLORARG2, D3DTA_CURRENT);
  this->device->state.SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
  this->device->state.SetTextureStageState(1, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
  this->device->state.SetTextureStageState(1, D3DTSS_ALPHAARG2, D3DTA_CURRENT);
  this->device->state.SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP);
  this->device->state.SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP);
  this->device->state.SetSamplerState(1, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
  this->device->state.SetSamplerState(1, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);

  for (LightmappedCommand*& command : queue) {
    if (command->vertexCount > 0) {
      this->device->state.SetTexture(0, command->texture ? static_cast<CTextureDX12*>(command->texture) : nullptr);
      this->device->state.SetTexture(1, command->lightmap ? static_cast<CTextureDX12*>(command->lightmap) : nullptr);
      this->DrawAuxiliaryPrimitive(command);
    }
  }
}

void CRendererDX12::DrawQueues() {
  this->ApplyFog();
  this->device->state.SetRenderState(D3DRS_ALPHATESTENABLE, 1);
  this->device->state.SetRenderState(D3DRS_ALPHAREF, 207);
  this->device->state.SetRenderState(D3DRS_ALPHABLENDENABLE, 0);
  this->device->state.SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE);
  this->device->state.SetRenderState(D3DRS_ZWRITEENABLE, 1);

  this->DrawOpaqueQueues();
  this->DrawGroundQueues();

  this->device->state.SetRenderState(D3DRS_ALPHAREF, 1);
  this->device->state.SetRenderState(D3DRS_ALPHABLENDENABLE, 1);
  this->DrawAuxiliaryQueue();

  const auto drawCommand = [&](RenderCommand& command) {
    this->device->state.SetTexture(0, command.texture ? static_cast<CTextureDX12*>(command.texture) : nullptr);
    this->SetTextureMode(command.textureMode);
    this->device->state.SetRenderState(D3DRS_SRCBLEND, command.sourceBlend);
    this->device->state.SetRenderState(D3DRS_DESTBLEND, command.destinationBlend);
    this->DrawPrimitive(&command);
  };

  {
    ScopedDeviceStateDX12 restore(this->device->state);
    this->device->state.SetRenderState(D3DRS_FOGENABLE, 0);
    this->device->state.SetRenderState(D3DRS_ZWRITEENABLE, 0);

    for (RenderCommand* command : this->queue_0210) {
      if (command) {
        drawCommand(*command);
      }
    }
  }

  {
    ScopedDeviceStateDX12 restore(this->device->state);
    this->device->state.SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
    this->DrawDepthQueue(true, this->queue_021C);
  }

  {
    ScopedDeviceStateDX12 restore(this->device->state);
    std::sort(this->queue_0180.begin(), this->queue_0180.end(), [](const DepthPrimitive& left, const DepthPrimitive& right) {
      if (left.depth < right.depth) {
        return true;
      }

      if (right.depth < left.depth) {
        return false;
      }

      return left.depth == right.depth && reinterpret_cast<std::uintptr_t>(left.primitive) < reinterpret_cast<std::uintptr_t>(right.primitive);
    });
    this->device->state.SetRenderState(D3DRS_ALPHATESTENABLE, 1);
    this->device->state.SetRenderState(D3DRS_ZWRITEENABLE, 1);
    this->device->state.SetRenderState(D3DRS_ALPHAREF, 159);
    this->device->state.SetRenderState(D3DRS_ALPHAFUNC, D3DCMP_GREATEREQUAL);
    this->DrawDepthQueue(false, this->queue_0180);
    this->device->state.SetRenderState(D3DRS_ZWRITEENABLE, 0);
    this->device->state.SetRenderState(D3DRS_ALPHAREF, 159);
    this->device->state.SetRenderState(D3DRS_ALPHAFUNC, D3DCMP_LESS);
    this->DrawDepthQueue(false, this->queue_0180);
  }

  {
    ScopedDeviceStateDX12 restore(this->device->state);
    this->device->state.SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ONE);
    this->device->state.SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ONE);
    this->DrawDepthQueue(true, this->queue_01C8);
  }

  this->device->state.SetRenderState(D3DRS_ZWRITEENABLE, 0);
  this->DrawAlphaQueues();

  {
    ScopedDeviceStateDX12 restore(this->device->state);
    this->device->state.SetRenderState(D3DRS_ALPHAREF, 207);
    this->DrawDepthQueue(true, this->queue_0228);
  }

  this->device->state.SetRenderState(D3DRS_FOGENABLE, 0);

  {
    ScopedDeviceStateDX12 restore(this->device->state);
    this->device->state.SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ONE);
    this->device->state.SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ONE);
    this->DrawDepthQueue(true, this->queue_01D4);
  }

  {
    ScopedDeviceStateDX12 restore(this->device->state);
    this->device->state.SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ONE);
    this->device->state.SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ONE);
    this->device->state.SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP);
    this->device->state.SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP);

    for (RenderCommand* command : this->queue_01EC) {
      drawCommand(*command);
    }
  }

  this->device->state.SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);

  {
    ScopedDeviceStateDX12 restore(this->device->state);
    this->device->state.SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ONE);
    this->device->state.SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ONE);
    this->DrawDepthQueue(true, this->queue_01E0);
  }

  {
    ScopedDeviceStateDX12 restore(this->device->state);
    this->device->state.SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    this->device->state.SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    this->DrawDepthQueue(false, this->queue_0198);
  }

  {
    ScopedDeviceStateDX12 restore(this->device->state);

    for (const DepthPrimitive& entry : this->queue_0234) {
      RenderCommand& command = *entry.primitive;
      this->device->state.SetTexture(0, command.texture ? static_cast<CTextureDX12*>(command.texture) : nullptr);
      this->SetTextureMode(command.textureMode);

      const bool selected = !(entry.depth >= 0.99f);
      this->device->state.SetRenderState(D3DRS_SRCBLEND, selected ? command.sourceBlend : D3DBLEND_SRCALPHA);
      this->device->state.SetRenderState(D3DRS_DESTBLEND, selected ? command.destinationBlend : D3DBLEND_INVSRCALPHA);
      this->DrawPrimitive(&command);
    }
  }
}

void CRendererDX12::DrawOpaqueQueues() {
  ScopedDeviceStateDX12 restore(this->device->state);
  this->device->state.SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
  this->device->state.SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
  const auto before = [](const RenderCommand* left, const RenderCommand* right) {
    return reinterpret_cast<std::uintptr_t>(left->texture) < reinterpret_cast<std::uintptr_t>(right->texture);
  };
  std::sort(this->queue_015C.begin(), this->queue_015C.end(), before);

  for (RenderCommand* command : this->queue_015C) {
    if (command) {
      this->device->state.SetTexture(0, command->texture ? static_cast<CTextureDX12*>(command->texture) : nullptr);
      this->DrawPrimitive(command);
    }
  }

  this->device->state.SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP);
  this->device->state.SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP);
  std::sort(this->queue_01B0.begin(), this->queue_01B0.end(), before);

  for (RenderCommand* command : this->queue_01B0) {
    if (command) {
      this->device->state.SetTexture(0, command->texture ? static_cast<CTextureDX12*>(command->texture) : nullptr);
      this->DrawPrimitive(command);
    }
  }
}

void CRendererDX12::DrawBaseQueue() {
  ScopedDeviceStateDX12 restore(this->device->state);
  std::sort(this->queue_0168.begin(), this->queue_0168.end(), [](const RenderCommand* left, const RenderCommand* right) {
    return reinterpret_cast<std::uintptr_t>(left->texture) < reinterpret_cast<std::uintptr_t>(right->texture);
  });

  for (RenderCommand* command : this->queue_0168) {
    this->device->state.SetTexture(0, command->texture ? static_cast<CTextureDX12*>(command->texture) : nullptr);
    this->DrawPrimitive(command);
  }
}

void CRendererDX12::DrawGroundQueues() {
  ScopedDeviceStateDX12 restore(this->device->state);
  this->device->state.SetRenderState(D3DRS_AMBIENT, 0xFFFFFFFFu);
  this->device->state.SetRenderState(D3DRS_CULLMODE, D3DCULL_CW);

  if (g_serviceOptions.Get(TT_LIGHTMAP_ON_OFF) != 0) {
    if (g_multitexture == 1) {
      ScopedDeviceStateDX12 restoreDual(this->device->state);
      this->device->state.SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_MODULATEALPHA_ADDCOLOR);
      auto& queue = this->lightmappedFirst;
      std::sort(queue.begin(), queue.end(), [](const LightmappedPrimitive* left, const LightmappedPrimitive* right) {
        const auto first = reinterpret_cast<std::uintptr_t>(left->texture);
        const auto second = reinterpret_cast<std::uintptr_t>(right->texture);

        if (first != second) {
          return first < second;
        }

        return reinterpret_cast<std::uintptr_t>(left->lightmap) < reinterpret_cast<std::uintptr_t>(right->lightmap);
      });

      for (LightmappedPrimitive* command : queue) {
        if (command->vertexCount > 0) {
          this->device->state.SetTexture(0, command->texture ? static_cast<CTextureDX12*>(command->texture) : nullptr);
          this->device->state.SetTexture(1, command->lightmap ? static_cast<CTextureDX12*>(command->lightmap) : nullptr);
          this->DrawLightmappedPrimitive(command);
        }
      }

      this->device->state.SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    } else {
      this->DrawBaseQueue();
      ScopedDeviceStateDX12 restoreLightmap(this->device->state);
      this->device->state.SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
      this->device->state.SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
      this->device->state.SetRenderState(D3DRS_ALPHABLENDENABLE, 1);
      this->device->state.SetRenderState(D3DRS_ALPHAREF, 1);
      this->device->state.SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ONE);
      this->device->state.SetRenderState(D3DRS_DESTBLEND, D3DBLEND_SRCALPHA);
      std::sort(this->queue_0174.begin(), this->queue_0174.end(), [](const RenderCommand* left, const RenderCommand* right) {
        return reinterpret_cast<std::uintptr_t>(left->texture) < reinterpret_cast<std::uintptr_t>(right->texture);
      });

      for (RenderCommand* command : this->queue_0174) {
        this->device->state.SetTexture(0, command->texture ? static_cast<CTextureDX12*>(command->texture) : nullptr);
        this->DrawPrimitive(command);
      }
    }
  } else {
    this->DrawBaseQueue();
  }

  this->device->state.SetRenderState(D3DRS_AMBIENT, 0);
  this->device->state.SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
}

void CRendererDX12::DrawAlphaQueues() {
  ScopedDeviceStateDX12 restore(this->device->state);
  const auto before = [](const DepthPrimitive& left, const DepthPrimitive& right) {
    return Before(left, right);
  };

  std::sort(this->queue_018C.begin(), this->queue_018C.end(), before);
  std::sort(this->queue_01BC.begin(), this->queue_01BC.end(), before);
  this->device->state.SetRenderState(D3DRS_ALPHABLENDENABLE, 1);
  this->device->state.SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
  this->device->state.SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
  this->device->state.SetRenderState(D3DRS_ALPHATESTENABLE, 1);
  this->device->state.SetRenderState(D3DRS_ALPHAREF, 1);
  this->device->state.SetRenderState(D3DRS_ALPHAFUNC, D3DCMP_GREATEREQUAL);

  if (this->twoPass == 1) {
    const auto drawAlphaTested = [this](const DepthPrimitive& entry) {
      RenderCommand* command = entry.primitive;
      this->device->state.SetRenderState(D3DRS_ALPHAREF, WritesDepth(*command) ? 1 : 240);
      this->device->state.SetTexture(0, command->texture ? static_cast<CTextureDX12*>(command->texture) : nullptr);
      this->DrawPrimitive(command);
    };

    this->device->state.SetRenderState(D3DRS_ZWRITEENABLE, 1);
    this->device->state.SetRenderState(D3DRS_ALPHAFUNC, D3DCMP_GREATEREQUAL);
    this->device->state.SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    this->device->state.SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    std::for_each(this->queue_018C.begin(), this->queue_018C.end(), drawAlphaTested);
    this->device->state.SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP);
    this->device->state.SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP);
    std::for_each(this->queue_01BC.begin(), this->queue_01BC.end(), drawAlphaTested);

    this->device->state.SetRenderState(D3DRS_ZWRITEENABLE, 0);
    this->device->state.SetRenderState(D3DRS_ALPHAFUNC, D3DCMP_LESS);
    this->device->state.SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    this->device->state.SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    std::for_each(this->queue_018C.begin(), this->queue_018C.end(), drawAlphaTested);
    this->device->state.SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP);
    this->device->state.SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP);
    std::for_each(this->queue_01BC.begin(), this->queue_01BC.end(), drawAlphaTested);

    this->device->state.SetRenderState(D3DRS_ALPHAREF, 1);
    this->device->state.SetRenderState(D3DRS_ALPHAFUNC, D3DCMP_GREATEREQUAL);
    this->DrawDepthQueue(true, this->queue_01A4);
  } else {
    const auto drawDepthWriting = [this](const DepthPrimitive& entry) {
      RenderCommand* command = entry.primitive;
      this->device->state.SetRenderState(D3DRS_ZWRITEENABLE, WritesDepth(*command));
      this->device->state.SetTexture(0, command->texture ? static_cast<CTextureDX12*>(command->texture) : nullptr);
      this->DrawPrimitive(command);
    };

    this->device->state.SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    this->device->state.SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    std::for_each(this->queue_018C.begin(), this->queue_018C.end(), drawDepthWriting);
    this->device->state.SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP);
    this->device->state.SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP);
    std::for_each(this->queue_01BC.begin(), this->queue_01BC.end(), drawDepthWriting);
  }
}
