// CostumeAuras: EffectLists::RegisterAllocators registers nothing for the effect ids 1325-1346 (EF_UNK_1325..1346), so
// EffectLists::Spawn returns null for them (e.g. a hat / costume effect from GetHatEffectID through
// CGameActor::SetHatEffect). After the original, allocators are added for them, following WARP EnableCostumeAuras:
// 11 class colours x 2 models.
// - 1325-1335: the level 99 aura model (CLevel99Effect's sparkles and segmented rings): sparkles use
//   effect\pikapika_<class>.tga untinted, rings effect\ring_white.tga tinted with the class colour.
// - 1336-1346: the level 160 aura model (CLevel150Effect's bubble rings effect\W_bubbleNN.tga and shocks
//   effect\emp shock.tga / effect\cir0002.tga, same motion), tinted with the class colour.
// Both draw with alpha blending (source 5, destination 6) at full ring / sparkle alpha, and live while their actor
// exists. The textures must be in the GRF.
#include "CostumeAuras.h"

#include "Client/Mode/CGameMode.h"
#include "Client/Mode/ModeMgr.h"
#include "Contents/Camera/Camera.h"
#include "Contents/Effects/Core/Effect.h"
#include "Contents/Effects/Core/EffectLists.h"
#include "Contents/Effects/Core/EffectMemoryMgr.h"
#include "Contents/Effects/Core/EffectPoint.h"
#include "Contents/Effects/Particles/Particle.h"
#include "Utility/Math/AffineMatrix.h"
#include "Utility/Text/FormatString.h"

#include <algorithm>
#include <new>
#include <string>
#include <vector>

namespace {
  constexpr const char* kName = "Auras";
  constexpr int kFirst99 = 1325;
  constexpr int kFirst160 = 1336;
  constexpr int kClassCount = 11;
  constexpr int kAlphaBlendSource = 5;
  constexpr int kAlphaBlendDestination = 6;
  constexpr float kPi = 3.141592f;

  struct Color {
    int red, green, blue;
  };

  const char* const kSparkles[kClassCount] = {
    "effect\\pikapika_rune_knight_red.tga",
    "effect\\pikapika_royal_guard_blue.tga",
    "effect\\pikapika_warlock_violet.tga",
    "effect\\pikapika_sorcerer_light_blue.tga",
    "effect\\pikapika_ranger_green.tga",
    "effect\\pikapika_minstrel_pink.tga",
    "effect\\pikapika_archbishop_white.tga",
    "effect\\pikapika_guillotine_cross_silver.tga",
    "effect\\pikapika_shadow_chaser_black.tga",
    "effect\\pikapika_mechanic_gold.tga",
    "effect\\pikapika_genetic_yellowgreen.tga",
  };

  const Color kColors99[kClassCount] = {
    {255, 45, 5}, {30, 150, 255}, {170, 40, 255}, {80, 200, 255}, {40, 230, 70}, {255, 80, 170},
    {255, 255, 255}, {180, 200, 220}, {35, 20, 8}, {255, 180, 20}, {160, 235, 30},
  };

  const Color kColors160[kClassCount] = {
    {214, 26, 0}, {0, 36, 216}, {141, 0, 228}, {0, 216, 255}, {0, 139, 22}, {255, 4, 169},
    {224, 249, 255}, {175, 185, 211}, {19, 9, 14}, {251, 193, 0}, {186, 255, 0},
  };

  bool (EffectLists::*g_registerAllocators)();

  void FreeAll(std::vector<CParticle*>& particles) {
    for (CParticle* particle : particles) {
      FreeParticle(particle);
    }
    particles.clear();
  }

  void FollowEffect(CParticle* particle, const float* position) {
    particle->position.x = position[0];
    particle->position.y = position[1];
    particle->position.z = position[2];
  }

  // The level 99 model (CLevel99Effect), sparkle texture per class, ring colour per class.
  class CostumeAura99 : public CEffect {
  public:
    explicit CostumeAura99(int classIndex) : CEffect(200), sparkleTexture(kSparkles[classIndex]), color(kColors99[classIndex]) {}

    void SetColor(short red, short green, short blue) override {
      for (CParticle* particle : this->rings) {
        particle->SetColor(red, green, blue);
      }
    }

    void Open() override {
      int index = 0;
      int tilt = 0;
      for (int angle = 0; angle < 44; angle += 22, tilt += 180, ++index) {
        CParticle* particle = CreateParticle(&this->positionVector, 13);
        if (!particle) {
          continue;
        }
        particle->SetTexture(this->sparkleTexture);
        particle->effectId = this->id;
        particle->columnIndex = index;
        particle->rowIndex = 0;
        particle->groupIndex = 1;
        particle->alpha = 255;
        particle->angle = static_cast<float>(angle);
        particle->phaseParameter = tilt;
        particle->scale = 9.0f;
        particle->sourceBlend = kAlphaBlendSource;
        particle->destinationBlend = kAlphaBlendDestination;
        particle->InitParticleQuad(particle->vertices);
        this->sparkles.push_back(particle);
      }
      this->AddRings();
    }

