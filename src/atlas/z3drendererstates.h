#pragma once

#include "zglmutils.h"
#include "z3drendercommands.h"

#include <array>
#include <optional>
#include <vector>

namespace nim {

struct RendererFrameState
{
  glm::uvec4 viewport{0};
  glm::mat4 viewportMatrix{1.f};
  glm::mat4 inverseViewportMatrix{1.f};

  struct ActiveSurface
  {
    std::vector<AttachmentDesc> colorAttachments;
    std::optional<AttachmentDesc> depthAttachment;

    void clear()
    {
      colorAttachments.clear();
      depthAttachment.reset();
    }

    [[nodiscard]] bool empty() const
    {
      return colorAttachments.empty() && !depthAttachment.has_value();
    }
  } activeSurface;

  bool updateViewportData(const glm::uvec4& rect)
  {
    if (viewport == rect) {
      return false;
    }

    viewport = rect;
    const float l = static_cast<float>(rect.x);
    const float b = static_cast<float>(rect.y);
    const float r = l + static_cast<float>(rect.z);
    const float t = b + static_cast<float>(rect.w);
    constexpr float n = 0.f;
    constexpr float f = 1.f;

    viewportMatrix = glm::mat4(glm::vec4((r - l) / 2.f, 0.f, 0.f, 0.f),
                               glm::vec4(0.f, (t - b) / 2.f, 0.f, 0.f),
                               glm::vec4(0.f, 0.f, (f - n) / 2.f, 0.f),
                               glm::vec4((r + l) / 2.f, (t + b) / 2.f, (f + n) / 2.f, 1.f));
    inverseViewportMatrix = glm::inverse(viewportMatrix);

    return true;
  }

  bool updateViewportData(const glm::uvec2& size)
  {
    return updateViewportData(glm::uvec4(0, 0, size.x, size.y));
  }

  void setActiveSurface(const ActiveSurface& surface)
  {
    activeSurface = surface;
  }

  void resetActiveSurface()
  {
    activeSurface.clear();
  }
};

struct RendererParameterState
{
  glm::mat4 coordTransform{glm::mat4(1.f)};
  float sizeScale{1.f};
  float opacity{1.f};
  glm::vec4 materialAmbient{glm::vec4(0.1f, 0.1f, 0.1f, 1.f)};
  glm::vec4 materialSpecular{glm::vec4(1.f, 1.f, 1.f, 1.f)};
  float materialShininess{100.f};
};

struct RendererViewState
{
  struct EyeState
  {
    glm::mat4 viewMatrix{1.f};
    glm::mat4 projectionMatrix{1.f};
    glm::mat4 projectionViewMatrix{1.f};
    glm::mat4 inverseViewMatrix{1.f};
    glm::mat4 inverseProjectionMatrix{1.f};
    glm::mat3 normalMatrix{1.f};
    glm::vec3 eyePosition{0.f};
    glm::vec2 frustumNearPlaneSize{0.f};
    float fieldOfView = 0.f;
    bool isPerspective = true;
  };

