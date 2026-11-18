// ActorDepth: CEZ2STRParticle::RenderScreen (the screen-space STR quads of CEZ2STRScreenEffect, which hat effects use)
// gives every vertex z = 3.102122e-05 (always in front) and rhw = projected rhw - index * 1e-05, then hands the quad to
// particle->submit. For a particle not attached to a window (m_attachToWindow false), the detour swaps submit for a
// filter that, before forwarding, adds to each vertex's rhw a bias of 1e-05 + 1e-04 * (y - minY) / (maxY - minY) (0 to 1
// from the quad's top to its bottom; 1 for a flat quad) and sets z = RhwToDepth(rhw), so the quad is depth tested at the
// actor's depth. Window-attached (UI) particles keep the game's values.
#include "RoPlugin.h"

#include "Contents/Effects/Core/Effect.h"
#include "Contents/Effects/Particles/Particle.h"
#include "Render/Primitives/RenderPrimitive.h"
#include "Render/Primitives/TextureQuad.h"

namespace {
  constexpr const char* kName = "HatEffectDepth";
  constexpr int kQuadVertices = 4;

  void (CEZ2STRParticle::*g_renderScreen)(EZeffectClip*, EZeffectTransform*, const matrix&, int);
  ParticleSubmit g_submit;  // the particle's own submit while RenderScreen runs

  void SubmitAtActorDepth(RenderCommand* primitive, int flags) {
    RenderVertex* vertices = primitive->vertices;
    float minY = vertices[0].y;
    float maxY = vertices[0].y;
    for (int i = 1; i < kQuadVertices; ++i) {
      if (vertices[i].y < minY) minY = vertices[i].y;
      if (vertices[i].y > maxY) maxY = vertices[i].y;
    }
    const float range = maxY - minY > 0.0001f ? maxY - minY : 1.0f;
    for (int i = 0; i < kQuadVertices; ++i) {
      vertices[i].rhw += 0.00001f + (vertices[i].y - minY) / range * 0.0001f;
      vertices[i].z = RhwToDepth(vertices[i].rhw);
    }
    g_submit(primitive, flags);
  }

  struct ParticleHook : CEZ2STRParticle {
    void RenderScreenDetour(EZeffectClip* clip, EZeffectTransform* transform, const matrix& view, int index) {
      if (this->m_attachToWindow || this->submit == nullptr) {
        (this->*g_renderScreen)(clip, transform, view, index);
        return;
      }
      const ParticleSubmit saved = this->submit;
      g_submit = saved;
      this->submit = &SubmitAtActorDepth;
      (this->*g_renderScreen)(clip, transform, view, index);
      this->submit = saved;
    }
  };
}

RO_PLUGIN_EXPORT bool RoPlugin_Init(const RoHostApi* host) {
  if (host->version < 6) {
    host->Log("%s: needs RoPluginHost API 6 for settings, found %u", kName, host->version);
    return false;
  }
  return !host->SettingBool(kName, "ActorDepth") ||
         ro::Hook(host, &CEZ2STRParticle::RenderScreen, &ParticleHook::RenderScreenDetour, g_renderScreen, kName);
}