    void Close() override {
      FreeAll(this->sparkles);
      FreeAll(this->rings);
    }

    bool IsAlive() override {
      return this->ActorExists();
    }

    void OnProcess() override {
      for (CParticle* particle : this->sparkles) {
        FollowEffect(particle, this->position);
        matrix rotation;
        rotation.Identity();
        rotation.MakeZRotation(particle->angle / 180.0f * kPi);
        rotation.AppendXRotation(1.570796f);

        int phase = particle->phaseParameter + 3;
        if (phase >= 360) {
          phase -= 360;
        }
        particle->phaseParameter = phase;

        matrix scale;
        scale.Identity();
        const float size = (g_sinTable[phase] + 1.0f) * (particle->scale * 0.1f) + particle->scale;
        scale.SetScale(size, size, 1.0f);
        particle->transform.Compose(scale, rotation);
        ++particle->elapsedFrames;
      }

      for (CParticle* particle : this->rings) {
        FollowEffect(particle, this->position);
        const float degrees = static_cast<float>(static_cast<int>(static_cast<float>(particle->rowIndex) + particle->angle + 3.0f));
        particle->angle = degrees;
        matrix rotation;
        rotation.Identity();
        rotation.MakeYRotation(degrees / 180.0f * kPi);
        particle->transform = rotation;
        ++particle->elapsedFrames;
      }
    }

    void Render() override {
      for (CParticle* particle : this->sparkles) {
        particle->Render(false, 0.0f, 1.0f, 0.0f, 1.0f, false, true);
      }
      for (CParticle* particle : this->rings) {
        const int segment = particle->columnIndex;
        if (segment > 0) {
          const ParticleQuad quad = {false, 0.0f, 1.0f, static_cast<float>(segment - 1) / 21.0f, static_cast<float>(segment) / 21.0f, false, true};
          particle->Render(quad);
        }
      }
    }

  private:
    void AddRings() {
      int row = 0;
      int startAngle = 0;
      for (int tilt = 55; tilt > 40; tilt -= 5, ++row, startAngle += 90) {
        for (int segment = 0; segment <= 21; ++segment) {
          CParticle* particle = CreateParticle(&this->positionVector, 13);
          if (!particle) {
            continue;
          }
          particle->SetTexture("effect\\ring_white.tga");
          particle->effectId = this->id;
          particle->columnIndex = segment;
          particle->rowIndex = row;
          particle->groupIndex = 3;
          particle->alpha = 255;
          particle->SetColor(static_cast<short>(this->color.red), static_cast<short>(this->color.green), static_cast<short>(this->color.blue));
          particle->angle = static_cast<float>(startAngle);
          particle->angleParameter = 315;
          particle->speedParameter = 15.0f - static_cast<float>(row) * 2.0f;
          particle->scale = static_cast<float>(row) * 0.2f + 3.9f;
          particle->phaseParameter = tilt;
          particle->animationParameter = 0.0f;
          particle->sourceBlend = kAlphaBlendSource;
          particle->destinationBlend = kAlphaBlendDestination;
          particle->InitParticleQuad(particle->vertices);
          this->rings.push_back(particle);

          const float radius = g_sinTable[segment * 9] * particle->speedParameter;
          const int angle = static_cast<int>(static_cast<int>(particle->angleParameter / 20.0f) * segment + particle->angle) % 360;
          const float spread = radius * g_cosTable[tilt];
          const float height = radius * g_sinTable[tilt];
          const float cosine = g_cosTable[angle];
          const float sine = g_sinTable[angle];
          const EffectPoint inner = MakePoint(particle->scale * cosine, 0.0f, particle->scale * sine);
          const EffectPoint outer = MakePoint(inner.x + spread * cosine, -height, inner.z + spread * sine);
          SetQuadEdge(particle, inner, outer, segment != 0, this->previous);
        }
      }
    }

  public:
    static EffectPoint MakePoint(float x, float y, float z) {
      EffectPoint point;
      point.x = x;
      point.y = y;
      point.z = z;
      return point;
    }