  std::array<EyeState, 3> eyes{};
  float nearClip = 0.f;
  float farClip = 0.f;
};

enum class TransparencyMode
{
  // NOTE: explicit values preserve on-disk / RPC stability.
  BlendDelayed = 0,
  BlendNoDepthMask = 1,
  WeightedAverage = 2,
  WeightedBlended = 3,
  DualDepthPeeling = 4,
  // Vulkan-only exact OIT via per-pixel fragment lists (PPLL). This is exposed
  // via the UI only when Render Backend is Vulkan; OpenGL falls back to DDP.
  PerPixelFragmentList = 6,
  Unknown = 5
};

enum class GeometryMSAAMode
{
  None,
  MSAA2x2
};

enum class FogMode
{
  None,
  Linear,
  Exponential,
  ExponentialSquared
};

struct RendererSceneState
{
  struct LightingState
  {
    std::vector<glm::vec4> positions{glm::vec4(0.1116f, 0.7660f, 0.6330f, 0.0f),
                                     glm::vec4(0.f, 0.f, 1.f, 0.0f),
                                     glm::vec4(-0.0449f, -0.9659f, 0.2549f, 0.0f),
                                     glm::vec4(0.9397f, 0.f, -0.3420f, 0.0f),
                                     glm::vec4(-0.9397f, 0.f, -0.3420f, 0.0f)};
    std::vector<glm::vec4> ambient{glm::vec4(0.1f, 0.1f, 0.1f, 1.0f),
                                   glm::vec4(0.1f * 0.333f, 0.1f * 0.333f, 0.1f * 0.333f, 1.0f),
                                   glm::vec4(0.1f * 0.333f, 0.1f * 0.333f, 0.1f * 0.333f, 1.0f),
                                   glm::vec4(0.1f * 0.27f, 0.1f * 0.27f, 0.1f * 0.27f, 1.0f),
                                   glm::vec4(0.1f * 0.27f, 0.1f * 0.27f, 0.1f * 0.27f, 1.0f)};
    std::vector<glm::vec4> diffuse{glm::vec4(0.75f, 0.75f, 0.75f, 1.0f),
                                   glm::vec4(0.75f * 0.333f, 0.75f * 0.333f, 0.75f * 0.333f, 1.0f),
                                   glm::vec4(0.75f * 0.333f, 0.75f * 0.333f, 0.75f * 0.333f, 1.0f),
                                   glm::vec4(0.75f * 0.27f, 0.75f * 0.27f, 0.75f * 0.27f, 1.0f),
                                   glm::vec4(0.75f * 0.27f, 0.75f * 0.27f, 0.75f * 0.27f, 1.0f)};
    std::vector<glm::vec4> specular{glm::vec4(0.85f, 0.85f, 0.85f, 1.0f),
                                    glm::vec4(0.f, 0.f, 0.f, 1.0f),
                                    glm::vec4(0.85f * 0.333f, 0.85f * 0.333f, 0.85f * 0.333f, 1.0f),
                                    glm::vec4(0.85f * 0.27f, 0.85f * 0.27f, 0.85f * 0.27f, 1.0f),
                                    glm::vec4(0.85f * 0.27f, 0.85f * 0.27f, 0.85f * 0.27f, 1.0f)};
    std::vector<glm::vec3> attenuation{glm::vec3(1.f, 0.f, 0.f),
                                       glm::vec3(1.f, 0.f, 0.f),
                                       glm::vec3(1.f, 0.f, 0.f),
                                       glm::vec3(1.f, 0.f, 0.f),
                                       glm::vec3(1.f, 0.f, 0.f)};
    std::vector<float> spotCutoff{180.f, 180.f, 180.f, 180.f, 180.f};
    std::vector<float> spotExponent{1.f, 1.f, 1.f, 1.f, 1.f};
    std::vector<glm::vec3> spotDirection{glm::vec3(-0.1116f, -0.7660f, -0.6330f),
                                         glm::vec3(0.f, 0.f, -1.f),
                                         glm::vec3(0.0449f, 0.9659f, -0.2549f),
                                         glm::vec3(-0.9397f, 0.f, 0.3420f),
                                         glm::vec3(0.9397f, 0.f, 0.3420f)};
    int lightCount = 5;
  } lighting;

  glm::vec4 sceneAmbient{0.2f, 0.2f, 0.2f, 1.f};
  float weightedBlendedDepthScale = 1.f;
  float devicePixelRatio = 1.f;
  TransparencyMode transparency = TransparencyMode::WeightedAverage;
  GeometryMSAAMode multisample = GeometryMSAAMode::MSAA2x2;

  struct FogState
  {
    FogMode mode = FogMode::None;
    glm::vec3 topColor{0.f};
    glm::vec3 bottomColor{0.f};
    glm::vec2 range{0.f};
    float density = 0.f;
  } fog;
};

} // namespace nim