    // Vertices 3 / 1 get this segment's inner / outer points, 0 / 2 the previous segment's outer / inner ones.
    static void SetQuadEdge(CParticle* particle, const EffectPoint& inner, const EffectPoint& outer, bool joinPrevious, EffectPoint* previous) {
      particle->vertices[3].x = inner.x;
      particle->vertices[3].y = inner.y;
      particle->vertices[3].z = inner.z;
      particle->vertices[1].x = outer.x;
      particle->vertices[1].y = outer.y;
      particle->vertices[1].z = outer.z;
      if (joinPrevious) {
        particle->vertices[0].x = previous[1].x;
        particle->vertices[0].y = previous[1].y;
        particle->vertices[0].z = previous[1].z;
        particle->vertices[2].x = previous[0].x;
        particle->vertices[2].y = previous[0].y;
        particle->vertices[2].z = previous[0].z;
      }
      previous[0] = inner;
      previous[1] = outer;
    }

  private:
    EffectPoint previous[2];
    std::vector<CParticle*> sparkles;
    std::vector<CParticle*> rings;
    std::string sparkleTexture;
    Color color;
  };

  // The level 160 model (CLevel150Effect's bubbles and shocks), tinted per class.
  class CostumeAura160 : public CEffect {
  public:
    explicit CostumeAura160(int classIndex) : CEffect(-1), color(kColors160[classIndex]) {}

    void SetDestinationBlend(int blend) override {
      if (blend == -1) {
        return;
      }
      for (CParticle* particle : this->bubbles) {
        particle->destinationBlend = blend;
      }
      for (CParticle* particle : this->shocks) {
        particle->destinationBlend = blend;
      }
    }

    void SetColor(short red, short green, short blue) override {
      for (CParticle* particle : this->bubbles) {
        particle->SetColor(red, green, blue);
      }
      for (CParticle* particle : this->shocks) {
        particle->SetColor(red, green, blue);
      }
    }

    void Open() override {
      this->AddBubbleRing(45);
      this->AddBubbleRing(25);
      this->AddShock(false);
      this->AddShock(true);
    }

    void Close() override {
      FreeAll(this->bubbles);
      FreeAll(this->shocks);
    }

    bool IsAlive() override {
      return this->ActorExists();
    }

    void OnProcess() override {
      for (CParticle* particle : this->bubbles) {
        FollowEffect(particle, this->position);
        int tilt = particle->phaseParameter - 1;
        float scale = particle->scale + 0.07f;
        int alpha = particle->alpha;
        if (tilt < 10) {
          tilt = 10;
          alpha = 0;
        }
        if (scale >= 3.5f) {
          alpha -= 3;
          if (alpha <= 0) {
            tilt = 74;
            scale = 2.87f;
          }
          alpha = std::max(0, alpha);
        } else {
          alpha += 10;
        }
        particle->alpha = alpha;
        particle->scale = scale;
        particle->phaseParameter = tilt;

        const float radius = particle->speedParameter;
        matrix transform;
        transform.Identity();
        transform.SetScale(scale * 0.5f, radius * g_sinTable[tilt] / (radius * g_sinTable[particle->columnIndex * 9] * 0.15f + radius), scale * 0.5f);
        particle->transform = transform;
        ++particle->elapsedFrames;
      }

      const float yaw = static_cast<float>(static_cast<int>(g_modeMgr.GetGameMode()->camera->camera.yaw));
      for (CParticle* particle : this->shocks) {
        FollowEffect(particle, this->position);
        int tilt = particle->phaseParameter + 3;
        if (tilt >= 360) {
          tilt -= 360;
        }
        particle->phaseParameter = tilt;

        const float size = particle->animationParameters[0];
        const float scale = size * 0.1f * g_sinTable[tilt] + 1.0f + size * 0.8f;
        matrix rotation;
        rotation.Identity();
        rotation.MakeXRotation(90.0f / 180.0f * kPi);
        rotation.AppendYRotation(static_cast<float>(particle->columnIndex * 23) / 180.0f * kPi - 160.0f / 180.0f * kPi - yaw / 180.0f * kPi);
        matrix scaling;
        scaling.Identity();
        scaling.SetScale(scale, scale, 1.0f);
        particle->transform.Compose(scaling, rotation);
        ++particle->elapsedFrames;
      }
    }

    void Render() override {
      for (CParticle* particle : this->bubbles) {
        const int segment = particle->columnIndex;
        if (segment <= 0) {
          continue;
        }
        const ParticleQuad quad = {false, 0.0f, 1.0f, static_cast<float>(segment) / 21.0f, static_cast<float>(segment + 1) / 21.0f, false, true};
        particle->SetTexture(FormatOwnedString("effect\\W_bubble%02d.tga", particle->elapsedFrames % 27 + 1));
        particle->Render(quad);
      }
      for (CParticle* particle : this->shocks) {
        particle->Render(false, 0.0f, 1.0f, 0.0f, 1.0f, false, true);
      }
    }

  private:
    void AddBubbleRing(int alphaOffset) {
      for (int ring = 0; ring < 4; ++ring) {
        for (int segment = 0; segment <= 21; ++segment) {
          CParticle* particle = CreateParticle(&this->positionVector, 13);
          if (!particle) {
            continue;
          }
          particle->SetTexture(FormatOwnedString("effect\\W_bubble%02d.tga", segment + 1));
          particle->effectId = this->id;
          particle->rowIndex = ring;
          particle->columnIndex = segment;
          particle->groupIndex = 4;
          particle->alpha = alphaOffset + (3 - ring) * 45;
          particle->SetColor(static_cast<short>(this->color.red), static_cast<short>(this->color.green), static_cast<short>(this->color.blue));
          particle->sourceBlend = kAlphaBlendSource;
          particle->destinationBlend = kAlphaBlendDestination;
          particle->angle = static_cast<float>((ring + 2) * 90 % 360);
          particle->angleParameter = 360;
          particle->speedParameter = 30.0f - static_cast<float>(ring);
          particle->scale = 3.5f;
          particle->phaseParameter = 80;
          particle->animationParameter = 0.0f;
          particle->elapsedFrames = 0;

          const float radius = particle->speedParameter * g_sinTable[segment * 9] * 0.15f + particle->speedParameter;
          const int direction = static_cast<int>(static_cast<float>(static_cast<int>(static_cast<float>(particle->angleParameter) / 20.0f) * segment) + particle->angle) % 360;
          const int tilt = particle->phaseParameter;
          const float spread = radius * g_cosTable[tilt];
          const float height = radius * g_sinTable[tilt];
          const float cosine = g_cosTable[direction];
          const float sine = g_sinTable[direction];
          const EffectPoint inner = CostumeAura99::MakePoint(cosine * particle->scale, 5.0f, sine * particle->scale);
          const EffectPoint outer = CostumeAura99::MakePoint(inner.x + spread * cosine, 5.0f - height, inner.z + spread * sine);
          CostumeAura99::SetQuadEdge(particle, inner, outer, segment != 0, this->previous);
          this->bubbles.push_back(particle);
        }
      }
    }

    void AddShock(bool circle) {
      for (int index = 0; index < 2; ++index) {
        CParticle* particle = CreateParticle(&this->positionVector, 13);
        if (!particle) {
          continue;
        }
        particle->effectId = this->id;
        particle->columnIndex = index;
        particle->rowIndex = 0;
        particle->groupIndex = 1;
        particle->phaseParameter = index * 180;
        particle->alpha = 200;
        particle->animationParameter = -1.0f;
        particle->elapsedFrames = 0;
        if (circle) {
          particle->SetTexture("effect\\cir0002.tga");
          particle->animationParameters[0] = 18.0f;
        } else {
          particle->SetTexture("effect\\emp shock.tga");
          particle->animationParameters[0] = 11.0f;
        }
        particle->sourceBlend = kAlphaBlendSource;
        particle->destinationBlend = kAlphaBlendDestination;
        particle->SetColor(static_cast<short>(this->color.red), static_cast<short>(this->color.green), static_cast<short>(this->color.blue));
        this->shocks.push_back(particle);
      }
    }

    EffectPoint previous[2];
    std::vector<CParticle*> bubbles;
    std::vector<CParticle*> shocks;
    Color color;
  };

  template <class Effect>
  struct CostumeAuraAllocator : CAllocator {
    explicit CostumeAuraAllocator(int classIndex) : classIndex(classIndex) {}
    CEffect* Create() override {
      return new (std::nothrow) Effect(this->classIndex);
    }
    int classIndex;
  };

  template <class Effect>
  bool Register(EffectLists& lists, int id, int classIndex) {
    CAllocator* allocator = new (std::nothrow) CostumeAuraAllocator<Effect>(classIndex);
    if (allocator == nullptr) {
      return false;
    }
    if (!lists.RegisterAllocator(id, allocator)) {
      delete allocator;  // id already taken, e.g. by a newer rodecomp: keep the game's
    }
    return true;
  }

  struct EffectListsHook : EffectLists {
    bool RegisterAllocatorsDetour() {
      if (!(this->*g_registerAllocators)()) {
        return false;
      }
      for (int index = 0; index < kClassCount; ++index) {
        if (!Register<CostumeAura99>(*this, kFirst99 + index, index) || !Register<CostumeAura160>(*this, kFirst160 + index, index)) {
          return false;
        }
      }
      return true;
    }
  };
}

bool InstallCostumeAuras(const RoHostApi* host) {
  return ro::Hook(host, &EffectLists::RegisterAllocators, &EffectListsHook::RegisterAllocatorsDetour, g_registerAllocators, kName);
}
