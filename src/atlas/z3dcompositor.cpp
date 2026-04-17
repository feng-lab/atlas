#include "z3dcompositor.h"

#include "z3dgl.h"
#include "z3drendertarget.h"
#include "z3dtexture.h"
#include "z3drendercommands.h"
#include "z3drenderglobalstate.h"
#include "zbenchtimer.h"
#include "z3dscratchresourcepool.h"
#include "zlog.h"
#include "zimg.h"
#include "zimgformat.h"
#include "zvulkandevice.h"
#include "zvulkantexture.h"
#include "z3drenderervulkanbackend.h"
#include "zvulkanlinearscript.h"
#include "zrenderthreadexecutor_tls.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <folly/ScopeGuard.h>
#include <functional>
#include <cmath>
#include <limits>
#include <memory>
#include <span>
#include <unordered_set>
#include <optional>

DEFINE_bool(atlas_vk_copy_yflip_in_shader, true, "Use y-flip in Vulkan final copy shader instead of UI flip");
DECLARE_bool(atlas_vk_ddp_indirect_count);
DEFINE_int32(atlas_ddp_max_passes, 100, "Maximum dual-depth peeling peel passes (applies to GL and Vulkan)");
DEFINE_int32(atlas_vk_ddp_cpu_chunk_passes,
             4,
             "Vulkan DDP: number of peel passes per submission for chunked early-stop. "
             "drawIndirectCount, when available, gates draws inside each chunk. Use <=0 to record the full loop.");

namespace {
using namespace nim;

inline uint8_t to_u8_from_unorm16(uint16_t v)
{
  return static_cast<uint8_t>((static_cast<uint32_t>(v) + 127u) / 257u);
}

void downloadVulkanTextureToLocalColorBuffer(ZVulkanTexture& tex, Z3DLocalColorBuffer& localColorBuffer)
{
  const uint32_t w = tex.width();
  const uint32_t h = tex.height();
  const size_t pixels = static_cast<size_t>(w) * h;
  const size_t desiredSize = pixels * 4u;
  if (localColorBuffer.data.size() < desiredSize) {
    localColorBuffer.data.resize(desiredSize);
  }

  if (tex.format() == vk::Format::eR8G8B8A8Unorm) {
    std::vector<uint8_t> rgba;
    rgba.resize(desiredSize);
    tex.downloadData(rgba.data(), rgba.size());
    for (size_t i = 0; i < pixels; ++i) {
      const uint8_t r = rgba[4 * i + 0];
      const uint8_t g = rgba[4 * i + 1];
      const uint8_t b = rgba[4 * i + 2];
      const uint8_t a = rgba[4 * i + 3];
      auto* out = &localColorBuffer.data[4 * i];
      out[0] = b;
      out[1] = g;
      out[2] = r;
      out[3] = a;
    }
  } else if (tex.format() == vk::Format::eR16G16B16A16Unorm) {
    std::vector<uint16_t> data16;
    data16.resize(pixels * 4u);
    tex.downloadData(data16.data(), data16.size() * sizeof(uint16_t));
    for (size_t i = 0, j = 0; i < pixels; ++i, j += 4) {
      const uint8_t r8 = to_u8_from_unorm16(data16[j + 0]);
      const uint8_t g8 = to_u8_from_unorm16(data16[j + 1]);
      const uint8_t b8 = to_u8_from_unorm16(data16[j + 2]);
      const uint8_t a8 = to_u8_from_unorm16(data16[j + 3]);
      auto* out = &localColorBuffer.data[4 * i];
      out[0] = b8;
      out[1] = g8;
      out[2] = r8;
      out[3] = a8;
    }
  } else {
    std::vector<uint8_t> rgba;
    rgba.resize(desiredSize);
    tex.downloadData(rgba.data(), rgba.size());
    for (size_t i = 0; i < pixels; ++i) {
      const uint8_t r = rgba[4 * i + 0];
      const uint8_t g = rgba[4 * i + 1];
      const uint8_t b = rgba[4 * i + 2];
      const uint8_t a = rgba[4 * i + 3];
      auto* out = &localColorBuffer.data[4 * i];
      out[0] = b;
      out[1] = g;
      out[2] = r;
      out[3] = a;
    }
  }

  localColorBuffer.width = w;
  localColorBuffer.height = h;
}

// Lightweight helper to log a Vulkan-backed lease's attachments and size
static void vlogVulkanLease(std::string_view label, const Z3DScratchResourcePool::RenderTargetLease& lease)
{
  if (!VLOG_IS_ON(2)) {
    return;
  }
  if (!lease || lease.backend != RenderBackend::Vulkan) {
    return;
  }
  const glm::uvec2 size = lease.descriptor.size;
  // Count color attachments advertised by the descriptor
  uint32_t colorCount = 0;
  for (const auto& a : lease.descriptor.attachments) {
    if (a.kind == ScratchAttachmentKind::Color) {
      colorCount++;
    }
  }
  ZVulkanTexture* c0 = lease.colorAttachment(0);
  ZVulkanTexture* d0 = lease.depthAttachmentTexture();
  const uint64_t c0Handle = reinterpret_cast<uint64_t>(c0);
  const uint64_t dHandle = reinterpret_cast<uint64_t>(d0);
  auto c0Fmt = c0 ? enumOrUnderlying(c0->format(), 16) : enumOrUnderlying(vk::Format{}, 16);
  auto dFmt = d0 ? enumOrUnderlying(d0->format(), 16) : enumOrUnderlying(vk::Format{}, 16);
  VLOG(2) << fmt::format("VK pass '{}': target={}x{} colors={} c0=0x{:x} fmt={} {}x{} depth={} d=0x{:x} fmt={} {}x{}",
                         label,
                         size.x,
                         size.y,
                         colorCount,
                         c0Handle,
                         c0Fmt,
                         c0 ? c0->width() : 0u,
                         c0 ? c0->height() : 0u,
                         d0 != nullptr,
                         dHandle,
                         dFmt,
                         d0 ? d0->width() : 0u,
                         d0 ? d0->height() : 0u);
}

// Unified compositor helper: render a filter into a given surface and ingest
// its recorded batches back into the compositor renderer. Optionally copy the
// source renderer's shader-hook parameters into the compositor before ingest.
static void recordFilterBatchesToSurfaceUnifiedWithViewport(Z3DRendererBase& compositor,
                                                            Z3DBoundedFilter* filter,
                                                            const RendererFrameState::ActiveSurface& surface,
                                                            const glm::uvec4& viewport,
                                                            const std::function<void()>& renderFn,
                                                            bool propagateHookPara)
{
  if (!filter) {
    return;
  }
  auto& source = filter->rendererBase();

  const glm::uvec4 previousViewport = source.frameState().viewport;
  const auto previousSurface = source.frameState().activeSurface;
  auto restoreGuard = folly::makeGuard([&]() {
    // Restore both the renderer-base viewport and any derived filter viewport
    // propagation (e.g., annotation filters that forward to child mesh filters).
    filter->setViewport(previousViewport);
    source.setActiveSurfaceWithLoadStore(previousSurface, Z3DRendererBase::Preserve);
  });

  // Mirror viewport and target surface into the source filter/renderer.
  // Note: use filter->setViewport() (not frameState().updateViewportData())
  // so any derived viewport propagation is applied only for this capture.
  filter->setViewport(viewport);
  source.setActiveSurfaceWithLoadStore(surface, Z3DRendererBase::Preserve);

  if (renderFn) {
    renderFn();
  }

  // Copy hook params if requested (needed by DDP paths)
  if (propagateHookPara) {
    compositor.shaderHookPara() = source.shaderHookPara();
  }

  auto& batches = source.cpuState().batches;
  for (auto& batch : batches) {
    if (batch.pass.colorAttachments.empty() && !surface.colorAttachments.empty()) {
      batch.pass.colorAttachments = surface.colorAttachments;
    }
    if (!batch.pass.depthAttachment.has_value() && surface.depthAttachment.has_value()) {
      batch.pass.depthAttachment = surface.depthAttachment;
    }
    // Populate missing viewport from compositor viewport
    if (batch.pass.viewport.extent == glm::vec2(0.0f) && viewport.z > 0u && viewport.w > 0u) {
      batch.pass.viewport.origin = glm::vec2(static_cast<float>(viewport.x), static_cast<float>(viewport.y));
      batch.pass.viewport.extent = glm::vec2(static_cast<float>(viewport.z), static_cast<float>(viewport.w));
      batch.pass.viewport.minDepth = 0.0f;
      batch.pass.viewport.maxDepth = 1.0f;
    }
    compositor.appendBatch(std::move(batch));
  }
  source.resetCPUState();
}

static void recordFilterBatchesToSurfaceUnified(Z3DRendererBase& compositor,
                                                Z3DBoundedFilter* filter,
                                                const RendererFrameState::ActiveSurface& surface,
                                                const std::function<void()>& renderFn,
                                                bool propagateHookPara)
{
  recordFilterBatchesToSurfaceUnifiedWithViewport(compositor,
                                                  filter,
                                                  surface,
                                                  compositor.frameState().viewport,
                                                  renderFn,
                                                  propagateHookPara);
}

static void recordTransparentFilterBatchesToSurfaceUnified(Z3DRendererBase& compositor,
                                                           Z3DBoundedFilter* filter,
                                                           const RendererFrameState::ActiveSurface& surface,
                                                           Z3DEye eye,
                                                           bool propagateHookPara)
{
  recordFilterBatchesToSurfaceUnified(
    compositor,
    filter,
    surface,
    [filter, eye]() {
      filter->renderTransparent(eye);
    },
    propagateHookPara);
}

static void
recordHandlePickingFilterBatchesToSurfaceUnifiedWithViewport(Z3DRendererBase& compositor,
                                                             Z3DBoundedFilter* filter,
                                                             const RendererFrameState::ActiveSurface& surface,
                                                             const glm::uvec4& viewport,
                                                             Z3DEye eye)
{
  recordFilterBatchesToSurfaceUnifiedWithViewport(
    compositor,
    filter,
    surface,
    viewport,
    [filter, eye]() {
      filter->renderHandlePicking(eye);
    },
    false);
}

static void
recordGeometryPickingFilterBatchesToSurfaceUnifiedWithViewport(Z3DRendererBase& compositor,
                                                               Z3DGeometryFilter* filter,
                                                               const RendererFrameState::ActiveSurface& surface,
                                                               const glm::uvec4& viewport,
                                                               Z3DEye eye)
{
  recordFilterBatchesToSurfaceUnifiedWithViewport(
    compositor,
    filter,
    surface,
    viewport,
    [filter, eye]() {
      filter->renderPicking(eye);
    },
    false);
}

// Like recordFilterBatchesToSurfaceUnified(), but captures the source renderer's
// batches into a caller-owned list (used for capture/replay within a single pass).
static void captureFilterBatchesToUnifiedList(Z3DRendererBase& compositor,
                                              Z3DBoundedFilter* filter,
                                              const RendererFrameState::ActiveSurface& surface,
                                              const std::function<void()>& renderFn,
                                              bool propagateHookPara,
                                              RendererCPUState& out)
{
  if (!filter) {
    return;
  }

  auto& source = filter->rendererBase();

  const glm::uvec4 previousViewport = source.frameState().viewport;
  const auto previousSurface = source.frameState().activeSurface;
  auto restoreGuard = folly::makeGuard([&]() {
    filter->setViewport(previousViewport);
    source.setActiveSurfaceWithLoadStore(previousSurface, Z3DRendererBase::Preserve);
  });

  // Mirror compositor viewport and target surface into the source renderer
  filter->setViewport(compositor.frameState().viewport);
  source.setActiveSurfaceWithLoadStore(surface, Z3DRendererBase::Preserve);

  if (renderFn) {
    renderFn();
  }

  // Copy hook params if requested (needed by DDP paths)
  if (propagateHookPara) {
    compositor.shaderHookPara() = source.shaderHookPara();
  }

  auto& batches = source.cpuState().batches;
  for (auto& batch : batches) {
    if (batch.pass.colorAttachments.empty() && !surface.colorAttachments.empty()) {
      batch.pass.colorAttachments = surface.colorAttachments;
    }
    if (!batch.pass.depthAttachment.has_value() && surface.depthAttachment.has_value()) {
      batch.pass.depthAttachment = surface.depthAttachment;
    }
    // Populate missing viewport from compositor viewport
    if (batch.pass.viewport.extent == glm::vec2(0.0f) && compositor.frameState().viewport.z > 0u &&
        compositor.frameState().viewport.w > 0u) {
      batch.pass.viewport.origin = glm::vec2(static_cast<float>(compositor.frameState().viewport.x),
                                             static_cast<float>(compositor.frameState().viewport.y));
      batch.pass.viewport.extent = glm::vec2(static_cast<float>(compositor.frameState().viewport.z),
                                             static_cast<float>(compositor.frameState().viewport.w));
      batch.pass.viewport.minDepth = 0.0f;
      batch.pass.viewport.maxDepth = 1.0f;
    }
    out.batches.push_back(std::move(batch));
  }
  out.uniformBytesEstimate += source.cpuState().uniformBytesEstimate;
  source.resetCPUState();
}

static void captureTransparentFilterBatchesToUnifiedList(Z3DRendererBase& compositor,
                                                         Z3DBoundedFilter* filter,
                                                         const RendererFrameState::ActiveSurface& surface,
                                                         Z3DEye eye,
                                                         bool propagateHookPara,
                                                         RendererCPUState& out)
{
  captureFilterBatchesToUnifiedList(
    compositor,
    filter,
    surface,
    [filter, eye]() {
      filter->renderTransparent(eye);
    },
    propagateHookPara,
    out);
}

} // namespace

namespace nim {

Z3DCompositor::Z3DCompositor(Z3DGlobalParameters& globalParas, QObject* parent)
  : Z3DBoundedFilter(globalParas, parent)
  , m_alphaBlendRenderer(m_rendererBase, TextureBlendMode::DepthTestBlending)
  , m_firstOnTopBlendRenderer(m_rendererBase, TextureBlendMode::FirstOnTopBlending)
  , m_firstOnTopRenderer(m_rendererBase, TextureBlendMode::FirstOnTop)
  , m_MIPImageAlphaBlendRenderer(m_rendererBase, TextureBlendMode::MIPImageDepthTestBlending)
  , m_textureCopyRenderer(m_rendererBase)
  , m_glowRenderer(m_rendererBase)
  , m_backgroundRenderer(m_rendererBase)
  , m_backgroundMode("Background Mode")
  , m_backgroundFirstColor("First Color", glm::vec4(1.0f, 1.0f, 1.0f, 1.0f))
  , m_backgroundSecondColor("Second Color", glm::vec4(0.2f, 0.2f, 0.2f, 1.0f))
  , m_backgroundGradientOrientation("Gradient Orientation")
  , m_ddpBlendShader()
  , m_ddpFinalShader()
  , m_waFinalShader()
  , m_wbFinalShader()
  , m_showBackground("Show Background", true)
  , m_lineRenderer(m_rendererBase)
  , m_arrowRenderer(m_rendererBase)
  , m_fontRenderer(m_rendererBase)
  , m_showAxis("Show Axis", true)
  , m_XAxisColor("X Axis Color", glm::vec4(1.f, 0.f, 0.f, 1.0f))
  , m_YAxisColor("Y Axis Color", glm::vec4(0.f, 1.f, 0.f, 1.0f))
  , m_ZAxisColor("Z Axis Color", glm::vec4(0.f, 0.f, 1.f, 1.0f))
  , m_axisRegionRatio("Axis Region Ratio", .25f, .1f, 1.f)
  , m_axisMode("Mode")
  , m_axisFontName("Font")
  , m_axisFontSize("Font Size", 32.f, .1f, 5000.f)
  , m_axisFontSoftEdgeScale("Font Softedge Scale", 80.f, 0.f, 200.f)
  , m_axisShowFontOutline("Show Font Outline", false)
  , m_axisFontOutlineMode("Font Outline Mode")
  , m_axisFontOutlineColor("Font Outline Color", glm::vec4(1.f))
  , m_axisShowFontShadow("Show Font Shadow", false)
  , m_axisFontShadowColor("Font Shadow Color", glm::vec4(0.f, 0.f, 0.f, 1.f))
  , m_region(0, 1, 0, 1)
{
  ensureOutputTargets(m_outputSize);

  m_monoCurrentTarget = &m_outRenderTarget1;
  m_monoReadyTarget = &m_outRenderTarget2;
  m_leftCurrentTarget = &m_leftEyeOutRenderTarget1;
  m_leftReadyTarget = &m_leftEyeOutRenderTarget2;
  m_rightCurrentTarget = &m_outRenderTarget1;
  m_rightReadyTarget = &m_outRenderTarget2;

  m_monoCurrentLocalBuffer = &m_localColorBuffer1;
  m_monoReadyLocalBuffer = &m_localColorBuffer2;
  m_leftCurrentLocalBuffer = &m_leftLocalColorBuffer1;
  m_leftReadyLocalBuffer = &m_leftLocalColorBuffer2;
  m_rightCurrentLocalBuffer = &m_localColorBuffer1;
  m_rightReadyLocalBuffer = &m_localColorBuffer2;

  addParameter(m_showBackground);
  m_showBackground.setDescription(QStringLiteral("Toggle rendering of the background (uniform or gradient fill)."));

  m_textureCopyRenderer.setDiscardTransparent(true);
  m_backgroundFirstColor.setStyle("COLOR");
  m_backgroundSecondColor.setStyle("COLOR");
  m_backgroundMode.clearOptions();
  m_backgroundMode.addOptionsWithData(
    std::make_pair(enumToQString(BackgroundMode::Uniform), static_cast<int>(BackgroundMode::Uniform)),
    std::make_pair(enumToQString(BackgroundMode::Gradient), static_cast<int>(BackgroundMode::Gradient)));
  m_backgroundMode.select(enumToQString(BackgroundMode::Gradient));
  m_backgroundMode.setDescription(QStringLiteral(
    "Background fill mode: 'Uniform' uses 'First Color'; 'Gradient' blends 'First Color' to 'Second Color'"
    " in the selected 'Gradient Orientation'."));
  m_backgroundGradientOrientation.clearOptions();
  m_backgroundGradientOrientation.addOptionsWithData(
    std::make_pair(enumToQString(BackgroundGradientOrientation::LeftToRight),
                   static_cast<int>(BackgroundGradientOrientation::LeftToRight)),
    std::make_pair(enumToQString(BackgroundGradientOrientation::RightToLeft),
                   static_cast<int>(BackgroundGradientOrientation::RightToLeft)),
    std::make_pair(enumToQString(BackgroundGradientOrientation::TopToBottom),
                   static_cast<int>(BackgroundGradientOrientation::TopToBottom)),
    std::make_pair(enumToQString(BackgroundGradientOrientation::BottomToTop),
                   static_cast<int>(BackgroundGradientOrientation::BottomToTop)));
  m_backgroundGradientOrientation.select(enumToQString(BackgroundGradientOrientation::BottomToTop));
  addParameter(m_backgroundMode);
  addParameter(m_backgroundFirstColor);
  addParameter(m_backgroundSecondColor);
  addParameter(m_backgroundGradientOrientation);
  m_backgroundFirstColor.setDescription(
    QStringLiteral("Primary background color (start of gradient or uniform fill)."));
  m_backgroundSecondColor.setDescription(QStringLiteral("Secondary color used at the end of the gradient."));
  m_backgroundGradientOrientation.setDescription(
    QStringLiteral("Direction of the gradient when 'Background Mode' is set to 'Gradient'."));

  updateBackgroundFirstColor();
  updateBackgroundSecondColor();
  updateBackgroundOrientation();
  updateBackgroundMode();

  connect(&m_backgroundMode, &ZStringIntOptionParameter::valueChanged, this, &Z3DCompositor::updateBackgroundMode);
  connect(&m_backgroundFirstColor, &ZVec4Parameter::valueChanged, this, &Z3DCompositor::updateBackgroundFirstColor);
  connect(&m_backgroundSecondColor, &ZVec4Parameter::valueChanged, this, &Z3DCompositor::updateBackgroundSecondColor);
  connect(&m_backgroundGradientOrientation,
          &ZStringIntOptionParameter::valueChanged,
          this,
          &Z3DCompositor::updateBackgroundOrientation);

  // Glow renderer is compositor-owned; parameters are synced from filters per-draw

  if (m_rendererBase.activeBackend() == RenderBackend::OpenGL) {
    // OpenGL-only resources. Vulkan startup intentionally avoids creating or
    // binding a GL context, so any GL calls here must be gated.
    m_screenQuadVAO = std::make_unique<Z3DVertexArrayObject>(1);

    m_waFinalShader = std::make_unique<Z3DShaderProgram>();
    m_waFinalShader->loadFromSourceFile("pass.vert", "wavg_final.frag", m_rendererBase.generateHeader());

    m_wbFinalShader = std::make_unique<Z3DShaderProgram>();
    m_wbFinalShader->loadFromSourceFile("pass.vert", "wblended_final.frag", m_rendererBase.generateHeader());

    m_ddpBlendShader = std::make_unique<Z3DShaderProgram>();
    m_ddpBlendShader->loadFromSourceFile("pass.vert", "dual_peeling_blend.frag", m_rendererBase.generateHeader());

    m_ddpFinalShader = std::make_unique<Z3DShaderProgram>();
    m_ddpFinalShader->loadFromSourceFile("pass.vert", "dual_peeling_final.frag", m_rendererBase.generateHeader());

    ensurePickingTarget(glm::uvec2(32u, 32u));
  } else {
    ensurePickingTargetVulkan(glm::uvec2(32u, 32u));
  }
  addInteractionHandler(globalParas.interactionHandler);

  m_XAxisColor.setStyle("COLOR");
  m_YAxisColor.setStyle("COLOR");
  m_ZAxisColor.setStyle("COLOR");
  m_axisMode.addOptions("Arrow", "Line");
  m_axisMode.select("Arrow");
  m_axisFontSize.setSingleStep(0.1);
  m_axisFontSize.setDecimal(1);
  m_axisFontSoftEdgeScale.setSingleStep(1.0);
  m_axisFontOutlineMode.clearOptions();
  m_axisFontOutlineMode.addOptionsWithData(
    std::make_pair(enumToQString(FontOutlineMode::Glow), static_cast<int>(FontOutlineMode::Glow)),
    std::make_pair(enumToQString(FontOutlineMode::Outline), static_cast<int>(FontOutlineMode::Outline)));
  m_axisFontOutlineMode.select(enumToQString(FontOutlineMode::Glow));
  m_axisFontOutlineColor.setStyle("COLOR");
  m_axisFontShadowColor.setStyle("COLOR");
  addParameter(m_showAxis);
  m_showAxis.setDescription(QStringLiteral("Show 3D axes overlay with labels (X,Y,Z)."));
  addParameter(m_XAxisColor);
  addParameter(m_YAxisColor);
  addParameter(m_ZAxisColor);
  addParameter(m_axisRegionRatio);
  addParameter(m_axisMode);
  addParameter(m_axisFontName);
  addParameter(m_axisFontSize);
  addParameter(m_axisFontSoftEdgeScale);
  addParameter(m_axisShowFontOutline);
  addParameter(m_axisFontOutlineMode);
  addParameter(m_axisFontOutlineColor);
  addParameter(m_axisShowFontShadow);
  addParameter(m_axisFontShadowColor);
  m_XAxisColor.setDescription(QStringLiteral("Color of the X axis and its label."));
  m_YAxisColor.setDescription(QStringLiteral("Color of the Y axis and its label."));
  m_ZAxisColor.setDescription(QStringLiteral("Color of the Z axis and its label."));
  m_axisRegionRatio.setDescription(QStringLiteral("Fraction of the viewport reserved for the axis overlay (0.1–1.0)."));
  m_axisMode.setDescription(
    QStringLiteral("Axis rendering style: 'Arrow' draws arrowheads; 'Line' draws simple lines."));
  m_axisFontName.setDescription(QStringLiteral("Font face used for axis labels (if available on system)."));
  m_axisFontSize.setDescription(QStringLiteral("Axis label font size in pixels."));
  m_axisFontSoftEdgeScale.setDescription(
    QStringLiteral("Soft-edge distance for signed-distance-field (SDF) font rendering; increases edge smoothing."));
  m_axisShowFontOutline.setDescription(QStringLiteral("Draw an outline around axis label glyphs."));
  m_axisFontOutlineMode.setDescription(QStringLiteral("Outline effect used when outlines are enabled (e.g., Glow)."));
  m_axisFontOutlineColor.setDescription(QStringLiteral("Color of the font outline."));
  m_axisShowFontShadow.setDescription(QStringLiteral("Draw a shadow behind axis label glyphs."));
  m_axisFontShadowColor.setDescription(QStringLiteral("Color of the font shadow."));

  if (!m_fontRenderer.fontNames().isEmpty()) {
    int idx = 0;
    for (const auto& name : m_fontRenderer.fontNames()) {
      m_axisFontName.addOptionWithData(std::make_pair(name, idx++));
    }
    m_axisFontName.select(m_fontRenderer.selectedFontName());
  } else {
    m_axisFontName.setVisible(false);
  }

  auto updateAxisFontWidgets = [this]() {
    const bool outlineVisible = m_axisShowFontOutline.get();
    m_axisFontOutlineMode.setVisible(outlineVisible);
    m_axisFontOutlineColor.setVisible(outlineVisible);
    m_axisFontShadowColor.setVisible(m_axisShowFontShadow.get());
  };

  connect(&m_axisFontName, &ZStringIntOptionParameter::valueChanged, this, [this]() {
    m_fontRenderer.setFontName(m_axisFontName.get());
  });
  connect(&m_axisFontSize, &ZFloatParameter::valueChanged, this, [this]() {
    m_fontRenderer.setFontSize(m_axisFontSize.get());
  });
  connect(&m_axisFontSoftEdgeScale, &ZFloatParameter::valueChanged, this, [this]() {
    m_fontRenderer.setFontSoftEdgeScale(m_axisFontSoftEdgeScale.get());
  });
  connect(&m_axisShowFontOutline, &ZBoolParameter::valueChanged, this, [this, updateAxisFontWidgets]() mutable {
    m_fontRenderer.setShowFontOutline(m_axisShowFontOutline.get());
    updateAxisFontWidgets();
  });
  connect(&m_axisFontOutlineMode, &ZStringIntOptionParameter::valueChanged, this, [this]() {
    m_fontRenderer.setFontOutlineMode(static_cast<FontOutlineMode>(m_axisFontOutlineMode.associatedData()));
  });
  connect(&m_axisFontOutlineColor, &ZVec4Parameter::valueChanged, this, [this]() {
    m_fontRenderer.setFontOutlineColor(m_axisFontOutlineColor.get());
  });
  connect(&m_axisShowFontShadow, &ZBoolParameter::valueChanged, this, [this, updateAxisFontWidgets]() mutable {
    m_fontRenderer.setShowFontShadow(m_axisShowFontShadow.get());
    updateAxisFontWidgets();
  });
  connect(&m_axisFontShadowColor, &ZVec4Parameter::valueChanged, this, [this]() {
    m_fontRenderer.setFontShadowColor(m_axisFontShadowColor.get());
  });

  m_fontRenderer.setFontName(m_axisFontName.get());
  m_fontRenderer.setFontSize(m_axisFontSize.get());
  m_fontRenderer.setFontSoftEdgeScale(m_axisFontSoftEdgeScale.get());
  m_fontRenderer.setShowFontOutline(m_axisShowFontOutline.get());
  m_fontRenderer.setFontOutlineMode(static_cast<FontOutlineMode>(m_axisFontOutlineMode.associatedData()));
  m_fontRenderer.setFontOutlineColor(m_axisFontOutlineColor.get());
  m_fontRenderer.setShowFontShadow(m_axisShowFontShadow.get());
  m_fontRenderer.setFontShadowColor(m_axisFontShadowColor.get());
  updateAxisFontWidgets();
#if !defined(ATLAS_USE_CORE_PROFILE) && defined(ATLAS_SUPPORT_FIXED_PIPELINE)
  m_arrowRenderer.setUseDisplayList(false);
  m_lineRenderer.setUseDisplayList(false);
#endif
  m_fontRenderer.setFollowCoordTransform(false);
  setupAxisCamera();

  for (auto* para : m_globalParameters.parameters()) {
    connect(para, &ZParameter::valueChanged, this, &Z3DBoundedFilter::invalidateResult);
  }
}

Z3DCompositor::~Z3DCompositor()
{
  resetVulkanSceneBatchCaches();
}

void Z3DCompositor::resetVulkanSceneBatchCaches()
{
  m_vkSceneBgGeomCacheKey = {};
  for (auto& cached : m_vkSceneBgGeomCache) {
    cached.reset();
  }
}

void Z3DCompositor::updateBackgroundMode()
{
  const auto mode = static_cast<BackgroundMode>(m_backgroundMode.associatedData());
  m_backgroundRenderer.setMode(mode);

  m_backgroundFirstColor.setVisible(true);
  const bool useGradient = mode == BackgroundMode::Gradient;
  m_backgroundSecondColor.setVisible(useGradient);
  m_backgroundGradientOrientation.setVisible(useGradient);
}

void Z3DCompositor::updateBackgroundFirstColor()
{
  m_backgroundRenderer.setFirstColor(m_backgroundFirstColor.get());
}

void Z3DCompositor::updateBackgroundSecondColor()
{
  m_backgroundRenderer.setSecondColor(m_backgroundSecondColor.get());
}

void Z3DCompositor::updateBackgroundOrientation()
{
  m_backgroundRenderer.setGradientOrientation(
    static_cast<BackgroundGradientOrientation>(m_backgroundGradientOrientation.associatedData()));
}

bool Z3DCompositor::isReady(Z3DEye) const
{
  return true;
}

std::shared_ptr<ZWidgetsGroup> Z3DCompositor::backgroundWidgetsGroup()
{
  if (!m_backgroundWidgetsGroup) {
    m_backgroundWidgetsGroup = std::make_shared<ZWidgetsGroup>("Background", 1);
    m_backgroundWidgetsGroup->addChild(m_showBackground, 1);
    m_backgroundWidgetsGroup->addChild(m_backgroundMode, 1);
    m_backgroundWidgetsGroup->addChild(m_backgroundFirstColor, 1);
    m_backgroundWidgetsGroup->addChild(m_backgroundSecondColor, 1);
    m_backgroundWidgetsGroup->addChild(m_backgroundGradientOrientation, 1);
    m_backgroundWidgetsGroup->setBasicAdvancedCutoff(4);
  }
  return m_backgroundWidgetsGroup;
}

std::shared_ptr<ZWidgetsGroup> Z3DCompositor::axisWidgetsGroup()
{
  if (!m_axisWidgetsGroup) {
    m_axisWidgetsGroup = std::make_shared<ZWidgetsGroup>("Axis", 1);
    m_axisWidgetsGroup->addChild(m_showAxis, 1);
    m_axisWidgetsGroup->addChild(m_axisMode, 1);
    m_axisWidgetsGroup->addChild(m_axisRegionRatio, 1);
    m_axisWidgetsGroup->addChild(m_XAxisColor, 1);
    m_axisWidgetsGroup->addChild(m_YAxisColor, 1);
    m_axisWidgetsGroup->addChild(m_ZAxisColor, 1);
    auto& rendererParas = m_rendererParameters;
    m_axisWidgetsGroup->addChild(rendererParas.sizeScale, 1);
    m_axisWidgetsGroup->addChild(rendererParas.opacity, 3);
    m_axisWidgetsGroup->addChild(m_axisFontName, 4);
    m_axisWidgetsGroup->addChild(m_axisFontSize, 4);
    m_axisWidgetsGroup->addChild(m_axisFontSoftEdgeScale, 4);
    m_axisWidgetsGroup->addChild(m_axisShowFontOutline, 4);
    m_axisWidgetsGroup->addChild(m_axisFontOutlineMode, 4);
    m_axisWidgetsGroup->addChild(m_axisFontOutlineColor, 4);
    m_axisWidgetsGroup->addChild(m_axisShowFontShadow, 4);
    m_axisWidgetsGroup->addChild(m_axisFontShadowColor, 4);
    m_axisWidgetsGroup->setBasicAdvancedCutoff(5);
  }
  return m_axisWidgetsGroup;
}

void Z3DCompositor::savePickingBufferToImage(const QString& filename)
{
  if (m_rendererBase.activeBackend() == RenderBackend::Vulkan) {
    CHECK(m_pickingTargetLease) << "Vulkan picking save requested but picking target is not initialized.";
    auto* vtex = m_pickingTargetLease.colorAttachment(0);
    CHECK(vtex) << "Vulkan picking color attachment not available.";

    const uint32_t w = vtex->width();
    const uint32_t h = vtex->height();
    std::vector<uint8_t, boost::alignment::aligned_allocator<uint8_t, 64>> raw;
    raw.resize(static_cast<size_t>(w) * h * 4u);
    try {
      vtex->downloadData(raw.data(), raw.size());
    }
    catch (const std::exception& e) {
      LOG(ERROR) << "Failed to download Vulkan picking image: " << e.what();
      return;
    }
    ZImg img;
    img.wrapData(raw.data(), w, h, 1, 4);
    ZImg tmp(img.info());
    // Vulkan is RGBA8; convert interleaved to planar without channel swaps
    ZImgFormat::CXYZtoXYZC(img, tmp);
    tmp.flip(Dimension::Y);
    tmp.infoRef().lastChannelIsAlphaChannel = true;
    try {
      tmp.save(filename);
    }
    catch (const ZException& e) {
      LOG(ERROR) << "Failed to save Vulkan picking image: " << e.what();
    }
    return;
  }

  const Z3DTexture* tex = pickingManager().renderTarget().attachment(GL_COLOR_ATTACHMENT0);
  tex->saveAsColorImage(filename);
}

void Z3DCompositor::saveOutputColorToImage(const QString& filename, Z3DEye eye)
{
  if (m_rendererBase.activeBackend() == RenderBackend::Vulkan) {
    const Z3DScratchResourcePool::RenderTargetLease* ready = (eye == MonoEye)   ? m_monoReadyTarget
                                                             : (eye == LeftEye) ? m_leftReadyTarget
                                                                                : m_rightReadyTarget;
    CHECK(ready && *ready && ready->backend == RenderBackend::Vulkan)
      << "Vulkan output save requested but ready lease is not Vulkan-backed.";

    ZVulkanTexture* vtex = ready->colorAttachment(0);
    CHECK(vtex) << "Vulkan output color attachment not available.";

    Z3DLocalColorBuffer temp{};
    downloadVulkanTextureToLocalColorBuffer(*vtex, temp);
    // Convert to planar RGBA and save
    ZImg bufImg;
    bufImg.wrapData(temp.data.data(), ZImgInfo(temp.width, temp.height, 1, 4));
    ZImg out(bufImg.info());
    ZImgFormat::CXYZtoXYZC(bufImg, out, true);
    out.infoRef().lastChannelIsAlphaChannel = true;
    out.flip(Dimension::Y);
    try {
      out.save(filename);
    }
    catch (const ZException& e) {
      LOG(ERROR) << "Failed to save Vulkan output color image: " << e.what();
    }
    return;
  }

  // GL fallback
  const Z3DScratchResourcePool::RenderTargetLease* ready = (eye == MonoEye)   ? m_monoReadyTarget
                                                           : (eye == LeftEye) ? m_leftReadyTarget
                                                                              : m_rightReadyTarget;
  CHECK(ready && ready->renderTarget) << "GL output save requested but ready render target missing.";

  const Z3DTexture* tex = ready->renderTarget->attachment(GL_COLOR_ATTACHMENT0);
  tex->saveAsColorImage(filename);
}

void Z3DCompositor::saveOutputDepthToImage(const QString& filename, Z3DEye eye)
{
  if (m_rendererBase.activeBackend() == RenderBackend::Vulkan) {
    const Z3DScratchResourcePool::RenderTargetLease* ready = (eye == MonoEye)   ? m_monoReadyTarget
                                                             : (eye == LeftEye) ? m_leftReadyTarget
                                                                                : m_rightReadyTarget;
    CHECK(ready && *ready && ready->backend == RenderBackend::Vulkan)
      << "Vulkan depth save requested but ready lease is not Vulkan-backed.";

    ZVulkanTexture* dtex = ready->depthAttachmentTexture();
    CHECK(dtex) << "Vulkan output depth attachment not available.";

    const uint32_t w = dtex->width();
    const uint32_t h = dtex->height();
    const size_t pixels = static_cast<size_t>(w) * h;

    // Convert to uint32_t image for saving (matches GL's saveAsDepthImage pattern)
    std::vector<uint32_t> depth;
    depth.resize(pixels);
    if (dtex->format() == vk::Format::eD32Sfloat) {
      std::vector<float> f;
      f.resize(pixels);
      dtex->downloadData(f.data(), f.size() * sizeof(float));
      for (size_t i = 0; i < pixels; ++i) {
        float v = std::clamp(f[i], 0.0f, 1.0f);
        depth[i] = static_cast<uint32_t>(v * 4294967295.0f + 0.5f);
      }
    } else {
      // Assume D24UnormS8: just download packed 32-bit and keep 24-bit depth in low bits.
      dtex->downloadData(depth.data(), depth.size() * sizeof(uint32_t));
    }
    ZImg img;
    img.wrapData(depth.data(), w, h, 1);
    img.flip(Dimension::Y);
    try {
      img.save(filename);
    }
    catch (const ZException& e) {
      LOG(ERROR) << "Failed to save Vulkan output depth image: " << e.what();
    }
    return;
  }

  // GL fallback
  const Z3DScratchResourcePool::RenderTargetLease* ready = (eye == MonoEye)   ? m_monoReadyTarget
                                                           : (eye == LeftEye) ? m_leftReadyTarget
                                                                              : m_rightReadyTarget;
  CHECK(ready && ready->renderTarget) << "GL depth save requested but ready render target missing.";

  ready->renderTarget->saveAsDepthImage(filename);
}

void Z3DCompositor::setRenderingRegion(double left, double right, double bottom, double top)
{
  m_backgroundRenderer.setRenderingRegion(left, right, bottom, top);
  m_region = glm::vec4(left, right - left, bottom, top - bottom);
}

void Z3DCompositor::invalidate(State inv)
{
  // VLOG(1) << "1";
  CHECK(inv != State::Valid);
  if (isFlagSet(m_state, inv)) {
    return;
  }
  // VLOG(1) << std::to_underlying(m_state) << " " << to_underlying(inv);
  setFlag(m_state, inv);

  Q_EMIT sceneParaUpdated();
}

void Z3DCompositor::setProgressiveRenderingMode(bool v)
{
  m_progressiveRendering = v;
  m_rendererBase.setCurrentRenderPassIsProgressive(v);
}

double Z3DCompositor::process(Z3DEye eye)
{
  if (m_rendererBase.activeBackend() == RenderBackend::Vulkan) {
    return processVulkan(eye);
  }
  return processGL(eye);
}

double Z3DCompositor::processGL(Z3DEye eye)
{
  syncRendererState();

  const GLboolean prevDepthTest = glIsEnabled(GL_DEPTH_TEST);
  GLboolean prevDepthMask = GL_TRUE;
  GLint prevDepthFuncInt = static_cast<GLint>(GL_LESS);
  glGetBooleanv(GL_DEPTH_WRITEMASK, &prevDepthMask);
  glGetIntegerv(GL_DEPTH_FUNC, &prevDepthFuncInt);
  const GLenum prevDepthFunc = static_cast<GLenum>(prevDepthFuncInt);
  auto depthStateGuard = folly::makeGuard([prevDepthTest, prevDepthMask, prevDepthFunc]() {
    if (prevDepthTest == GL_TRUE) {
      glEnable(GL_DEPTH_TEST);
    } else {
      glDisable(GL_DEPTH_TEST);
    }
    glDepthMask(prevDepthMask);
    glDepthFunc(prevDepthFunc);
  });

  const auto& filters = m_geometryFilters;
  const auto& vFilters = m_volumeFilters;
  // VLOG(1) << filters.size() << " " << vFilters.size();
  std::vector<Z3DBoundedFilter*> onTopOpaqueFilters;
  std::vector<Z3DBoundedFilter*> onTopTransparentFilters;
  std::vector<Z3DBoundedFilter*> normalOpaqueFilters;
  std::vector<Z3DBoundedFilter*> normalTransparentFilters;
  std::vector<Z3DBoundedFilter*> selectedFilters;
  std::vector<Z3DBoundedFilter*> showHandleFilters;

  const auto transparencyMode = m_rendererBase.sceneState().transparency;
  const bool supersample2x2 = (m_rendererBase.sceneState().geometryAAMode == GeometryAAMode::Supersample2x2);
  for (auto vFilter : vFilters) {
    if (vFilter->isReady(eye) && vFilter->hasOpaque(eye)) {
      normalOpaqueFilters.push_back(vFilter);
    }
    if (vFilter->isSelected()) {
      selectedFilters.push_back(vFilter);
      if (vFilter->isTransformEnabled()) {
        showHandleFilters.push_back(vFilter);
      }
    }
  }
  // if (m_renderGeometries.get()) {
  for (auto geomFilter : filters) {
    if (geomFilter->isReady(eye) && (geomFilter->opacity() > 0.0)) {
      if (geomFilter->hasOpaque(eye)) {
        if (geomFilter->isStayOnTop()) {
          onTopOpaqueFilters.push_back(geomFilter);
        } else {
          normalOpaqueFilters.push_back(geomFilter);
        }
      }
      if (geomFilter->hasTransparent(eye)) {
        if (geomFilter->isStayOnTop()) {
          onTopTransparentFilters.push_back(geomFilter);
        } else {
          normalTransparentFilters.push_back(geomFilter);
        }
      }
    }
    if (geomFilter->isSelected()) {
      selectedFilters.push_back(geomFilter);
      if (geomFilter->isTransformEnabled()) {
        showHandleFilters.push_back(geomFilter);
      }
    }
  }
  //}
  size_t numNormalFilters = normalOpaqueFilters.size() + normalTransparentFilters.size();
  size_t numOnTopFilters = onTopOpaqueFilters.size() + onTopTransparentFilters.size();

  ensureOutputTargets(m_outputSize);

  // If we need to overlay handles, render the scene into a pooled temp first
  Z3DScratchResourcePool::RenderTargetLease overlayLease;
  Z3DScratchResourcePool::RenderTargetLease* currentOutLease = nullptr;
  Z3DRenderTarget* currentOutPtr = nullptr;
  if (!showHandleFilters.empty()) {
    overlayLease = Z3DRenderGlobalState::instance().scratchPool().acquireTempRenderTarget2D(
      m_monoCurrentTarget->renderTarget->size());
    // VLOG(1) << "lease acquired";
    currentOutLease = &overlayLease;
    currentOutPtr = overlayLease.renderTarget;
  } else {
    currentOutLease = (eye == MonoEye)   ? m_monoCurrentTarget
                      : (eye == LeftEye) ? m_leftCurrentTarget
                                         : m_rightCurrentTarget;
    currentOutPtr = currentOutLease->renderTarget;
  }
  CHECK(currentOutPtr != nullptr);
  auto& currentOutRenderTarget = *currentOutPtr;

  const bool anyVolumeReady = std::any_of(vFilters.begin(), vFilters.end(), [eye](const Z3DImgFilter* filter) {
    return filter && filter->isReady(eye) && filter->hasTransparent(eye);
  });

  // Debug aid: during incremental mesh streaming we occasionally observe frames where the
  // volume appears missing even though the image filter is in the pipeline. The compositor
  // chooses the geometry-only path when `anyVolumeReady == false`; log that decision (and
  // per-volume readiness) on transitions and when volumes are unexpectedly not ready.
  //
  // Rate-limited to avoid flooding logs in normal interactive use.
  {
    static std::optional<bool> s_lastAnyVolumeReady;
    static auto s_lastLogTime = std::chrono::steady_clock::now() - std::chrono::hours(24);
    constexpr auto kLogInterval = std::chrono::milliseconds(750);

    const bool changed = !s_lastAnyVolumeReady || (*s_lastAnyVolumeReady != anyVolumeReady);
    const bool unexpectedNoVolume = (!anyVolumeReady && !vFilters.empty());
    const auto now = std::chrono::steady_clock::now();
    if ((changed || unexpectedNoVolume) && (now - s_lastLogTime) > kLogInterval) {
      s_lastAnyVolumeReady = anyVolumeReady;
      s_lastLogTime = now;

      VLOG(1) << fmt::format(
        "Z3DCompositor volume decision (GL): anyVolumeReady={} volFilters={} geomFilters={} eye={}",
        anyVolumeReady,
        vFilters.size(),
        filters.size(),
        static_cast<int>(eye));
      if (VLOG_IS_ON(2)) {
        size_t readyCount = 0;
        size_t transparentCount = 0;
        size_t hasImageCount = 0;
        for (size_t i = 0; i < vFilters.size(); ++i) {
          const Z3DImgFilter* vf = vFilters[i];
          if (!vf) {
            VLOG(2) << fmt::format("  vol[{}]: <null>", i);
            continue;
          }
          const bool ready = vf->isReady(eye);
          const bool valid = vf->isValid(eye);
          const bool hasImage = vf->hasImage();
          const bool hasTrans = vf->hasTransparent(eye);
          // `m_transparentValid` is the internal flag backing hasTransparent().
          const bool transValid = vf->m_transparentValid[static_cast<size_t>(eye)];
          readyCount += ready ? 1u : 0u;
          transparentCount += hasTrans ? 1u : 0u;
          hasImageCount += hasImage ? 1u : 0u;

          VLOG(2) << fmt::format(
            "  vol[{}]: {} ptr={} ready={} valid={} hasImage={} hasTransparent={} transparentValid={} stayOnTop={}",
            i,
            vf->className().toStdString(),
            (const void*)vf,
            ready,
            valid,
            hasImage,
            hasTrans,
            transValid,
            vf->isStayOnTop());
        }
        VLOG(2) << fmt::format("  summary: ready={} hasImage={} hasTransparent={}",
                               readyCount,
                               hasImageCount,
                               transparentCount);
      }
    }
  }

  glEnable(GL_DEPTH_TEST);

  if (transparencyMode == TransparencyMode::BlendNoDepthMask || transparencyMode == TransparencyMode::BlendDelayed) {
    if (!anyVolumeReady) { // no volume, only geometrys to render
      if (numNormalFilters == 0 || numOnTopFilters == 0) {
        // Acquire temp for geometry-only path (optionally twice the size)
        Z3DScratchResourcePool::RenderTargetLease temp1Lease =
          Z3DRenderGlobalState::instance().scratchPool().acquireTempRenderTarget2D(
            supersample2x2 ? (currentOutRenderTarget.size() * 2_u32) : currentOutRenderTarget.size());
        // VLOG(1) << "lease acquired";

        if (numOnTopFilters == 0) {
          renderGeometries(normalOpaqueFilters, normalTransparentFilters, temp1Lease, eye);
        } else {
          renderGeometries(onTopOpaqueFilters, onTopTransparentFilters, temp1Lease, eye);
        }

        // copy to out
        currentOutRenderTarget.bind();
        currentOutRenderTarget.clear();
        setViewport(currentOutRenderTarget.size());

        if (m_showBackground.get()) {
          m_rendererBase.render(eye, m_backgroundRenderer);
          glEnable(GL_BLEND);
          glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        }
        glDepthFunc(GL_ALWAYS);
        m_textureCopyRenderer.setColorTexture(temp1Lease.renderTarget->colorTexture());
        m_textureCopyRenderer.setDepthTexture(temp1Lease.renderTarget->depthTexture());
        m_rendererBase.render(eye, m_textureCopyRenderer);
        glDepthFunc(GL_LESS);
        if (m_showBackground.get() || m_showAxis.get()) {
          glBlendFunc(GL_ONE, GL_ZERO);
          glDisable(GL_BLEND);
        }

        currentOutRenderTarget.release();
      } else {
        auto tempSize = supersample2x2 ? (currentOutRenderTarget.size() * 2_u32) : currentOutRenderTarget.size();
        Z3DScratchResourcePool::RenderTargetLease temp1Lease =
          Z3DRenderGlobalState::instance().scratchPool().acquireTempRenderTarget2D(tempSize);
        // VLOG(1) << "lease acquired";
        Z3DScratchResourcePool::RenderTargetLease temp2Lease =
          Z3DRenderGlobalState::instance().scratchPool().acquireTempRenderTarget2D(tempSize);
        // VLOG(1) << "lease acquired";
        Z3DScratchResourcePool::RenderTargetLease tempCompositeLease;
        Z3DRenderTarget* blendTarget = &currentOutRenderTarget;
        if (supersample2x2) {
          tempCompositeLease = Z3DRenderGlobalState::instance().scratchPool().acquireTempRenderTarget2D(tempSize);
          blendTarget = tempCompositeLease.renderTarget;
        }

        // render normal geometries to tempport
        renderGeometries(normalOpaqueFilters, normalTransparentFilters, temp1Lease, eye);

        // render on top geometries to tempport2
        renderGeometries(onTopOpaqueFilters, onTopTransparentFilters, temp2Lease, eye);

        // Blend in supersampled space first when AA=2x2, then resolve once
        // into the final output to match Vulkan ordering.
        blendTarget->bind();
        blendTarget->clear();
        setViewport(blendTarget->size());

        if (!supersample2x2 && m_showBackground.get()) {
          m_rendererBase.render(eye, m_backgroundRenderer);
          glEnable(GL_BLEND);
          glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        }
        m_firstOnTopBlendRenderer.setColorTexture1(temp2Lease.renderTarget->colorTexture());
        m_firstOnTopBlendRenderer.setDepthTexture1(temp2Lease.renderTarget->depthTexture());
        m_firstOnTopBlendRenderer.setColorTexture2(temp1Lease.renderTarget->colorTexture());
        m_firstOnTopBlendRenderer.setDepthTexture2(temp1Lease.renderTarget->depthTexture());
        m_rendererBase.render(eye, m_firstOnTopBlendRenderer);
        blendTarget->release();

        if (supersample2x2) {
          currentOutRenderTarget.bind();
          currentOutRenderTarget.clear();
          setViewport(currentOutRenderTarget.size());

          if (m_showBackground.get()) {
            m_rendererBase.render(eye, m_backgroundRenderer);
            glEnable(GL_BLEND);
            glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
          }
          glDepthFunc(GL_ALWAYS);
          m_textureCopyRenderer.setColorTexture(tempCompositeLease.renderTarget->colorTexture());
          m_textureCopyRenderer.setDepthTexture(tempCompositeLease.renderTarget->depthTexture());
          m_rendererBase.render(eye, m_textureCopyRenderer);
          glDepthFunc(GL_LESS);
        }

        if (m_showBackground.get() || m_showAxis.get()) {
          glBlendFunc(GL_ONE, GL_ZERO);
          glDisable(GL_BLEND);
        }

        currentOutRenderTarget.release();
      }
    } else { // with volume
      // Only collect non-opaque image layers; opaque ones were rendered via the opaque pass
      auto nonOpaqueLayers = collectNonOpaqueImageLayers(eye);
      CHECK(!nonOpaqueLayers.empty()) << "should have images";
      if (numNormalFilters == 0 && numOnTopFilters == 0) { // directly copy image to out
        const Z3DTexture* colorTex = nullptr;
        const Z3DTexture* depthTex = nullptr;
        mergeImageLayers(nonOpaqueLayers, eye, *currentOutLease, colorTex, depthTex);

        currentOutRenderTarget.bind();
        currentOutRenderTarget.clear();
        setViewport(currentOutRenderTarget.size());

        if (m_showBackground.get()) {
          m_rendererBase.render(eye, m_backgroundRenderer);
          glEnable(GL_BLEND);
          glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        }
        glDepthFunc(GL_ALWAYS);
        m_textureCopyRenderer.setColorTexture(colorTex);
        m_textureCopyRenderer.setDepthTexture(depthTex);
        m_rendererBase.render(eye, m_textureCopyRenderer);
        glDepthFunc(GL_LESS);
        if (m_showBackground.get() || m_showAxis.get()) {
          glBlendFunc(GL_ONE, GL_ZERO);
          glDisable(GL_BLEND);
        }

        currentOutRenderTarget.release();
      } else if (numNormalFilters == 0 ||
                 numOnTopFilters == 0) { // render geometries into one temp port then blend with volume
        Z3DScratchResourcePool::RenderTargetLease tempGeoLease =
          Z3DRenderGlobalState::instance().scratchPool().acquireTempRenderTarget2D(
            supersample2x2 ? (currentOutRenderTarget.size() * 2_u32) : currentOutRenderTarget.size());
        // VLOG(1) << "lease acquired";

        // render geometries into one temp port
        if (numOnTopFilters == 0) {
          renderGeometries(normalOpaqueFilters, normalTransparentFilters, tempGeoLease, eye);
        } else {
          renderGeometries(onTopOpaqueFilters, onTopTransparentFilters, tempGeoLease, eye);
        }

        const Z3DTexture* colorTex = nullptr;
        const Z3DTexture* depthTex = nullptr;
        mergeImageLayers(nonOpaqueLayers, eye, *currentOutLease, colorTex, depthTex);

        // blend tempPort with volume
        currentOutRenderTarget.bind();
        currentOutRenderTarget.clear();
        setViewport(currentOutRenderTarget.size());

        if (m_showBackground.get()) {
          m_rendererBase.render(eye, m_backgroundRenderer);
          glEnable(GL_BLEND);
          glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        }

        if (numOnTopFilters == 0) {
          m_alphaBlendRenderer.setColorTexture1(tempGeoLease.renderTarget->colorTexture());
          m_alphaBlendRenderer.setDepthTexture1(tempGeoLease.renderTarget->depthTexture());
          m_alphaBlendRenderer.setColorTexture2(colorTex);
          m_alphaBlendRenderer.setDepthTexture2(depthTex);
          m_rendererBase.render(eye, m_alphaBlendRenderer);
        } else {
          m_firstOnTopBlendRenderer.setColorTexture1(tempGeoLease.renderTarget->colorTexture());
          m_firstOnTopBlendRenderer.setDepthTexture1(tempGeoLease.renderTarget->depthTexture());
          m_firstOnTopBlendRenderer.setColorTexture2(colorTex);
          m_firstOnTopBlendRenderer.setDepthTexture2(depthTex);
          m_rendererBase.render(eye, m_firstOnTopBlendRenderer);
        }

        if (m_showBackground.get() || m_showAxis.get()) {
          glBlendFunc(GL_ONE, GL_ZERO);
          glDisable(GL_BLEND);
        }

        currentOutRenderTarget.release();
      } else { // render normal geometries into tempport, then blend inport and tempport into tempport2, then render on
               // top geometries into tempport, then
        // blend into out
        auto tempSize2 = supersample2x2 ? (currentOutRenderTarget.size() * 2_u32) : currentOutRenderTarget.size();
        Z3DScratchResourcePool::RenderTargetLease temp1LeaseA =
          Z3DRenderGlobalState::instance().scratchPool().acquireTempRenderTarget2D(tempSize2);
        // VLOG(1) << "lease acquired";
        Z3DScratchResourcePool::RenderTargetLease temp2LeaseA =
          Z3DRenderGlobalState::instance().scratchPool().acquireTempRenderTarget2D(tempSize2);
        // VLOG(1) << "lease acquired";
        Z3DScratchResourcePool::RenderTargetLease tempCompositeLeaseA;
        Z3DRenderTarget* blendTargetA = &currentOutRenderTarget;
        if (supersample2x2) {
          tempCompositeLeaseA = Z3DRenderGlobalState::instance().scratchPool().acquireTempRenderTarget2D(tempSize2);
          blendTargetA = tempCompositeLeaseA.renderTarget;
        }

        // render normal geometries into tempport
        renderGeometries(normalOpaqueFilters, normalTransparentFilters, temp1LeaseA, eye);

        const Z3DTexture* colorTex = nullptr;
        const Z3DTexture* depthTex = nullptr;
        mergeImageLayers(nonOpaqueLayers, eye, *currentOutLease, colorTex, depthTex);

        // blend inport and tempport into tempport2
        temp2LeaseA.renderTarget->bind();
        setViewport(temp2LeaseA.renderTarget->size());
        m_alphaBlendRenderer.setColorTexture1(temp1LeaseA.renderTarget->colorTexture());
        m_alphaBlendRenderer.setDepthTexture1(temp1LeaseA.renderTarget->depthTexture());
        m_alphaBlendRenderer.setColorTexture2(colorTex);
        m_alphaBlendRenderer.setDepthTexture2(depthTex);
        m_rendererBase.render(eye, m_alphaBlendRenderer);
        temp2LeaseA.renderTarget->release();

        // render on top geometries into tempport
        renderGeometries(onTopOpaqueFilters, onTopTransparentFilters, temp1LeaseA, eye);

        // Blend the supersampled intermediate scene and on-top overlays in
        // supersampled space first, then resolve once into the final output.
        blendTargetA->bind();
        blendTargetA->clear();
        setViewport(blendTargetA->size());

        if (!supersample2x2 && m_showBackground.get()) {
          m_rendererBase.render(eye, m_backgroundRenderer);
          glEnable(GL_BLEND);
          glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        }
        m_firstOnTopBlendRenderer.setColorTexture1(temp1LeaseA.renderTarget->colorTexture());
        m_firstOnTopBlendRenderer.setDepthTexture1(temp1LeaseA.renderTarget->depthTexture());
        m_firstOnTopBlendRenderer.setColorTexture2(temp2LeaseA.renderTarget->colorTexture());
        m_firstOnTopBlendRenderer.setDepthTexture2(temp2LeaseA.renderTarget->depthTexture());
        m_rendererBase.render(eye, m_firstOnTopBlendRenderer);
        blendTargetA->release();

        if (supersample2x2) {
          currentOutRenderTarget.bind();
          currentOutRenderTarget.clear();
          setViewport(currentOutRenderTarget.size());

          if (m_showBackground.get()) {
            m_rendererBase.render(eye, m_backgroundRenderer);
            glEnable(GL_BLEND);
            glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
          }
          glDepthFunc(GL_ALWAYS);
          m_textureCopyRenderer.setColorTexture(tempCompositeLeaseA.renderTarget->colorTexture());
          m_textureCopyRenderer.setDepthTexture(tempCompositeLeaseA.renderTarget->depthTexture());
          m_rendererBase.render(eye, m_textureCopyRenderer);
          glDepthFunc(GL_LESS);
        }

        if (m_showBackground.get() || m_showAxis.get()) {
          glBlendFunc(GL_ONE, GL_ZERO);
          glDisable(GL_BLEND);
        }

        currentOutRenderTarget.release();
      }
    }
  } else { // OIT
    //    for (auto vFilter : vFilters) {
    //      if (vFilter->isReady(eye) && vFilter->hasTransparent(eye)) {
    //        normalTransparentFilters.push_back(vFilter);
    //      }
    //    }

    // Do not pre-merge image layers here; OIT path collects each non-opaque layer
    // individually in renderGeomsOIT for correct per-layer composition.
    numNormalFilters = normalOpaqueFilters.size() + normalTransparentFilters.size() + vFilters.size();
    if (numNormalFilters == 0 || numOnTopFilters == 0) {
      Z3DScratchResourcePool::RenderTargetLease temp1Lease =
        Z3DRenderGlobalState::instance().scratchPool().acquireTempRenderTarget2D(
          supersample2x2 ? (currentOutRenderTarget.size() * 2_u32) : currentOutRenderTarget.size());
      // VLOG(1) << "lease acquired";

      if (numOnTopFilters == 0) {
        renderGeometries(normalOpaqueFilters, normalTransparentFilters, temp1Lease, eye);
      } else {
        renderGeometries(onTopOpaqueFilters, onTopTransparentFilters, temp1Lease, eye);
      }

      // copy to out
      currentOutRenderTarget.bind();
      currentOutRenderTarget.clear();
      setViewport(currentOutRenderTarget.size());

      if (m_showBackground.get()) {
        m_rendererBase.render(eye, m_backgroundRenderer);
        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
      }
      glDepthFunc(GL_ALWAYS);
      m_textureCopyRenderer.setColorTexture(temp1Lease.renderTarget->colorTexture());
      m_textureCopyRenderer.setDepthTexture(temp1Lease.renderTarget->depthTexture());
      m_rendererBase.render(eye, m_textureCopyRenderer);
      glDepthFunc(GL_LESS);
      if (m_showBackground.get() || m_showAxis.get()) {
        glBlendFunc(GL_ONE, GL_ZERO);
        glDisable(GL_BLEND);
      }

      currentOutRenderTarget.release();
    } else {
      auto tempSize = supersample2x2 ? (currentOutRenderTarget.size() * 2_u32) : currentOutRenderTarget.size();
      Z3DScratchResourcePool::RenderTargetLease temp1Lease2 =
        Z3DRenderGlobalState::instance().scratchPool().acquireTempRenderTarget2D(tempSize);
      // VLOG(1) << "lease acquired";
      Z3DScratchResourcePool::RenderTargetLease temp2Lease2 =
        Z3DRenderGlobalState::instance().scratchPool().acquireTempRenderTarget2D(tempSize);
      // VLOG(1) << "lease acquired";
      Z3DScratchResourcePool::RenderTargetLease tempCompositeLease2;
      Z3DRenderTarget* blendTarget = &currentOutRenderTarget;
      if (supersample2x2) {
        tempCompositeLease2 = Z3DRenderGlobalState::instance().scratchPool().acquireTempRenderTarget2D(tempSize);
        blendTarget = tempCompositeLease2.renderTarget;
      }

      // render normal geometries to tempport
      renderGeometries(normalOpaqueFilters, normalTransparentFilters, temp1Lease2, eye);

      // render on top geometries to tempport2
      renderGeometries(onTopOpaqueFilters, onTopTransparentFilters, temp2Lease2, eye);

      // Blend in supersampled space first when AA=2x2, then resolve once into
      // the final output. This matches the Vulkan ordering and avoids
      // backend-specific downsampling during the on-top blend itself.
      blendTarget->bind();
      blendTarget->clear();
      setViewport(blendTarget->size());

      if (!supersample2x2 && m_showBackground.get()) {
        m_rendererBase.render(eye, m_backgroundRenderer);
        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
      }
      m_firstOnTopBlendRenderer.setColorTexture1(temp2Lease2.renderTarget->colorTexture());
      m_firstOnTopBlendRenderer.setDepthTexture1(temp2Lease2.renderTarget->depthTexture());
      m_firstOnTopBlendRenderer.setColorTexture2(temp1Lease2.renderTarget->colorTexture());
      m_firstOnTopBlendRenderer.setDepthTexture2(temp1Lease2.renderTarget->depthTexture());
      m_rendererBase.render(eye, m_firstOnTopBlendRenderer);
      blendTarget->release();

      if (supersample2x2) {
        currentOutRenderTarget.bind();
        currentOutRenderTarget.clear();
        setViewport(currentOutRenderTarget.size());

        if (m_showBackground.get()) {
          m_rendererBase.render(eye, m_backgroundRenderer);
          glEnable(GL_BLEND);
          glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        }
        glDepthFunc(GL_ALWAYS);
        m_textureCopyRenderer.setColorTexture(tempCompositeLease2.renderTarget->colorTexture());
        m_textureCopyRenderer.setDepthTexture(tempCompositeLease2.renderTarget->depthTexture());
        m_rendererBase.render(eye, m_textureCopyRenderer);
        glDepthFunc(GL_LESS);
      }

      if (m_showBackground.get() || m_showAxis.get()) {
        glBlendFunc(GL_ONE, GL_ZERO);
        glDisable(GL_BLEND);
      }

      currentOutRenderTarget.release();
    }
  }

  if (!showHandleFilters.empty()) {
    auto* finalOutLease = (eye == MonoEye)   ? m_monoCurrentTarget
                          : (eye == LeftEye) ? m_leftCurrentTarget
                                             : m_rightCurrentTarget;
    auto& finalOutRenderTarget = *finalOutLease->renderTarget;
    Z3DScratchResourcePool::RenderTargetLease handleLease =
      Z3DRenderGlobalState::instance().scratchPool().acquireTempRenderTarget2D(finalOutRenderTarget.size());
    // VLOG(1) << "lease acquired";

    handleLease.renderTarget->bind();
    handleLease.renderTarget->clear();
    for (auto& showHandleFilter : showHandleFilters) {
      showHandleFilter->setViewport(handleLease.renderTarget->size());
      showHandleFilter->renderHandle(eye);
    }
    handleLease.renderTarget->release();
    CHECK_GL_ERROR
    finalOutRenderTarget.bind();
    finalOutRenderTarget.clear();
    setViewport(finalOutRenderTarget.size());
    m_firstOnTopBlendRenderer.setColorTexture1(handleLease.renderTarget->colorTexture());
    m_firstOnTopBlendRenderer.setDepthTexture1(handleLease.renderTarget->depthTexture());
    m_firstOnTopBlendRenderer.setColorTexture2(currentOutRenderTarget.colorTexture());
    m_firstOnTopBlendRenderer.setDepthTexture2(currentOutRenderTarget.depthTexture());
    m_rendererBase.render(eye, m_firstOnTopBlendRenderer);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    for (auto& selectedFilter : selectedFilters) {
      selectedFilter->setViewport(finalOutRenderTarget.size());
      selectedFilter->renderSelectionBox(eye);
    }
    glBlendFunc(GL_ONE, GL_ZERO);
    glDisable(GL_BLEND);
    finalOutRenderTarget.release();
    CHECK_GL_ERROR
  } else if (!selectedFilters.empty()) {
    currentOutRenderTarget.bind();
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    for (auto& selectedFilter : selectedFilters) {
      selectedFilter->setViewport(currentOutRenderTarget.size());
      selectedFilter->renderSelectionBox(eye);
    }
    glBlendFunc(GL_ONE, GL_ZERO);
    glDisable(GL_BLEND);
    currentOutRenderTarget.release();
    CHECK_GL_ERROR
  }

  if (m_showAxis.get()) {
    // Draw the axis overlay after all depth-tested overlays (e.g. selection boxes).
    //
    // The axis overlay clears the depth buffer inside the inset viewport so the widget can
    // depth-test itself while still sampling the main scene color. If we clear that depth
    // region earlier, later depth-tested overlays will "pop" on top of the scene within the
    // inset region because the scene depth has been wiped there.
    Z3DRenderTarget* axisOutRenderTarget = nullptr;
    if (!showHandleFilters.empty()) {
      auto* finalOutLease = (eye == MonoEye)   ? m_monoCurrentTarget
                            : (eye == LeftEye) ? m_leftCurrentTarget
                                               : m_rightCurrentTarget;
      axisOutRenderTarget = finalOutLease->renderTarget;
    } else {
      axisOutRenderTarget = &currentOutRenderTarget;
    }
    CHECK(axisOutRenderTarget != nullptr);

    axisOutRenderTarget->bind();
    setViewport(axisOutRenderTarget->size());
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    renderAxis(eye);
    glBlendFunc(GL_ONE, GL_ZERO);
    glDisable(GL_BLEND);
    axisOutRenderTarget->release();
    CHECK_GL_ERROR
  }

  // render picking objects
  ensurePickingTarget(m_monoCurrentTarget->renderTarget->size());
  if (filters.empty() && !showHandleFilters.empty()) {
    pickingManager().bindTarget();
    pickingManager().clearTarget();
    for (auto& showHandleFilter : showHandleFilters) {
      showHandleFilter->setViewport(pickingManager().renderTarget().size());
      showHandleFilter->renderHandlePicking(eye);
    }
    pickingManager().releaseTarget();
  } else if (showHandleFilters.empty() && !filters.empty()) {
    pickingManager().bindTarget();
    pickingManager().clearTarget();
    for (auto geomFilter : filters) {
      if (geomFilter->isReady(eye)) {
        geomFilter->setViewport(pickingManager().renderTarget().size());
        geomFilter->renderPicking(eye);
      }
    }
    pickingManager().releaseTarget();
  } else if (!filters.empty() && !showHandleFilters.empty()) {
    auto pickSize = pickingManager().renderTarget().size();
    auto leaseHandles = Z3DRenderGlobalState::instance().scratchPool().acquireTempRenderTarget2D(pickSize);
    // VLOG(1) << "lease acquired";
    auto leaseGeoms = Z3DRenderGlobalState::instance().scratchPool().acquireTempRenderTarget2D(pickSize);
    // VLOG(1) << "lease acquired";

    leaseHandles.renderTarget->bind();
    leaseHandles.renderTarget->clear();
    for (auto& showHandleFilter : showHandleFilters) {
      showHandleFilter->setViewport(leaseHandles.renderTarget->size());
      showHandleFilter->renderHandlePicking(eye);
    }
    leaseHandles.renderTarget->release();

    leaseGeoms.renderTarget->bind();
    leaseGeoms.renderTarget->clear();
    for (auto geomFilter : filters) {
      if (geomFilter->isReady(eye)) {
        geomFilter->setViewport(leaseGeoms.renderTarget->size());
        geomFilter->renderPicking(eye);
        CHECK_GL_ERROR
      }
    }
    leaseGeoms.renderTarget->release();

    pickingManager().bindTarget();
    pickingManager().clearTarget();
    setViewport(pickingManager().renderTarget().size());
    m_firstOnTopRenderer.setColorTexture1(leaseHandles.renderTarget->colorTexture());
    m_firstOnTopRenderer.setDepthTexture1(leaseHandles.renderTarget->depthTexture());
    m_firstOnTopRenderer.setColorTexture2(leaseGeoms.renderTarget->colorTexture());
    m_firstOnTopRenderer.setDepthTexture2(leaseGeoms.renderTarget->depthTexture());
    m_rendererBase.render(eye, m_firstOnTopRenderer);
    pickingManager().releaseTarget();
  }

  glDisable(GL_DEPTH_TEST);

#if defined(ATLAS_USE_OPENGLWIDGET)
  glFinish();
#else
  // glFinish();
  // VLOG(1) << "start downloading texture";
  downloadTextureToLocalColorBuffer((eye == MonoEye)   ? *m_monoCurrentTarget->renderTarget->colorTexture()
                                    : (eye == LeftEye) ? *m_leftCurrentTarget->renderTarget->colorTexture()
                                                       : *m_rightCurrentTarget->renderTarget->colorTexture(),
                                    (eye == MonoEye)   ? *m_monoCurrentLocalBuffer
                                    : (eye == LeftEye) ? *m_leftCurrentLocalBuffer
                                                       : *m_rightCurrentLocalBuffer);
#endif

  if (eye == MonoEye) {
    {
      const std::scoped_lock lock(m_globalParameters.targetSwitchMutex);
      std::swap(m_monoReadyTarget, m_monoCurrentTarget);
      std::swap(m_monoReadyLocalBuffer, m_monoCurrentLocalBuffer);
    }

    m_globalParameters.hasNewRendering = true;
    VLOG(1) << fmt::format("{} finished to {}",
                           m_progressiveRendering ? "progressive rendering" : "rendering",
                           (void*)m_monoReadyLocalBuffer);
    // Log scratch pool memory usage after the mono render completes
    const auto& pool = Z3DRenderGlobalState::instance().scratchPool();
    static uint64_t s_lastCreate = 0;
    static uint64_t s_lastChange = 0;
    static uint64_t s_lastReuse = 0;
    const uint64_t curCreate = pool.creationCounter();
    const uint64_t curChange = pool.changeCounter();
    const uint64_t curReuse = pool.reuseStatsCounter();
    if (curCreate != s_lastCreate || curChange != s_lastChange) {
      VLOG(1) << pool.describeMemoryUsage(true);
      s_lastCreate = curCreate;
      s_lastChange = curChange;
    }
    if (curReuse != s_lastReuse) {
      VLOG(1) << pool.describeReuseStats(true);
      s_lastReuse = curReuse;
    }
    Q_EMIT renderingFinished();

  } else if (eye == LeftEye) {
    {
      const std::scoped_lock lock(m_globalParameters.targetSwitchMutex);
      std::swap(m_leftReadyTarget, m_leftCurrentTarget);
      std::swap(m_leftReadyLocalBuffer, m_leftCurrentLocalBuffer);
    }
  } else {
    {
      const std::scoped_lock lock(m_globalParameters.targetSwitchMutex);
      std::swap(m_rightReadyTarget, m_rightCurrentTarget);
      std::swap(m_rightReadyLocalBuffer, m_rightCurrentLocalBuffer);
    }

    m_globalParameters.hasNewRendering = true;
    VLOG(1) << fmt::format("{} finished", m_progressiveRendering ? "progressive rendering" : "rendering");
    Q_EMIT renderingFinished();
  }

  return 1.0;
}

double Z3DCompositor::processVulkan(Z3DEye eye)
{
  syncRendererState();

  // Build basic opaque/transparent lists similar to GL path (no overlays yet)
  const auto& gFilters = m_geometryFilters;
  const auto& vFilters = m_volumeFilters;
  std::vector<Z3DBoundedFilter*> opaqueFilters;
  std::vector<Z3DBoundedFilter*> transparentFilters;
  std::vector<Z3DBoundedFilter*> onTopOpaqueFilters;
  std::vector<Z3DBoundedFilter*> onTopTransparentFilters;
  std::vector<Z3DBoundedFilter*> selectedFilters;
  std::vector<Z3DBoundedFilter*> showHandleFilters;

  for (auto* v : vFilters) {
    if (!v) {
      continue;
    }
    if (v->isReady(eye) && v->hasOpaque(eye)) {
      if (v->isStayOnTop()) {
        onTopOpaqueFilters.push_back(v);
      } else {
        opaqueFilters.push_back(v);
      }
    }
    if (v->isSelected()) {
      selectedFilters.push_back(v);
      if (v->isTransformEnabled()) {
        showHandleFilters.push_back(v);
      }
    }
  }
  for (auto* gf : gFilters) {
    if (!gf) {
      continue;
    }
    if (!gf->isReady(eye) || gf->opacity() <= 0.0f) {
      continue;
    }
    if (gf->hasOpaque(eye)) {
      if (gf->isStayOnTop()) {
        onTopOpaqueFilters.push_back(gf);
      } else {
        opaqueFilters.push_back(gf);
      }
    }
    if (gf->hasTransparent(eye)) {
      if (gf->isStayOnTop()) {
        onTopTransparentFilters.push_back(gf);
      } else {
        transparentFilters.push_back(gf);
      }
    }
    if (gf->isSelected()) {
      selectedFilters.push_back(gf);
      if (gf->isTransformEnabled()) {
        showHandleFilters.push_back(gf);
      }
    }
  }

  ensureOutputTargets(m_outputSize);

  // Use primary output lease according to eye
  Z3DScratchResourcePool::RenderTargetLease* outLease = nullptr;
  if (eye == MonoEye) {
    outLease = m_monoCurrentTarget;
  } else if (eye == LeftEye) {
    outLease = m_leftCurrentTarget;
  } else { // RightEye
    outLease = m_rightCurrentTarget; // mirrors current right-eye mapping in ctor
  }
  CHECK(outLease != nullptr) << "Compositor Vulkan path missing current output lease for eye";
  if (!outLease || !*outLease) {
    return 0.0;
  }

  Z3DLocalColorBuffer* frameLocalPtr = (eye == MonoEye)   ? m_monoCurrentLocalBuffer
                                       : (eye == LeftEye) ? m_leftCurrentLocalBuffer
                                                          : m_rightCurrentLocalBuffer;
  CHECK(frameLocalPtr != nullptr) << "Compositor Vulkan path missing current local color buffer for eye";

  auto* vulkanBackend = dynamic_cast<Z3DRendererVulkanBackend*>(m_rendererBase.backend());
  CHECK(vulkanBackend != nullptr) << "Compositor Vulkan path requires a Vulkan backend";
  CHECK(!m_rendererBase.isVulkanFrameActive())
    << "Compositor Vulkan path requires no active Vulkan frame (linear script owns submission boundaries)";

  // Ensure Vulkan device feature gates are cached before deciding OIT paths.
  // The Vulkan backend caches fragmentStoresAndAtomics/drawIndirectCount gates
  // on first device access (ensureDevice). Some compositor decisions (PPLL/indirect
  // count gating) happen before beginRender() opens a frame, so prime them here.
  static_cast<void>(vulkanBackend->device());

  // Supersample 2x2 parity (render to 2x scene lease, then downsample)
  // Note: Vulkan batches use the renderer's frame viewport to define the
  // render area. When rendering into a supersampled lease, we must temporarily
  // update the frame viewport to the supersampled size so that every pass
  // (background, geometry, OIT) covers the full attachment extent. Otherwise
  // only the lower-left quarter of the supersampled target would be written
  // and the final resolve would appear scaled down in the corner.
  const bool supersample2x2 = (m_rendererBase.sceneState().geometryAAMode == GeometryAAMode::Supersample2x2);
  const glm::uvec4 prevViewport = m_rendererBase.frameState().viewport;

  auto& pool = Z3DRenderGlobalState::instance().scratchPool();
  Z3DScratchResourcePool::RenderTargetLease sceneLease;
  Z3DScratchResourcePool::RenderTargetLease* sceneOutLease = outLease;
  if (supersample2x2) {
    const glm::uvec2 ssSize = m_outputSize * 2_u32;
    sceneLease =
      pool.acquireTempRenderTarget2D(ssSize, ScratchFormat::RGBA16, ScratchFormat::Depth32F, RenderBackend::Vulkan);
    sceneOutLease = &sceneLease;
    // Temporarily render with a supersampled viewport. The viewport is
    // restored to prevViewport right before the resolve step below.
    m_rendererBase.frameState().updateViewportData(ssSize);
  }

  // Decide OIT usage and collect non-opaque image layers (volumes/slices) once.
  const auto transparencyMode = m_rendererBase.sceneState().transparency;
  const bool useOIT = transparencyMode == TransparencyMode::DualDepthPeeling ||
                      transparencyMode == TransparencyMode::PerPixelFragmentList ||
                      transparencyMode == TransparencyMode::WeightedAverage ||
                      transparencyMode == TransparencyMode::WeightedBlended;
  const auto nonOpaqueLayers = collectNonOpaqueImageLayers(eye);
  const bool haveAnyNonOpaqueLayer = std::any_of(nonOpaqueLayers.begin(),
                                                 nonOpaqueLayers.end(),
                                                 [](const Z3DCompositorImageLayer& layer) {
                                                   const auto& c = layer.colorAttachment.handle;
                                                   const auto& d = layer.depthAttachment.handle;
                                                   return c.backend == RenderBackend::Vulkan && c.valid() &&
                                                          d.backend == RenderBackend::Vulkan && d.valid();
                                                 });
  const bool needTempSceneForNonOITImages = (!useOIT && haveAnyNonOpaqueLayer && (sceneOutLease == outLease));

  // Mirror GL: when handle/on-top overlays are present (or when we need to composite
  // non-opaque image layers in a non-OIT path), render the scene into a temporary
  // lease first. Later compositing passes must sample the scene color/depth while
  // writing to a different attachment set; Vulkan forbids read-while-write feedback.
  Z3DScratchResourcePool::RenderTargetLease sceneOverlayLease;
  Z3DScratchResourcePool::RenderTargetLease sceneCompositeLease; // supersampled composite target when needed
  Z3DScratchResourcePool::RenderTargetLease
    resolvedLease; // output-sized resolve target when SS + pixel-sized overlays need it
  const bool haveHandles = !showHandleFilters.empty();
  const bool haveOnTop = !onTopOpaqueFilters.empty() || !onTopTransparentFilters.empty();
  if (haveHandles || haveOnTop || needTempSceneForNonOITImages) {
    const glm::uvec2 sz = sceneOutLease->descriptor.size;
    sceneOverlayLease =
      pool.acquireTempRenderTarget2D(sz, ScratchFormat::RGBA16, ScratchFormat::Depth32F, RenderBackend::Vulkan);
    sceneOutLease = &sceneOverlayLease;
  }
  // When supersampling is enabled, pixel-sized overlays (transform handles,
  // selection boxes) must be rendered at the output resolution. To avoid Vulkan
  // read-while-write feedback, resolve the supersampled scene into a temporary
  // output-sized lease and composite overlays into the final outLease.
  const bool needResolvedLeaseForHandles = (supersample2x2 && haveHandles);
  if (needResolvedLeaseForHandles) {
    resolvedLease = pool.acquireTempRenderTarget2D(outLease->descriptor.size,
                                                   ScratchFormat::RGBA16,
                                                   ScratchFormat::Depth32F,
                                                   RenderBackend::Vulkan);
    CHECK(resolvedLease && resolvedLease.backend == RenderBackend::Vulkan)
      << "Failed to acquire Vulkan resolved scene lease for handle overlay";
  }

  // Declare the script after compositor scratch targets so its destructor flush
  // (submission) runs before those move-only leases release their scratch slots.
  ZVulkanLinearScript script(m_rendererBase, *vulkanBackend, /*frameLabel=*/{});

  // Vulkan frame/submission lifetime is owned by the linear script; the
  // compositor should only express segment order and explicit CPU readback
  // boundaries.
  bool imagesIntegratedViaOIT = false;
  bool imagesCompositedToFinal = false; // track ping-pong writes to final out (non-SS)

  auto recordFilterBatchesToSurface = [&](Z3DBoundedFilter* filter,
                                          const Z3DScratchResourcePool::RenderTargetLease& target,
                                          const glm::uvec2& viewportSize,
                                          auto&& renderFn) {
    if (!filter) {
      return;
    }
    auto& source = filter->rendererBase();
    const glm::uvec4 previousViewport = source.frameState().viewport;
    const auto previousSurface = source.frameState().activeSurface;
    auto restoreGuard = folly::makeGuard([&]() {
      filter->setViewport(previousViewport);
      source.setActiveSurfaceWithLoadStore(previousSurface, Z3DRendererBase::Preserve);
    });
    // Describe the target lease into a surface description, then preserve
    // the per-attachment load/store+clear policy from the current active
    // surface on the compositor renderer. This ensures that upstream calls
    // to setActiveSurfaceWithLoadStore (e.g., Clear vs. Load) are honored
    // when we forward recording through a child filter renderer.
    auto surfaceCopy = m_rendererBase.describeSurface(target);
    const auto& active = m_rendererBase.frameState().activeSurface;
    // Copy color attachment policies when indices align
    const size_t copyColorCount = std::min(surfaceCopy.colorAttachments.size(), active.colorAttachments.size());
    for (size_t i = 0; i < copyColorCount; ++i) {
      surfaceCopy.colorAttachments[i].loadOp = active.colorAttachments[i].loadOp;
      surfaceCopy.colorAttachments[i].storeOp = active.colorAttachments[i].storeOp;
      surfaceCopy.colorAttachments[i].clearValue = active.colorAttachments[i].clearValue;
    }
    // Depth policy
    if (surfaceCopy.depthAttachment && active.depthAttachment) {
      surfaceCopy.depthAttachment->loadOp = active.depthAttachment->loadOp;
      surfaceCopy.depthAttachment->storeOp = active.depthAttachment->storeOp;
      surfaceCopy.depthAttachment->clearValue = active.depthAttachment->clearValue;
    }
    filter->setViewport(glm::uvec4(0u, 0u, viewportSize.x, viewportSize.y));
    source.setActiveSurfaceWithLoadStore(surfaceCopy, Z3DRendererBase::Preserve);
    renderFn();
    auto& batches = source.cpuState().batches;
    for (auto& batch : batches) {
      if (batch.pass.colorAttachments.empty() && !surfaceCopy.colorAttachments.empty()) {
        batch.pass.colorAttachments = surfaceCopy.colorAttachments;
      }
      if (!batch.pass.depthAttachment.has_value() && surfaceCopy.depthAttachment.has_value()) {
        batch.pass.depthAttachment = surfaceCopy.depthAttachment;
      }
      if (batch.pass.viewport.extent == glm::vec2(0.0f)) {
        batch.pass.viewport.origin = glm::vec2(0.0f, 0.0f);
        batch.pass.viewport.extent = glm::vec2(static_cast<float>(viewportSize.x), static_cast<float>(viewportSize.y));
        batch.pass.viewport.minDepth = 0.0f;
        batch.pass.viewport.maxDepth = 1.0f;
      }
      m_rendererBase.appendBatch(std::move(batch));
    }
    source.resetCPUState();
  };

  // Only engage OIT when there is actual transparency to resolve (either
  // geometry with transparent fragments or non-opaque image layers). If we
  // only have opaque geometry, rendering via OIT paths can overwrite the
  // background with cleared textures (e.g. WA/WB accumulators), yielding a
  // white or incorrect background.
  if (useOIT && (!transparentFilters.empty() || !nonOpaqueLayers.empty())) {
    // Record background only; geometry/transparency handled by OIT path below
    recordSceneSegmentsVulkan({},
                              {},
                              *sceneOutLease,
                              eye,
                              /*includeGeometry*/ false,
                              /*clearAtStart*/ true,
                              /*drawBackground*/ m_showBackground.get(),
                              script);
    auto dispatchOIT = [&](Z3DScratchResourcePool::RenderTargetLease& lease,
                           AttachmentHandle depthHandle,
                           bool clearResolve) {
      switch (transparencyMode) {
        case TransparencyMode::DualDepthPeeling:
          renderTransparentDDPVulkan(transparentFilters,
                                     lease,
                                     eye,
                                     depthHandle,
                                     nonOpaqueLayers,
                                     clearResolve,
                                     script);
          break;
        case TransparencyMode::PerPixelFragmentList:
          renderTransparentPPLLVulkan(transparentFilters,
                                      lease,
                                      eye,
                                      depthHandle,
                                      nonOpaqueLayers,
                                      clearResolve,
                                      script);
          break;
        case TransparencyMode::WeightedAverage:
          renderTransparentWAVulkan(transparentFilters, lease, eye, depthHandle, nonOpaqueLayers, clearResolve, script);
          break;
        case TransparencyMode::WeightedBlended:
          renderTransparentWBVulkan(transparentFilters, lease, eye, depthHandle, nonOpaqueLayers, clearResolve, script);
          break;
        default:
          break;
      }
    };

    if (opaqueFilters.empty()) {
      // Without opaque geometry, the OIT resolve writes directly to the final
      // scene target. Preserve the target only if the background pass actually
      // initialized it; otherwise Vulkan must clear instead of loading from an
      // undefined layout.
      const bool backgroundDrawn = m_showBackground.get();
      dispatchOIT(*sceneOutLease, {}, /*clearResolve=*/!backgroundDrawn);
    } else {
      const glm::uvec2 targetSize = sceneOutLease->descriptor.size;
      auto leaseOpaque = std::make_shared<Z3DScratchResourcePool::RenderTargetLease>(
        pool.acquireTempRenderTarget2D(targetSize,
                                       ScratchFormat::RGBA16,
                                       ScratchFormat::Depth32F,
                                       RenderBackend::Vulkan));
      script.keepAlive(leaseOpaque);
      // Render opaque geometry into an opaque-only intermediate without drawing background
      recordSceneSegmentsVulkan(opaqueFilters,
                                {},
                                *leaseOpaque,
                                eye,
                                /*includeGeometry*/ true,
                                /*clearAtStart*/ true,
                                /*drawBackground*/ false,
                                script);

      auto leaseTrans = std::make_shared<Z3DScratchResourcePool::RenderTargetLease>(
        pool.acquireTempRenderTarget2D(targetSize,
                                       ScratchFormat::RGBA16,
                                       ScratchFormat::Depth32F,
                                       RenderBackend::Vulkan));
      script.keepAlive(leaseTrans);
      AttachmentHandle depthHandle;
      if (auto* depthTex = leaseOpaque->depthAttachmentTexture()) {
        depthHandle.backend = RenderBackend::Vulkan;
        depthHandle.index = 0;
        depthHandle.id = reinterpret_cast<uint64_t>(depthTex);
      }
      // Write OIT results into a temporary lease; clear it first
      dispatchOIT(*leaseTrans, depthHandle, /*clearResolve=*/true);

      // Compose transparent OIT result over opaque using premultiplied alpha.
      // Transparent contributions behind opaque were already culled during the
      // OIT init/peel passes via the provided depth attachment, so the final
      // resolve does not require an additional depth test here.
      AttachmentHandle opaqueColor{};
      opaqueColor.backend = RenderBackend::Vulkan;
      opaqueColor.index = 0;
      opaqueColor.id = reinterpret_cast<uint64_t>(leaseOpaque->colorAttachment(0));
      AttachmentHandle opaqueDepth{};
      opaqueDepth.backend = RenderBackend::Vulkan;
      opaqueDepth.index = 0;
      opaqueDepth.id = reinterpret_cast<uint64_t>(leaseOpaque->depthAttachmentTexture());
      AttachmentHandle transColor{};
      transColor.backend = RenderBackend::Vulkan;
      transColor.index = 0;
      transColor.id = reinterpret_cast<uint64_t>(leaseTrans->colorAttachment(0));
      AttachmentHandle transDepth{};
      transDepth.backend = RenderBackend::Vulkan;
      transDepth.index = 0;
      transDepth.id = reinterpret_cast<uint64_t>(leaseTrans->depthAttachmentTexture());

      // First-on-top blending expects source 0 to be the overlay and source 1
      // the base. Put transparent (overlay) in slot 0 and opaque (base) in 1.
      m_alphaBlendRenderer.setSourceAttachments0(transColor, transDepth);
      m_alphaBlendRenderer.setSourceAttachments1(opaqueColor, opaqueDepth);

      // Preserve the previously rendered background when composing OIT result
      // (GL draws background first and then overlays; mimic by loading color).
      vlogVulkanLease("transparency_resolve", *sceneOutLease);
      script.raster("transparency_resolve", {}, [&]() {
        m_rendererBase.setActiveSurfaceWithLoadStore(*sceneOutLease,
                                                     m_showBackground.get() ? LoadOp::Load : LoadOp::Clear,
                                                     StoreOp::Store,
                                                     LoadOp::Clear,
                                                     StoreOp::Store);
        // Enable fixed-function blending when a background was drawn so the
        // compositor shader output (premultiplied) blends over the background
        // instead of overwriting it with black where no geometry lies.
        m_alphaBlendRenderer.setEnableFixedBlend(m_showBackground.get());
        m_rendererBase.renderVulkan(eye, m_alphaBlendRenderer);
      });
    }

    imagesIntegratedViaOIT = true;
  } else {
    // No OIT path: background + geometry via single driver call
    recordSceneSegmentsVulkan(opaqueFilters,
                              transparentFilters,
                              *sceneOutLease,
                              eye,
                              /*includeGeometry*/ true,
                              /*clearAtStart*/ true,
                              /*drawBackground*/ m_showBackground.get(),
                              script);
  }

  auto applyGlowOverlay = [&](const std::vector<Z3DBoundedFilter*>& filters) {
    if (filters.empty()) {
      return;
    }
    // Determine output size from lease
    const glm::uvec2 targetSize = outLease->descriptor.size;
    for (auto* filter : filters) {
      if (!filter) {
        continue;
      }
      auto* glowPara = filter->parameter("Glow");
      auto* glowBool = glowPara ? dynamic_cast<ZBoolParameter*>(glowPara) : nullptr;
      if (!glowBool || !glowBool->get()) {
        continue;
      }

      // 1) render filter geometry to temp lease
      auto glowGeomLease = std::make_shared<Z3DScratchResourcePool::RenderTargetLease>(
        pool.acquireTempRenderTarget2D(targetSize,
                                       ScratchFormat::RGBA16,
                                       ScratchFormat::Depth32F,
                                       RenderBackend::Vulkan));
      script.keepAlive(glowGeomLease);
      auto blurXLease = std::make_shared<Z3DScratchResourcePool::RenderTargetLease>(
        pool.acquireTempRenderTarget2D(targetSize,
                                       ScratchFormat::RGBA16,
                                       ScratchFormat::Depth32F,
                                       RenderBackend::Vulkan));
      script.keepAlive(blurXLease);
      auto blurYLease = std::make_shared<Z3DScratchResourcePool::RenderTargetLease>(
        pool.acquireTempRenderTarget2D(targetSize,
                                       ScratchFormat::RGBA16,
                                       ScratchFormat::Depth32F,
                                       RenderBackend::Vulkan));
      script.keepAlive(blurYLease);
      // Clear temp attachments, record filter geometry
      vlogVulkanLease("glow_geometry", *glowGeomLease);
      const auto segGlowGeom = script.raster("glow_geometry", {}, [&]() {
        m_rendererBase.setActiveSurfaceWithLoadStore(*glowGeomLease,
                                                     LoadOp::Clear,
                                                     StoreOp::Store,
                                                     LoadOp::Clear,
                                                     StoreOp::Store);
        for (auto& att : m_rendererBase.frameState().activeSurface.colorAttachments) {
          att.finalUse = AttachmentFinalUse::Sampled;
        }
        if (m_rendererBase.frameState().activeSurface.depthAttachment) {
          m_rendererBase.frameState().activeSurface.depthAttachment->finalUse = AttachmentFinalUse::Sampled;
        }
        recordFilterBatchesToSurface(filter, *glowGeomLease, targetSize, [&]() {
          filter->renderOpaque(eye);
        });
      });

      // 2) sync glow params and render glow into the output surface
      if (auto* mode = dynamic_cast<ZStringIntOptionParameter*>(filter->parameter("Glow Mode"))) {
        m_glowRenderer.setGlowMode(static_cast<GlowMode>(mode->associatedData()));
      }
      if (auto* radius = dynamic_cast<ZIntParameter*>(filter->parameter("Glow Blur Radius"))) {
        m_glowRenderer.setBlurRadius(radius->get());
      }
      if (auto* scale = dynamic_cast<ZFloatParameter*>(filter->parameter("Glow Blur Scale"))) {
        m_glowRenderer.setBlurScale(scale->get());
      }
      if (auto* strength = dynamic_cast<ZFloatParameter*>(filter->parameter("Glow Blur Strength"))) {
        m_glowRenderer.setBlurStrength(strength->get());
      }

      AttachmentHandle colorHandle;
      colorHandle.backend = RenderBackend::Vulkan;
      colorHandle.index = 0;
      colorHandle.id = reinterpret_cast<uint64_t>(glowGeomLease->colorAttachment(0));
      AttachmentHandle depthHandle;
      depthHandle.backend = RenderBackend::Vulkan;
      depthHandle.index = 0;
      depthHandle.id = reinterpret_cast<uint64_t>(glowGeomLease->depthAttachmentTexture());

      AttachmentHandle blurXColorHandle{};
      blurXColorHandle.backend = RenderBackend::Vulkan;
      blurXColorHandle.index = 0;
      blurXColorHandle.id = reinterpret_cast<uint64_t>(blurXLease->colorAttachment(0));
      AttachmentHandle blurXDepthHandle{};
      blurXDepthHandle.backend = RenderBackend::Vulkan;
      blurXDepthHandle.index = 0;
      blurXDepthHandle.id = reinterpret_cast<uint64_t>(blurXLease->depthAttachmentTexture());

      AttachmentHandle blurYColorHandle{};
      blurYColorHandle.backend = RenderBackend::Vulkan;
      blurYColorHandle.index = 0;
      blurYColorHandle.id = reinterpret_cast<uint64_t>(blurYLease->colorAttachment(0));
      AttachmentHandle blurYDepthHandle{};
      blurYDepthHandle.backend = RenderBackend::Vulkan;
      blurYDepthHandle.index = 0;
      blurYDepthHandle.id = reinterpret_cast<uint64_t>(blurYLease->depthAttachmentTexture());

      // Do not clear when overlaying glow; choose destination matching recent composition
      {
        const bool compositedToFinalLocal = (!supersample2x2 && (haveHandles || haveOnTop || imagesCompositedToFinal));
        auto& dest = compositedToFinalLocal ? *outLease : *sceneOutLease;
        vlogVulkanLease("glow_composite", dest);
      }

      const auto segBlurX = script.raster("glow_blur_x", {segGlowGeom}, [&]() {
        m_rendererBase.setActiveSurfaceWithLoadStore(*blurXLease,
                                                     LoadOp::Clear,
                                                     StoreOp::Store,
                                                     LoadOp::Clear,
                                                     StoreOp::Store);
        for (auto& att : m_rendererBase.frameState().activeSurface.colorAttachments) {
          att.finalUse = AttachmentFinalUse::Sampled;
        }
        if (m_rendererBase.frameState().activeSurface.depthAttachment) {
          m_rendererBase.frameState().activeSurface.depthAttachment->finalUse = AttachmentFinalUse::Sampled;
        }

        TextureGlowPayload blurPayload{};
        blurPayload.stage = TextureGlowPayload::Stage::BlurX;
        blurPayload.mode = m_glowRenderer.glowMode();
        blurPayload.blurRadius = m_glowRenderer.blurRadius();
        blurPayload.blurScale = m_glowRenderer.blurScale();
        blurPayload.blurStrength = m_glowRenderer.blurStrength();
        blurPayload.colorAttachmentHandle = colorHandle;
        blurPayload.depthAttachmentHandle = depthHandle;

        RenderBatch batch{};
        batch.eye = eye;
        batch.pass.externalImageUses.push_back(
          {colorHandle, ExternalImageUseKind::SampledRead, ExternalImageAspectHint::Color});
        batch.pass.externalImageUses.push_back(
          {depthHandle, ExternalImageUseKind::SampledRead, ExternalImageAspectHint::Depth});
        batch.draw.topology = PrimitiveTopology::TriangleStrip;
        batch.draw.vertexCount = 4;
        batch.draw.indexCount = 0;
        batch.geometry = std::move(blurPayload);
        m_rendererBase.appendBatch(std::move(batch));
      });

      const auto segBlurY = script.raster("glow_blur_y", {segBlurX}, [&]() {
        m_rendererBase.setActiveSurfaceWithLoadStore(*blurYLease,
                                                     LoadOp::Clear,
                                                     StoreOp::Store,
                                                     LoadOp::Clear,
                                                     StoreOp::Store);
        for (auto& att : m_rendererBase.frameState().activeSurface.colorAttachments) {
          att.finalUse = AttachmentFinalUse::Sampled;
        }
        if (m_rendererBase.frameState().activeSurface.depthAttachment) {
          m_rendererBase.frameState().activeSurface.depthAttachment->finalUse = AttachmentFinalUse::Sampled;
        }

        TextureGlowPayload blurPayload{};
        blurPayload.stage = TextureGlowPayload::Stage::BlurY;
        blurPayload.mode = m_glowRenderer.glowMode();
        blurPayload.blurRadius = m_glowRenderer.blurRadius();
        blurPayload.blurScale = m_glowRenderer.blurScale();
        blurPayload.blurStrength = m_glowRenderer.blurStrength();
        blurPayload.colorAttachmentHandle = blurXColorHandle;
        blurPayload.depthAttachmentHandle = blurXDepthHandle;

        RenderBatch batch{};
        batch.eye = eye;
        batch.pass.externalImageUses.push_back(
          {blurXColorHandle, ExternalImageUseKind::SampledRead, ExternalImageAspectHint::Color});
        batch.pass.externalImageUses.push_back(
          {blurXDepthHandle, ExternalImageUseKind::SampledRead, ExternalImageAspectHint::Depth});
        batch.draw.topology = PrimitiveTopology::TriangleStrip;
        batch.draw.vertexCount = 4;
        batch.draw.indexCount = 0;
        batch.geometry = std::move(blurPayload);
        m_rendererBase.appendBatch(std::move(batch));
      });

      script.raster("glow_composite", {segBlurY}, [&]() {
        const bool compositedToFinalLocal = (!supersample2x2 && (haveHandles || haveOnTop || imagesCompositedToFinal));
        auto& dest = compositedToFinalLocal ? *outLease : *sceneOutLease;
        m_rendererBase.setActiveSurfaceWithLoadStore(dest,
                                                     LoadOp::Load,
                                                     StoreOp::Store,
                                                     LoadOp::DontCare,
                                                     StoreOp::Store);

        TextureGlowPayload glowPayload{};
        glowPayload.stage = TextureGlowPayload::Stage::Composite;
        glowPayload.mode = m_glowRenderer.glowMode();
        glowPayload.blurRadius = m_glowRenderer.blurRadius();
        glowPayload.blurScale = m_glowRenderer.blurScale();
        glowPayload.blurStrength = m_glowRenderer.blurStrength();
        glowPayload.colorAttachmentHandle = colorHandle;
        glowPayload.depthAttachmentHandle = depthHandle;
        glowPayload.blurColorAttachmentHandle = blurYColorHandle;
        glowPayload.blurDepthAttachmentHandle = blurYDepthHandle;

        RenderBatch batch{};
        batch.eye = eye;
        batch.pass.externalImageUses.push_back(
          {colorHandle, ExternalImageUseKind::SampledRead, ExternalImageAspectHint::Color});
        batch.pass.externalImageUses.push_back(
          {depthHandle, ExternalImageUseKind::SampledRead, ExternalImageAspectHint::Depth});
        batch.pass.externalImageUses.push_back(
          {blurYColorHandle, ExternalImageUseKind::SampledRead, ExternalImageAspectHint::Color});
        batch.pass.externalImageUses.push_back(
          {blurYDepthHandle, ExternalImageUseKind::SampledRead, ExternalImageAspectHint::Depth});
        // Let backend fill current active surface into batch if not set explicitly
        batch.draw.topology = PrimitiveTopology::TriangleStrip;
        batch.draw.vertexCount = 4;
        batch.draw.indexCount = 0;
        batch.geometry = std::move(glowPayload);
        m_rendererBase.appendBatch(std::move(batch));
      });
    }
  };

  // Glow overlays are now scheduled under the pass-graph driver in the non-OIT path.
  // In the OIT path, apply glow overlays here after OIT resolution.

  if (imagesIntegratedViaOIT) {
    applyGlowOverlay(transparentFilters);
    applyGlowOverlay(opaqueFilters);
  }

  // Collect per-filter Vulkan image layers (color+depth) and blend onto output
  // Skip if images already participated in OIT above
  if (!imagesIntegratedViaOIT) {
    using LayerHandles = std::pair<AttachmentHandle, AttachmentHandle>;
    std::vector<LayerHandles> layers;
    layers.reserve(nonOpaqueLayers.size());

    for (const auto& layer : nonOpaqueLayers) {
      const auto& colorDesc = layer.colorAttachment;
      const auto& depthDesc = layer.depthAttachment;
      if (colorDesc.handle.backend != RenderBackend::Vulkan || !colorDesc.handle.valid()) {
        continue;
      }
      if (depthDesc.handle.backend != RenderBackend::Vulkan || !depthDesc.handle.valid()) {
        continue;
      }
      layers.emplace_back(colorDesc.handle, depthDesc.handle);
    }

    if (!layers.empty()) {
      const glm::uvec2 tgtSize = outLease->descriptor.size;

      // Helper to build handles from a lease
      auto handlesFromLease = [](const Z3DScratchResourcePool::RenderTargetLease& lease) -> LayerHandles {
        AttachmentHandle c{};
        c.backend = RenderBackend::Vulkan;
        c.index = 0;
        c.id = reinterpret_cast<uint64_t>(lease.colorAttachment(0));
        AttachmentHandle d{};
        d.backend = RenderBackend::Vulkan;
        d.index = 0;
        d.id = reinterpret_cast<uint64_t>(lease.depthAttachmentTexture());
        return {c, d};
      };

      if (layers.size() == 1) {
        // Blend geometry (current scene) with the single image layer
        AttachmentHandle outC{};
        outC.backend = RenderBackend::Vulkan;
        outC.index = 0;
        outC.id = reinterpret_cast<uint64_t>(sceneOutLease->colorAttachment(0));
        AttachmentHandle outD{};
        outD.backend = RenderBackend::Vulkan;
        outD.index = 0;
        outD.id = reinterpret_cast<uint64_t>(sceneOutLease->depthAttachmentTexture());

        // Choose destination: final surface (non-SS) or supersampled composite (SS)
        if (supersample2x2) {
          if (!sceneCompositeLease) {
            const glm::uvec2 ssSize = sceneOutLease->descriptor.size;
            sceneCompositeLease = pool.acquireTempRenderTarget2D(ssSize,
                                                                 ScratchFormat::RGBA16,
                                                                 ScratchFormat::Depth32F,
                                                                 RenderBackend::Vulkan);
          }
        } else {
          imagesCompositedToFinal = true;
        }

        script.raster("image_blend_single", {}, [&]() {
          if (supersample2x2) {
            m_rendererBase.setActiveSurfaceWithLoadStore(sceneCompositeLease,
                                                         LoadOp::Clear,
                                                         StoreOp::Store,
                                                         LoadOp::Clear,
                                                         StoreOp::Store);
          } else {
            m_rendererBase.setActiveSurfaceWithLoadStore(*outLease,
                                                         LoadOp::Clear,
                                                         StoreOp::Store,
                                                         LoadOp::Clear,
                                                         StoreOp::Store);
          }

          m_alphaBlendRenderer.setEnableFixedBlend(m_showBackground.get());
          m_alphaBlendRenderer.setSourceAttachments0(outC, outD);
          m_alphaBlendRenderer.setSourceAttachments1(layers[0].first, layers[0].second);
          m_rendererBase.renderVulkan(eye, m_alphaBlendRenderer);
        });
        if (supersample2x2) {
          // Promote composite as the current scene for any follow-up overlays/passes
          sceneOutLease = &sceneCompositeLease;
        }
      } else {
        // Merge N image layers pairwise into an intermediate lease, then blend onto out
        auto mergeLeaseA = std::make_shared<Z3DScratchResourcePool::RenderTargetLease>(
          pool.acquireTempRenderTarget2D(tgtSize,
                                         ScratchFormat::RGBA16,
                                         ScratchFormat::Depth32F,
                                         RenderBackend::Vulkan));
        auto mergeLeaseB = std::make_shared<Z3DScratchResourcePool::RenderTargetLease>(
          pool.acquireTempRenderTarget2D(tgtSize,
                                         ScratchFormat::RGBA16,
                                         ScratchFormat::Depth32F,
                                         RenderBackend::Vulkan));
        script.keepAlive(mergeLeaseA);
        script.keepAlive(mergeLeaseB);

        // First merge: layers[0] and layers[1] -> A
        script.raster("image_merge_initial", {}, [&]() {
          m_rendererBase.setActiveSurfaceWithLoadStore(*mergeLeaseA,
                                                       LoadOp::Clear,
                                                       StoreOp::Store,
                                                       LoadOp::Clear,
                                                       StoreOp::Store);
          m_MIPImageAlphaBlendRenderer.setSourceAttachments0(layers[0].first, layers[0].second);
          m_MIPImageAlphaBlendRenderer.setSourceAttachments1(layers[1].first, layers[1].second);
          m_rendererBase.renderVulkan(eye, m_MIPImageAlphaBlendRenderer);
        });

        // Subsequent merges: (A + layers[i]) -> B, then swap
        auto resHandles = handlesFromLease(*mergeLeaseA);
        for (size_t i = 2; i < layers.size(); ++i) {
          script.raster("image_merge_iter", {}, [&]() {
            m_rendererBase.setActiveSurfaceWithLoadStore(*mergeLeaseB,
                                                         LoadOp::Clear,
                                                         StoreOp::Store,
                                                         LoadOp::Clear,
                                                         StoreOp::Store);
            m_MIPImageAlphaBlendRenderer.setSourceAttachments0(resHandles.first, resHandles.second);
            m_MIPImageAlphaBlendRenderer.setSourceAttachments1(layers[i].first, layers[i].second);
            m_rendererBase.renderVulkan(eye, m_MIPImageAlphaBlendRenderer);
          });
          // swap A<->B and update resHandles
          std::swap(mergeLeaseA, mergeLeaseB);
          resHandles = handlesFromLease(*mergeLeaseA);
        }

        // Blend geometry (out) with final merged image (res in mergeLeaseA)
        AttachmentHandle outC{};
        outC.backend = RenderBackend::Vulkan;
        outC.index = 0;
        outC.id = reinterpret_cast<uint64_t>(sceneOutLease->colorAttachment(0));
        AttachmentHandle outD{};
        outD.backend = RenderBackend::Vulkan;
        outD.index = 0;
        outD.id = reinterpret_cast<uint64_t>(sceneOutLease->depthAttachmentTexture());

        // Destination selection: non-SS→final out, SS→supersampled composite
        if (supersample2x2) {
          if (!sceneCompositeLease) {
            const glm::uvec2 ssSize = sceneOutLease->descriptor.size;
            sceneCompositeLease = pool.acquireTempRenderTarget2D(ssSize,
                                                                 ScratchFormat::RGBA16,
                                                                 ScratchFormat::Depth32F,
                                                                 RenderBackend::Vulkan);
          }
        } else {
          imagesCompositedToFinal = true;
        }

        script.raster("image_merge_finalize", {}, [&]() {
          if (supersample2x2) {
            m_rendererBase.setActiveSurfaceWithLoadStore(sceneCompositeLease,
                                                         LoadOp::Clear,
                                                         StoreOp::Store,
                                                         LoadOp::Clear,
                                                         StoreOp::Store);
          } else {
            m_rendererBase.setActiveSurfaceWithLoadStore(*outLease,
                                                         LoadOp::Clear,
                                                         StoreOp::Store,
                                                         LoadOp::Clear,
                                                         StoreOp::Store);
          }

          m_alphaBlendRenderer.setEnableFixedBlend(m_showBackground.get());
          m_alphaBlendRenderer.setSourceAttachments0(outC, outD);
          m_alphaBlendRenderer.setSourceAttachments1(resHandles.first, resHandles.second);
          m_rendererBase.renderVulkan(eye, m_alphaBlendRenderer);
        });
        if (supersample2x2) {
          sceneOutLease = &sceneCompositeLease;
        }
      }
    }
  }

  // Render stay-on-top filters by composing them into a separate lease,
  // then alpha-blending the result on top of the final scene. This ensures
  // on-top content is not depth-tested against previously rendered geometry.
  if (!onTopOpaqueFilters.empty() || !onTopTransparentFilters.empty()) {
    const glm::uvec2 targetSize = sceneOutLease->descriptor.size;
    const bool useOITOnTop = m_rendererBase.sceneState().transparency == TransparencyMode::DualDepthPeeling ||
                             m_rendererBase.sceneState().transparency == TransparencyMode::PerPixelFragmentList ||
                             m_rendererBase.sceneState().transparency == TransparencyMode::WeightedAverage ||
                             m_rendererBase.sceneState().transparency == TransparencyMode::WeightedBlended;

    if (!useOITOnTop) {
      // Non-OIT: render all on-top (opaque+transparent) into a temp lease, then overlay.
      auto onTopLease = std::make_shared<Z3DScratchResourcePool::RenderTargetLease>(
        pool.acquireTempRenderTarget2D(targetSize,
                                       ScratchFormat::RGBA16,
                                       ScratchFormat::Depth32F,
                                       RenderBackend::Vulkan));
      script.keepAlive(onTopLease);
      script.raster("geometry_on_top_blend", {}, [&]() {
        m_rendererBase.setActiveSurfaceWithLoadStore(*onTopLease,
                                                     LoadOp::Clear,
                                                     StoreOp::Store,
                                                     LoadOp::Clear,
                                                     StoreOp::Store);
        for (auto* filter : onTopOpaqueFilters) {
          if (!filter) {
            continue;
          }
          recordFilterBatchesToSurface(filter, *onTopLease, targetSize, [&]() {
            filter->renderOpaque(eye);
          });
        }
        for (auto* filter : onTopTransparentFilters) {
          if (!filter) {
            continue;
          }
          recordFilterBatchesToSurface(filter, *onTopLease, targetSize, [&]() {
            filter->renderTransparent(eye);
          });
        }
      });

      // Overlay onto scene output using "first on top" blending
      AttachmentHandle overlayC{};
      overlayC.backend = RenderBackend::Vulkan;
      overlayC.index = 0;
      overlayC.id = reinterpret_cast<uint64_t>(onTopLease->colorAttachment(0));
      AttachmentHandle overlayD{};
      overlayD.backend = RenderBackend::Vulkan;
      overlayD.index = 0;
      overlayD.id = reinterpret_cast<uint64_t>(onTopLease->depthAttachmentTexture());

      AttachmentHandle outC{};
      outC.backend = RenderBackend::Vulkan;
      outC.index = 0;
      outC.id = reinterpret_cast<uint64_t>(sceneOutLease->colorAttachment(0));
      AttachmentHandle outD{};
      outD.backend = RenderBackend::Vulkan;
      outD.index = 0;
      outD.id = reinterpret_cast<uint64_t>(sceneOutLease->depthAttachmentTexture());

      // Overlay on-top content over the scene. In GL, blending is enabled
      // only when the background is drawn to the destination first. Mirror
      // that: enable fixed-function blending iff background is shown.
      script.raster("on_top_overlay_blend", {}, [&]() {
        m_rendererBase.setActiveSurfaceWithLoadStore(*sceneOutLease,
                                                     LoadOp::Load,
                                                     StoreOp::Store,
                                                     LoadOp::Clear,
                                                     StoreOp::Store);

        m_firstOnTopBlendRenderer.setEnableFixedBlend(m_showBackground.get());
        m_firstOnTopBlendRenderer.setSourceAttachments0(overlayC, overlayD);
        m_firstOnTopBlendRenderer.setSourceAttachments1(outC, outD);
        m_rendererBase.renderVulkan(eye, m_firstOnTopBlendRenderer);
        m_firstOnTopBlendRenderer.setEnableFixedBlend(false);
      });

      // Apply glow for on-top filters
      applyGlowOverlay(onTopTransparentFilters);
      applyGlowOverlay(onTopOpaqueFilters);
    } else {
      // OIT path: split opaque/transparent, run OIT for transparent with opaque depth, then overlay
      std::shared_ptr<Z3DScratchResourcePool::RenderTargetLease> onTopOpaqueLease;
      if (!onTopOpaqueFilters.empty()) {
        onTopOpaqueLease = std::make_shared<Z3DScratchResourcePool::RenderTargetLease>(
          pool.acquireTempRenderTarget2D(targetSize,
                                         ScratchFormat::RGBA16,
                                         ScratchFormat::Depth32F,
                                         RenderBackend::Vulkan));
        script.keepAlive(onTopOpaqueLease);
        script.raster("geometry_on_top_opaque", {}, [&]() {
          m_rendererBase.setActiveSurfaceWithLoadStore(*onTopOpaqueLease,
                                                       LoadOp::Clear,
                                                       StoreOp::Store,
                                                       LoadOp::Clear,
                                                       StoreOp::Store);
          for (auto* filter : onTopOpaqueFilters) {
            if (!filter) {
              continue;
            }
            recordFilterBatchesToSurface(filter, *onTopOpaqueLease, targetSize, [&]() {
              filter->renderOpaque(eye);
            });
          }
        });
      }

      auto handlesFromLease = [](const Z3DScratchResourcePool::RenderTargetLease& lease) {
        AttachmentHandle c{};
        c.backend = RenderBackend::Vulkan;
        c.index = 0;
        c.id = reinterpret_cast<uint64_t>(lease.colorAttachment(0));
        AttachmentHandle d{};
        d.backend = RenderBackend::Vulkan;
        d.index = 0;
        d.id = reinterpret_cast<uint64_t>(lease.depthAttachmentTexture());
        return std::make_pair(c, d);
      };

      // OIT for on-top transparent filters into a temporary lease
      std::shared_ptr<Z3DScratchResourcePool::RenderTargetLease> onTopTransLease;
      const auto transparencyModeOnTop = m_rendererBase.sceneState().transparency;
      const bool onTopHasTrans = !onTopTransparentFilters.empty();
      const bool onTopHasOpaque = !onTopOpaqueFilters.empty();
      if (onTopHasTrans) {
        onTopTransLease = std::make_shared<Z3DScratchResourcePool::RenderTargetLease>(
          pool.acquireTempRenderTarget2D(targetSize,
                                         ScratchFormat::RGBA16,
                                         ScratchFormat::Depth32F,
                                         RenderBackend::Vulkan));
        script.keepAlive(onTopTransLease);
        AttachmentHandle depthHandle{};
        if (onTopHasOpaque) {
          CHECK(onTopOpaqueLease) << "onTopHasOpaque without an opaque lease";
          depthHandle.backend = RenderBackend::Vulkan;
          depthHandle.index = 0;
          depthHandle.id = reinterpret_cast<uint64_t>(onTopOpaqueLease->depthAttachmentTexture());
        }
        // Restrict image layers to stay-on-top volumes
        std::vector<Z3DCompositorImageLayer> onTopImageLayers;
        for (auto* vf : m_volumeFilters) {
          if (!vf || !vf->isStayOnTop() || !vf->isReady(eye) || !vf->hasTransparent(eye)) {
            continue;
          }
          Z3DCompositorImageLayer layer;
          const auto& lease = vf->transparentLease(eye);
          if (!lease || lease.backend != RenderBackend::Vulkan) {
            continue;
          }
          auto surface = m_rendererBase.describeSurface(lease);
          if (surface.colorAttachments.empty()) {
            continue;
          }
          layer.colorAttachment = surface.colorAttachments[0];
          if (surface.depthAttachment) {
            layer.depthAttachment = *surface.depthAttachment;
          }
          onTopImageLayers.push_back(layer);
        }

        switch (transparencyModeOnTop) {
          case TransparencyMode::DualDepthPeeling:
            renderTransparentDDPVulkan(onTopTransparentFilters,
                                       *onTopTransLease,
                                       eye,
                                       depthHandle,
                                       onTopImageLayers,
                                       /*clearResolveTarget=*/true,
                                       script);
            break;
          case TransparencyMode::PerPixelFragmentList:
            renderTransparentPPLLVulkan(onTopTransparentFilters,
                                        *onTopTransLease,
                                        eye,
                                        depthHandle,
                                        onTopImageLayers,
                                        /*clearResolveTarget=*/true,
                                        script);
            break;
          case TransparencyMode::WeightedAverage:
            renderTransparentWAVulkan(onTopTransparentFilters,
                                      *onTopTransLease,
                                      eye,
                                      depthHandle,
                                      onTopImageLayers,
                                      /*clearResolveTarget=*/true,
                                      script);
            break;
          case TransparencyMode::WeightedBlended:
            renderTransparentWBVulkan(onTopTransparentFilters,
                                      *onTopTransLease,
                                      eye,
                                      depthHandle,
                                      onTopImageLayers,
                                      /*clearResolveTarget=*/true,
                                      script);
            break;
          default:
            break;
        }
      }

      // Determine the overlay handles (opaque-only, trans-only, or merged both)
      std::pair<AttachmentHandle, AttachmentHandle> overlayHandles{};
      if (onTopHasOpaque && onTopHasTrans) {
        CHECK(onTopOpaqueLease) << "onTopHasOpaque without an opaque lease";
        CHECK(onTopTransLease) << "onTopHasTrans without a transparent lease";
        auto mergeLease = std::make_shared<Z3DScratchResourcePool::RenderTargetLease>(
          pool.acquireTempRenderTarget2D(targetSize,
                                         ScratchFormat::RGBA16,
                                         ScratchFormat::Depth32F,
                                         RenderBackend::Vulkan));
        script.keepAlive(mergeLease);
        const auto opaqueHandles = handlesFromLease(*onTopOpaqueLease);
        const auto transHandles = handlesFromLease(*onTopTransLease);

        script.raster("on_top_merge", {}, [&]() {
          // Clear merge target
          m_rendererBase.setActiveSurfaceWithLoadStore(*mergeLease,
                                                       LoadOp::Clear,
                                                       StoreOp::Store,
                                                       LoadOp::Clear,
                                                       StoreOp::Store);
          m_alphaBlendRenderer.setEnableFixedBlend(false);
          // Depth-tested alpha blend to pick per-pixel front-most between opaque and transparent
          m_alphaBlendRenderer.setSourceAttachments0(opaqueHandles.first, opaqueHandles.second);
          m_alphaBlendRenderer.setSourceAttachments1(transHandles.first, transHandles.second);
          m_rendererBase.renderVulkan(eye, m_alphaBlendRenderer);
        });

        overlayHandles = handlesFromLease(*mergeLease);
      } else if (onTopHasTrans) {
        CHECK(onTopTransLease) << "onTopHasTrans without a transparent lease";
        overlayHandles = handlesFromLease(*onTopTransLease);
      } else if (onTopHasOpaque) {
        CHECK(onTopOpaqueLease) << "onTopHasOpaque without an opaque lease";
        overlayHandles = handlesFromLease(*onTopOpaqueLease);
      }

      // Blend overlayHandles over the scene temp into the final output (no SS) or a supersampled composite (SS)
      if (overlayHandles.first.valid()) {
        // Scene inputs come from sceneOutLease (temp); write into outLease (final) when not supersampling,
        // otherwise write into a supersampled composite lease and keep sceneOutLease pointing to it.
        AttachmentHandle sceneC{};
        sceneC.backend = RenderBackend::Vulkan;
        sceneC.index = 0;
        sceneC.id = reinterpret_cast<uint64_t>(sceneOutLease->colorAttachment(0));
        AttachmentHandle sceneD{};
        sceneD.backend = RenderBackend::Vulkan;
        sceneD.index = 0;
        sceneD.id = reinterpret_cast<uint64_t>(sceneOutLease->depthAttachmentTexture());

        if (supersample2x2) {
          // Allocate composite surface once at supersampled size and render there
          if (!sceneCompositeLease) {
            const glm::uvec2 ssSize = sceneOutLease->descriptor.size;
            sceneCompositeLease = pool.acquireTempRenderTarget2D(ssSize,
                                                                 ScratchFormat::RGBA16,
                                                                 ScratchFormat::Depth32F,
                                                                 RenderBackend::Vulkan);
          }
        } else {
          // Non-supersampled path writes directly to the final output lease.
        }

        // First-on-top blend over scene temp into final (no SS) or composite (SS).
        // Destination may or may not already include background; we do not need
        // an extra blend here because the shader composes overlay+scene.
        script.raster("on_top_overlay", {}, [&]() {
          if (supersample2x2) {
            m_rendererBase.setActiveSurfaceWithLoadStore(sceneCompositeLease,
                                                         LoadOp::Clear,
                                                         StoreOp::Store,
                                                         LoadOp::Clear,
                                                         StoreOp::Store);
          } else {
            // Clear color/depth on the final surface; we compute the final pixels in the compositor
            m_rendererBase.setActiveSurfaceWithLoadStore(*outLease,
                                                         LoadOp::Clear,
                                                         StoreOp::Store,
                                                         LoadOp::Clear,
                                                         StoreOp::Store);
          }

          m_firstOnTopBlendRenderer.setEnableFixedBlend(false);
          // First-on-top blending: overlay (0) over scene (1)
          m_firstOnTopBlendRenderer.setSourceAttachments0(overlayHandles.first, overlayHandles.second);
          m_firstOnTopBlendRenderer.setSourceAttachments1(sceneC, sceneD);
          m_rendererBase.renderVulkan(eye, m_firstOnTopBlendRenderer);
        });

        if (supersample2x2) {
          // Promote composite result as the new scene base for subsequent overlays/passes
          sceneOutLease = &sceneCompositeLease;
        }
      }

      // Apply glow for on-top filters
      applyGlowOverlay(onTopTransparentFilters);
      applyGlowOverlay(onTopOpaqueFilters);
    }
  }

  // Supersample resolve must happen before pixel-sized overlays (handles,
  // selection boxes) so their "pixel" sizing is based on the output viewport
  // (GL parity).
  Z3DScratchResourcePool::RenderTargetLease* resolvedSceneLeaseForHandles = nullptr;
  if (supersample2x2) {
    // Restore viewport to the output size for the resolve + overlay passes.
    m_rendererBase.frameState().updateViewportData(prevViewport);
    Z3DScratchResourcePool::RenderTargetLease* resolveDst = nullptr;
    if (needResolvedLeaseForHandles) {
      resolveDst = &resolvedLease;
      resolvedSceneLeaseForHandles = &resolvedLease;
    } else {
      resolveDst = outLease;
    }
    CHECK(resolveDst != nullptr) << "Vulkan supersample resolve missing destination lease";

    script.raster("supersample_resolve", {}, [&]() {
      m_rendererBase.setActiveSurfaceWithLoadStore(*resolveDst,
                                                   LoadOp::Clear,
                                                   StoreOp::Store,
                                                   LoadOp::Clear,
                                                   StoreOp::Store);
      // If we already composited into a supersampled composite lease, prefer that; otherwise use sceneOutLease.
      Z3DScratchResourcePool::RenderTargetLease* copySrc = sceneOutLease;
      AttachmentHandle srcColor{};
      srcColor.backend = RenderBackend::Vulkan;
      srcColor.index = 0;
      srcColor.id = reinterpret_cast<uint64_t>(copySrc->colorAttachment(0));
      AttachmentHandle srcDepth{};
      srcDepth.backend = RenderBackend::Vulkan;
      srcDepth.index = 0;
      srcDepth.id = reinterpret_cast<uint64_t>(copySrc->depthAttachmentTexture());
      // Internal resolve: no Y-flip
      const bool resolveDepth = needResolvedLeaseForHandles || !selectedFilters.empty();
      m_textureCopyRenderer.setCopyDepth(resolveDepth);
      auto copyDepthGuard = folly::makeGuard([&]() {
        m_textureCopyRenderer.setCopyDepth(true);
      });
      m_textureCopyRenderer.setFlipY(false);
      m_textureCopyRenderer.setSourceAttachments(srcColor, srcDepth);
      m_rendererBase.renderVulkan(eye, m_textureCopyRenderer);
    });
  }

  if (haveHandles) {
    // For supersampling, handles must render at output size; otherwise they
    // appear half-size after the downsample.
    const bool renderAtOutputSize = supersample2x2;
    const glm::uvec2 targetSize = renderAtOutputSize ? outLease->descriptor.size : sceneOutLease->descriptor.size;
    auto handleLease = std::make_shared<Z3DScratchResourcePool::RenderTargetLease>(
      pool.acquireTempRenderTarget2D(targetSize,
                                     ScratchFormat::RGBA16,
                                     ScratchFormat::Depth32F,
                                     RenderBackend::Vulkan));
    script.keepAlive(handleLease);
    CHECK(handleLease && *handleLease && handleLease->backend == RenderBackend::Vulkan)
      << "Failed to acquire Vulkan handle overlay render target";

    script.raster("handles_overlay", {}, [&]() {
      m_rendererBase.setActiveSurfaceWithLoadStore(*handleLease,
                                                   LoadOp::Clear,
                                                   StoreOp::Store,
                                                   LoadOp::Clear,
                                                   StoreOp::Store);
      for (auto* filter : showHandleFilters) {
        if (!filter) {
          continue;
        }
        recordFilterBatchesToSurface(filter, *handleLease, targetSize, [&]() {
          filter->renderHandle(eye);
        });
      }
    });

    AttachmentHandle handleColor{};
    handleColor.backend = RenderBackend::Vulkan;
    handleColor.index = 0;
    handleColor.id = reinterpret_cast<uint64_t>(handleLease->colorAttachment(0));
    AttachmentHandle handleDepth{};
    handleDepth.backend = RenderBackend::Vulkan;
    handleDepth.index = 0;
    handleDepth.id = reinterpret_cast<uint64_t>(handleLease->depthAttachmentTexture());
    CHECK(handleColor.id != 0 && handleDepth.id != 0) << "Handles overlay lease missing attachments";

    // Sample the rendered scene and composite handles over it into the final output.
    Z3DScratchResourcePool::RenderTargetLease* sceneForHandles = sceneOutLease;
    if (supersample2x2) {
      CHECK(resolvedSceneLeaseForHandles != nullptr)
        << "Supersampled handle overlay requires a resolved output-sized scene lease";
      sceneForHandles = resolvedSceneLeaseForHandles;
    }

    AttachmentHandle sceneColor{};
    sceneColor.backend = RenderBackend::Vulkan;
    sceneColor.index = 0;
    sceneColor.id = reinterpret_cast<uint64_t>(sceneForHandles->colorAttachment(0));
    AttachmentHandle sceneDepth{};
    sceneDepth.backend = RenderBackend::Vulkan;
    sceneDepth.index = 0;
    sceneDepth.id = reinterpret_cast<uint64_t>(sceneForHandles->depthAttachmentTexture());
    CHECK(sceneColor.id != 0 && sceneDepth.id != 0) << "Scene lease missing attachments for handles overlay";

    // Handle overlay composes handle over scene in-shader; do not enable
    // fixed-function blending to avoid a second blend.
    script.raster("handles_composite", {}, [&]() {
      // Non-SS can preserve any prior final content by loading outLease first
      // (even though the full-screen compositor shader overwrites the result).
      const bool havePriorFinalContent = (!supersample2x2 && (haveOnTop || imagesCompositedToFinal));
      m_rendererBase.setActiveSurfaceWithLoadStore(*outLease,
                                                   havePriorFinalContent ? LoadOp::Load : LoadOp::Clear,
                                                   StoreOp::Store,
                                                   LoadOp::Clear,
                                                   StoreOp::Store);

      m_firstOnTopBlendRenderer.setEnableFixedBlend(false);
      m_firstOnTopBlendRenderer.setSourceAttachments0(handleColor, handleDepth);
      m_firstOnTopBlendRenderer.setSourceAttachments1(sceneColor, sceneDepth);
      m_rendererBase.renderVulkan(eye, m_firstOnTopBlendRenderer);
    });
  }

  if (!selectedFilters.empty()) {
    // GL parity: selection boxes are output-sized overlays and should not be
    // rendered in the supersampled scene.
    Z3DScratchResourcePool::RenderTargetLease* selectionLease = nullptr;
    if (supersample2x2) {
      selectionLease = outLease;
    } else {
      // After overlay/handle composition, draw selection boxes onto the final output
      // when either overlay path was engaged; otherwise keep using the current scene lease.
      const bool compositedToFinal = (!supersample2x2 && (haveHandles || haveOnTop || imagesCompositedToFinal));
      selectionLease = compositedToFinal ? outLease : sceneOutLease;
    }
    CHECK(selectionLease != nullptr);
    const glm::uvec2 targetSize = selectionLease->descriptor.size;
    script.raster("selection_boxes", {}, [&]() {
      m_rendererBase.setActiveSurfaceWithLoadStore(*selectionLease,
                                                   LoadOp::Load,
                                                   StoreOp::Store,
                                                   LoadOp::Load,
                                                   StoreOp::Store);
      for (auto* filter : selectedFilters) {
        if (!filter) {
          continue;
        }
        recordFilterBatchesToSurface(filter, *selectionLease, targetSize, [&]() {
          filter->renderSelectionBox(eye);
        });
      }
    });
  }

  if (m_showAxis.get()) {
    // Axis should draw on the same surface as selection boxes.
    if (supersample2x2) {
      renderAxisVulkan(eye, *outLease, script);
    } else {
      const bool compositedToFinal = (!supersample2x2 && (haveHandles || haveOnTop || imagesCompositedToFinal));
      renderAxisVulkan(eye, *(compositedToFinal ? outLease : sceneOutLease), script);
    }
  }

  // Vulkan picking (render to RGBA8+Depth24 Vulkan scratch image)
  {
    // Match GL behavior: picking targets are always output-sized and must not
    // depend on the supersampling mode. When supersampling is enabled, the
    // compositor viewport is temporarily set to the supersampled size; Vulkan
    // render passes use frameState().viewport as the render area, so restore
    // the output-sized viewport for picking.
    const glm::uvec2 pickSize = outLease->descriptor.size;
    const glm::uvec4 pickViewport(0u, 0u, pickSize.x, pickSize.y);
    m_rendererBase.frameState().updateViewportData(pickViewport);

    ensurePickingTargetVulkan(pickSize);

    if (gFilters.empty() && !showHandleFilters.empty()) {
      vlogVulkanLease("picking_handles", m_pickingTargetLease);

      script.raster("picking_handles", {}, [&]() {
        m_rendererBase.setActiveSurfaceWithLoadStore(m_pickingTargetLease,
                                                     LoadOp::Clear,
                                                     StoreOp::Store,
                                                     LoadOp::Clear,
                                                     StoreOp::Store);
        const auto surfaceCopy = m_rendererBase.frameState().activeSurface;
        for (auto* f : showHandleFilters) {
          recordHandlePickingFilterBatchesToSurfaceUnifiedWithViewport(m_rendererBase,
                                                                       f,
                                                                       surfaceCopy,
                                                                       pickViewport,
                                                                       eye);
        }
      });

    } else if (showHandleFilters.empty() && !gFilters.empty()) {
      vlogVulkanLease("picking_geometry", m_pickingTargetLease);

      script.raster("picking_geometry", {}, [&]() {
        m_rendererBase.setActiveSurfaceWithLoadStore(m_pickingTargetLease,
                                                     LoadOp::Clear,
                                                     StoreOp::Store,
                                                     LoadOp::Clear,
                                                     StoreOp::Store);
        const auto surfaceCopy = m_rendererBase.frameState().activeSurface;
        for (auto* gf : gFilters) {
          if (gf && gf->isReady(eye)) {
            recordGeometryPickingFilterBatchesToSurfaceUnifiedWithViewport(m_rendererBase,
                                                                           gf,
                                                                           surfaceCopy,
                                                                           pickViewport,
                                                                           eye);
          }
        }
      });

    } else if (!gFilters.empty() && !showHandleFilters.empty()) {
      auto leaseHandles = std::make_shared<Z3DScratchResourcePool::RenderTargetLease>(
        pool.acquireTempRenderTarget2D(pickSize,
                                       ScratchFormat::RGBA16,
                                       ScratchFormat::Depth32F,
                                       RenderBackend::Vulkan));
      auto leaseGeoms = std::make_shared<Z3DScratchResourcePool::RenderTargetLease>(
        pool.acquireTempRenderTarget2D(pickSize,
                                       ScratchFormat::RGBA16,
                                       ScratchFormat::Depth32F,
                                       RenderBackend::Vulkan));
      script.keepAlive(leaseHandles);
      script.keepAlive(leaseGeoms);

      // Record handle picking

      vlogVulkanLease("picking_handles_temp", *leaseHandles);

      const auto segHandles = script.raster("picking_handles_temp", {}, [&]() {
        m_rendererBase.setActiveSurfaceWithLoadStore(*leaseHandles,
                                                     LoadOp::Clear,
                                                     StoreOp::Store,
                                                     LoadOp::Clear,
                                                     StoreOp::Store);
        const auto surfaceCopy = m_rendererBase.frameState().activeSurface;
        for (auto* f : showHandleFilters) {
          recordHandlePickingFilterBatchesToSurfaceUnifiedWithViewport(m_rendererBase,
                                                                       f,
                                                                       surfaceCopy,
                                                                       pickViewport,
                                                                       eye);
        }
      });

      // Record geometry picking
      vlogVulkanLease("picking_geometry_temp", *leaseGeoms);

      const auto segGeoms = script.raster("picking_geometry_temp", {}, [&]() {
        m_rendererBase.setActiveSurfaceWithLoadStore(*leaseGeoms,
                                                     LoadOp::Clear,
                                                     StoreOp::Store,
                                                     LoadOp::Clear,
                                                     StoreOp::Store);
        const auto surfaceCopy = m_rendererBase.frameState().activeSurface;
        for (auto* gf : gFilters) {
          if (gf && gf->isReady(eye)) {
            recordGeometryPickingFilterBatchesToSurfaceUnifiedWithViewport(m_rendererBase,
                                                                           gf,
                                                                           surfaceCopy,
                                                                           pickViewport,
                                                                           eye);
          }
        }
      });

      // Composite into picking target using first-on-top blend
      AttachmentHandle handlesColor{};
      handlesColor.backend = RenderBackend::Vulkan;
      handlesColor.index = 0;
      handlesColor.id = reinterpret_cast<uint64_t>(leaseHandles->colorAttachment(0));
      AttachmentHandle handlesDepth{};
      handlesDepth.backend = RenderBackend::Vulkan;
      handlesDepth.index = 0;
      handlesDepth.id = reinterpret_cast<uint64_t>(leaseHandles->depthAttachmentTexture());
      AttachmentHandle geomsColor{};
      geomsColor.backend = RenderBackend::Vulkan;
      geomsColor.index = 0;
      geomsColor.id = reinterpret_cast<uint64_t>(leaseGeoms->colorAttachment(0));
      AttachmentHandle geomsDepth{};
      geomsDepth.backend = RenderBackend::Vulkan;
      geomsDepth.index = 0;
      geomsDepth.id = reinterpret_cast<uint64_t>(leaseGeoms->depthAttachmentTexture());

      m_firstOnTopRenderer.setSourceAttachments0(handlesColor, handlesDepth);
      m_firstOnTopRenderer.setSourceAttachments1(geomsColor, geomsDepth);

      script.raster("picking_composite", {segHandles, segGeoms}, [&]() {
        m_rendererBase.setActiveSurfaceWithLoadStore(m_pickingTargetLease,
                                                     LoadOp::Clear,
                                                     StoreOp::Store,
                                                     LoadOp::Clear,
                                                     StoreOp::Store);
        m_rendererBase.renderVulkan(eye, m_firstOnTopRenderer);
      });
    }

    // Update global picking manager with current Vulkan picking attachments for interactive queries
    if (m_pickingTargetLease && m_pickingTargetLease.backend == RenderBackend::Vulkan) {
      auto* colorTex = m_pickingTargetLease.colorAttachment(0);
      auto* depthTex = m_pickingTargetLease.depthAttachmentTexture();
      if (colorTex && depthTex) {
        m_globalParameters.pickingManager.setPickingTarget(*colorTex, *depthTex, m_pickingTargetLease.descriptor.size);
      } else {
        m_globalParameters.pickingManager.resetRenderTarget();
      }
    }
  }

  // Per-pass recording ends any frame it began unless we kept a tail frame open.

  // Finalize: enqueue end-of-frame readback to staging and perform CPU copy after fence.
  {
    ZVulkanTexture* finalColor = outLease->colorAttachment(0);
    auto* scratchPool = &pool;

    auto installFinalColorReadback = [this, scratchPool](Z3DLocalColorBuffer* localPtr,
                                                         Z3DScratchResourcePool::RenderTargetLease* targetPtr,
                                                         Z3DEye eyeCopy,
                                                         uint64_t perfFrameToken,
                                                         const void* mapped,
                                                         const glm::uvec2& size,
                                                         std::function<void()> releaseSlot,
                                                         bool noCopy) {
      CHECK(localPtr != nullptr) << "VK final readback install requires a valid local color buffer";
      CHECK(targetPtr != nullptr) << "VK final readback install requires a valid output target lease";

      const char* suffix = noCopy ? " (no copy)" : "";
      VLOG(1) << fmt::format("VK final readback ready{} frame#{} mapped={} size={}x{} eye={}",
                             suffix,
                             perfFrameToken,
                             mapped,
                             size.x,
                             size.y,
                             static_cast<int>(eyeCopy));

      const size_t eyeIndex = static_cast<size_t>(eyeCopy);
      CHECK_LT(eyeIndex, m_lastPublishedPerfFrameToken.size())
        << "VK final readback eye index out of range: eye=" << static_cast<int>(eyeCopy);

      bool dropStale = false;
      uint64_t lastPublishedToken = 0;
      bool emitFinished = false;
      const char* finishedLabel = nullptr;
      Z3DLocalColorBuffer* readyBufferForLog = nullptr;

      {
        const std::scoped_lock lock(m_globalParameters.targetSwitchMutex);

        lastPublishedToken = m_lastPublishedPerfFrameToken[eyeIndex];
        if (perfFrameToken != 0 && lastPublishedToken != 0 && perfFrameToken < lastPublishedToken) {
          dropStale = true;
        } else if (perfFrameToken != 0) {
          m_lastPublishedPerfFrameToken[eyeIndex] = perfFrameToken;
        }

        if (!dropStale) {
          // Replace the external mapping for the destination local buffer. This must
          // happen under targetSwitchMutex so the UI never observes a half-updated
          // ready buffer.
          if (localPtr->externalRelease) {
            localPtr->externalRelease();
            localPtr->externalRelease = {};
          }
          localPtr->external = static_cast<const uint8_t*>(mapped);
          localPtr->externalStride = static_cast<size_t>(size.x) * 4u;
          localPtr->externalRelease = std::move(releaseSlot);
          localPtr->width = size.x;
          localPtr->height = size.y;

          if (eyeCopy == MonoEye) {
            CHECK(localPtr == &m_localColorBuffer1 || localPtr == &m_localColorBuffer2)
              << "VK mono install targeted unexpected local buffer pointer";
            CHECK(targetPtr == &m_outRenderTarget1 || targetPtr == &m_outRenderTarget2)
              << "VK mono install targeted unexpected output lease pointer";

            // Publish the buffer/target that actually received this completed frame.
            // With >1 frames in flight, multiple completion hooks can be queued
            // before the UI consumes the previous signal. Using unconditional
            // swaps here makes the ready pointers depend on the *parity* of
            // completions (and can bounce between old/new frames). Publish the
            // completed destinations directly to make presentation monotonic.
            m_monoReadyLocalBuffer = localPtr;
            m_monoReadyTarget = targetPtr;
            m_monoCurrentLocalBuffer = (localPtr == &m_localColorBuffer1) ? &m_localColorBuffer2 : &m_localColorBuffer1;
            m_monoCurrentTarget = (targetPtr == &m_outRenderTarget1) ? &m_outRenderTarget2 : &m_outRenderTarget1;
            emitFinished = true;
            finishedLabel = noCopy ? "(mono, no copy)" : "(mono)";
            readyBufferForLog = m_monoReadyLocalBuffer;
          } else if (eyeCopy == LeftEye) {
            CHECK(localPtr == &m_leftLocalColorBuffer1 || localPtr == &m_leftLocalColorBuffer2)
              << "VK left-eye install targeted unexpected local buffer pointer";
            CHECK(targetPtr == &m_leftEyeOutRenderTarget1 || targetPtr == &m_leftEyeOutRenderTarget2)
              << "VK left-eye install targeted unexpected output lease pointer";

            m_leftReadyLocalBuffer = localPtr;
            m_leftReadyTarget = targetPtr;
            m_leftCurrentLocalBuffer =
              (localPtr == &m_leftLocalColorBuffer1) ? &m_leftLocalColorBuffer2 : &m_leftLocalColorBuffer1;
            m_leftCurrentTarget =
              (targetPtr == &m_leftEyeOutRenderTarget1) ? &m_leftEyeOutRenderTarget2 : &m_leftEyeOutRenderTarget1;
          } else {
            CHECK(localPtr == &m_localColorBuffer1 || localPtr == &m_localColorBuffer2)
              << "VK right-eye install targeted unexpected local buffer pointer";
            CHECK(targetPtr == &m_outRenderTarget1 || targetPtr == &m_outRenderTarget2)
              << "VK right-eye install targeted unexpected output lease pointer";

            m_rightReadyLocalBuffer = localPtr;
            m_rightReadyTarget = targetPtr;
            m_rightCurrentLocalBuffer =
              (localPtr == &m_localColorBuffer1) ? &m_localColorBuffer2 : &m_localColorBuffer1;
            m_rightCurrentTarget = (targetPtr == &m_outRenderTarget1) ? &m_outRenderTarget2 : &m_outRenderTarget1;
            emitFinished = true;
            finishedLabel = noCopy ? "(right, no copy)" : "(right)";
          }
        }
      }

      if (dropStale) {
        VLOG(1) << fmt::format("VK final readback drop stale frame#{} < last#{} eye={}",
                               perfFrameToken,
                               lastPublishedToken,
                               static_cast<int>(eyeCopy));
        if (releaseSlot) {
          releaseSlot();
        }
        return;
      }

      // Mono/right-eye publish complete. Emit update signal (left eye waits for right).
      if (emitFinished) {
        m_globalParameters.hasNewRendering = true;
      }

      if (eyeCopy == MonoEye) {
        static uint64_t s_lastCreate = 0, s_lastChange = 0, s_lastReuse = 0;
        const uint64_t curCreate = scratchPool->creationCounter();
        const uint64_t curChange = scratchPool->changeCounter();
        const uint64_t curReuse = scratchPool->reuseStatsCounter();
        if (curCreate != s_lastCreate || curChange != s_lastChange) {
          VLOG(1) << scratchPool->describeMemoryUsage(true);
          s_lastCreate = curCreate;
          s_lastChange = curChange;
        }
        if (curReuse != s_lastReuse) {
          VLOG(1) << scratchPool->describeReuseStats(true);
          s_lastReuse = curReuse;
        }
        CHECK(finishedLabel != nullptr) << "VK mono publish expected a finished label";
        VLOG(1) << fmt::format("VK renderingFinished {} readyBuffer={}", finishedLabel, (void*)readyBufferForLog);
        Q_EMIT renderingFinished();
      } else if (eyeCopy == RightEye) {
        CHECK(finishedLabel != nullptr) << "VK right-eye publish expected a finished label";
        VLOG(1) << fmt::format("VK renderingFinished {}", finishedLabel);
        Q_EMIT renderingFinished();
      }
    };

    auto enqueueFinalReadbackInActiveFrame = [installFinalColorReadback](
                                               Z3DRendererVulkanBackend& backend,
                                               ZVulkanTexture& tex,
                                               Z3DLocalColorBuffer* localPtr,
                                               Z3DScratchResourcePool::RenderTargetLease* targetPtr,
                                               Z3DEye eyeCopy,
                                               std::string_view ticketLabel,
                                               std::string_view consumeLabel,
                                               bool noCopy) {
      CHECK(localPtr != nullptr) << "VK enqueue final readback requires local color buffer";
      CHECK(targetPtr != nullptr) << "VK enqueue final readback requires output target lease";
      const char* suffix = noCopy ? " (no copy)" : "";
      VLOG(1) << fmt::format("VK enqueue final readback{} tex=0x{:x} size={}x{} eye={}",
                             suffix,
                             reinterpret_cast<uint64_t>(&tex),
                             tex.width(),
                             tex.height(),
                             static_cast<int>(eyeCopy));
      // Measure end-to-end latency from the engine's perf-frame start (Network
      // wrapper "Since Start") until the result is published/host-ready and
      // renderingFinished is emitted. This matches OpenGL's synchronous UX
      // timing better than measuring only enqueue→ready latency in Vulkan.
      auto perfFrameStartTs = Z3DRenderGlobalState::instance().currentPerfFrameStartTime();
      if (perfFrameStartTs.time_since_epoch().count() == 0) {
        // Perf timing should never be missing, but do not crash on a logging
        // metric. Fall back to "now" so the value is still bounded.
        perfFrameStartTs = std::chrono::steady_clock::now();
      }
      const uint64_t perfFrameToken = Z3DRenderGlobalState::instance().currentPerfFrameToken();
      auto ticket = backend.requestEndOfFrameColorReadbackTicket(tex, eyeCopy, ticketLabel);
      backend.registerAfterCurrentFrameCompletionHook(
        currentRenderThreadExecutorKeepAlive(consumeLabel),
        [installFinalColorReadback,
         localPtr,
         targetPtr,
         eyeCopy,
         perfFrameStartTs,
         perfFrameToken,
         ticket = std::move(ticket),
         noCopy](Z3DRendererVulkanBackend& backend) mutable -> folly::coro::Task<void> {
          co_await Z3DRendererVulkanBackend::waitActiveSubmissionFence(ticket.fence);
          installFinalColorReadback(localPtr,
                                    targetPtr,
                                    eyeCopy,
                                    perfFrameToken,
                                    ticket.mapped,
                                    ticket.size,
                                    std::move(ticket.releaseSlot),
                                    noCopy);
          const double allMs =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - perfFrameStartTs).count();
          backend.recordAllMsForCompletionSafePoint(allMs);
          co_return;
        },
        consumeLabel);
    };

    // Request readback while a Vulkan frame is active so backend can insert the copy before endRender.
    // If final color is not RGBA8, first render a copy to an RGBA8 scratch surface, then enqueue the readback inside
    // the same frame.
    if (finalColor && finalColor->format() != vk::Format::eR8G8B8A8Unorm) {
      auto rgba8Lease = std::make_shared<Z3DScratchResourcePool::RenderTargetLease>(
        pool.acquireTempRenderTarget2D(m_outputSize,
                                       ScratchFormat::RGBA8,
                                       ScratchFormat::Depth32F,
                                       RenderBackend::Vulkan));
      script.keepAlive(rgba8Lease);
      if (*rgba8Lease && rgba8Lease->backend == RenderBackend::Vulkan) {
        const auto segCopy = script.raster("final_rgba8_copy", {}, [&]() {
          m_rendererBase.setActiveSurfaceWithLoadStore(*rgba8Lease,
                                                       LoadOp::Clear,
                                                       StoreOp::Store,
                                                       LoadOp::Clear,
                                                       StoreOp::Store);
          // GPU copy to RGBA8 first. If we composed overlays directly into the final surface
          // (non-SS path), read from outLease; otherwise read from the current scene lease.
          //
          // Supersampling note: in SS 2x2 we resolve into `outLease` before drawing
          // pixel-sized overlays (handles/selection). Those overlays live on
          // `outLease`, so the final copy must read from `outLease` (not from the
          // supersampled sceneOutLease).
          Z3DScratchResourcePool::RenderTargetLease* copySrc = nullptr;
          if (supersample2x2) {
            copySrc = outLease;
          } else {
            const bool compositedToFinal = (!supersample2x2 && (haveHandles || haveOnTop || imagesCompositedToFinal));
            copySrc = compositedToFinal ? outLease : sceneOutLease;
          }
          CHECK(copySrc != nullptr) << "final_rgba8_copy: missing copy source";

          AttachmentHandle srcColor{};
          srcColor.backend = RenderBackend::Vulkan;
          srcColor.index = 0;
          srcColor.id = reinterpret_cast<uint64_t>(copySrc->colorAttachment(0));
          AttachmentHandle srcDepth{};
          srcDepth.backend = RenderBackend::Vulkan;
          srcDepth.index = 0;
          srcDepth.id = reinterpret_cast<uint64_t>(copySrc->depthAttachmentTexture());
          CHECK(srcColor.id != 0) << "final_rgba8_copy: missing source color attachment";
          VLOG(1) << fmt::format("VK final_rgba8_copy srcColor=0x{:x} srcDepth=0x{:x}",
                                 static_cast<uint64_t>(srcColor.id),
                                 static_cast<uint64_t>(srcDepth.id));
          m_textureCopyRenderer.setCopyDepth(false);
          auto copyDepthGuard = folly::makeGuard([&]() {
            m_textureCopyRenderer.setCopyDepth(true);
          });
          m_textureCopyRenderer.setFlipY(FLAGS_atlas_vk_copy_yflip_in_shader);
          m_textureCopyRenderer.setSourceAttachments(srcColor, srcDepth);
          m_rendererBase.renderVulkan(eye, m_textureCopyRenderer);
        });

        const Z3DEye eyeCopy = eye;
        script.commands("final_rgba8_readback_enqueue",
                        {segCopy},
                        [enqueueFinalReadbackInActiveFrame, rgba8Lease, eyeCopy, frameLocalPtr, outLease](
                          Z3DRendererVulkanBackend& backend) {
                          ZVulkanTexture* rgba8Tex = rgba8Lease ? rgba8Lease->colorAttachment(0) : nullptr;
                          CHECK(rgba8Tex != nullptr) << "VK final_rgba8_copy: missing RGBA8 color attachment";
                          enqueueFinalReadbackInActiveFrame(backend,
                                                            *rgba8Tex,
                                                            frameLocalPtr,
                                                            outLease,
                                                            eyeCopy,
                                                            "vk_final_rgba8_readback",
                                                            "vk_final_rgba8_readback_consume",
                                                            /*noCopy=*/false);
                        });
      }
    } else if (finalColor) {
      // No conversion needed; enqueue readback on the tail submission.
      const Z3DEye eyeCopy = eye;
      script.commands("final_color_readback_enqueue",
                      {},
                      [enqueueFinalReadbackInActiveFrame, finalColor, eyeCopy, frameLocalPtr, outLease](
                        Z3DRendererVulkanBackend& backend) {
                        enqueueFinalReadbackInActiveFrame(backend,
                                                          *finalColor,
                                                          frameLocalPtr,
                                                          outLease,
                                                          eyeCopy,
                                                          "vk_final_color_readback",
                                                          "vk_final_color_readback_consume",
                                                          /*noCopy=*/true);
                      });
    }
    // Do not enqueue picking readback; rely on synchronous 1x1 reads on demand.
  }

  // Explicit flush so cancellation exceptions propagate (do not rely on script destructor).
  script.flush("compositor_done");

  return 1.0;
}

void Z3DCompositor::updateSize(const glm::uvec2& targetSize)
{
  if (targetSize == m_outputSize) {
    invalidate(State::AllResultInvalid);
    return;
  }

  CHECK_GT(targetSize.x, 0u);
  CHECK_GT(targetSize.y, 0u);

  m_outputSize = targetSize;
  ensureOutputTargets(m_outputSize);
  invalidate(State::AllResultInvalid);
}

void Z3DCompositor::recordSceneSegmentsVulkan(const std::vector<Z3DBoundedFilter*>& opaqueFilters,
                                              const std::vector<Z3DBoundedFilter*>& transparentFilters,
                                              Z3DScratchResourcePool::RenderTargetLease& sceneOutLease,
                                              Z3DEye eye,
                                              bool includeGeometry,
                                              bool clearAtStart,
                                              bool drawBackground,
                                              ZVulkanLinearScript& script)
{
  // Note: this helper expresses compositor logic in a linear, GL-like way.
  // It does not decide submission boundaries; ZVulkanLinearScript owns frame
  // begin/end (and may choose to merge segments).

  const glm::uvec2 targetSize = sceneOutLease.descriptor.size;

  ZVulkanLinearScript::SegmentHandle segLast{};

  // Background first (mirrors GL: draw background, then blend geometry over it).
  if (drawBackground && m_showBackground.get()) {
    vlogVulkanLease("background", sceneOutLease);
    segLast = script.raster("background", {}, [&]() {
      // Set initial surface with optional clear at start. This determines load/store
      // semantics for the first pass that actually begins rendering on this surface.
      m_rendererBase.setActiveSurfaceWithLoadStore(sceneOutLease,
                                                   clearAtStart ? LoadOp::Clear : LoadOp::Load,
                                                   StoreOp::Store,
                                                   clearAtStart ? LoadOp::Clear : LoadOp::Load,
                                                   StoreOp::Store);
      m_rendererBase.renderVulkan(eye, m_backgroundRenderer);
    });
  }

  if (!includeGeometry) {
    return;
  }

  const bool haveGeometry = (!opaqueFilters.empty() || !transparentFilters.empty());
  if (!haveGeometry) {
    return;
  }

  // Geometry should always clear depth, and should clear color only when the
  // caller asked for it and we did not already draw the background.
  const bool backgroundDrawn = (drawBackground && m_showBackground.get());
  const LoadOp geomColorLoad = (clearAtStart && !backgroundDrawn) ? LoadOp::Clear : LoadOp::Load;
  vlogVulkanLease("geometry", sceneOutLease);

  const auto segGeometry =
    script.raster("geometry",
                  segLast ? std::initializer_list<ZVulkanLinearScript::SegmentHandle>{segLast}
                          : std::initializer_list<ZVulkanLinearScript::SegmentHandle>{},
                  [&]() {
                    m_rendererBase.setActiveSurfaceWithLoadStore(sceneOutLease,
                                                                 geomColorLoad,
                                                                 StoreOp::Store,
                                                                 LoadOp::Clear,
                                                                 StoreOp::Store);
                    const auto surface = m_rendererBase.frameState().activeSurface;

                    for (auto* filter : opaqueFilters) {
                      if (!filter) {
                        continue;
                      }
                      recordFilterBatchesToSurfaceUnified(
                        m_rendererBase,
                        filter,
                        surface,
                        [&]() {
                          filter->renderOpaque(eye);
                        },
                        /*propagateHookPara=*/false);
                    }

                    for (auto* filter : transparentFilters) {
                      if (!filter) {
                        continue;
                      }
                      recordTransparentFilterBatchesToSurfaceUnified(m_rendererBase,
                                                                     filter,
                                                                     surface,
                                                                     eye,
                                                                     /*propagateHookPara=*/false);
                    }
                  });

  // Glow overlays (applied on top of the base scene).
  auto overlayGlow = [&](const std::vector<Z3DBoundedFilter*>& filters, ZVulkanLinearScript::SegmentHandle depsScene) {
    if (filters.empty()) {
      return;
    }

    auto& pool = Z3DRenderGlobalState::instance().scratchPool();
    ZVulkanLinearScript::SegmentHandle deps = depsScene;

    for (auto* filter : filters) {
      if (!filter) {
        continue;
      }
      auto* glowPara = filter->parameter("Glow");
      auto* glowBool = glowPara ? dynamic_cast<ZBoolParameter*>(glowPara) : nullptr;
      if (!glowBool || !glowBool->get()) {
        continue;
      }

      auto glowGeomLease = std::make_shared<Z3DScratchResourcePool::RenderTargetLease>(
        pool.acquireTempRenderTarget2D(targetSize,
                                       ScratchFormat::RGBA16,
                                       ScratchFormat::Depth32F,
                                       RenderBackend::Vulkan));
      script.keepAlive(glowGeomLease);

      auto blurXLease = std::make_shared<Z3DScratchResourcePool::RenderTargetLease>(
        pool.acquireTempRenderTarget2D(targetSize,
                                       ScratchFormat::RGBA16,
                                       ScratchFormat::Depth32F,
                                       RenderBackend::Vulkan));
      script.keepAlive(blurXLease);

      auto blurYLease = std::make_shared<Z3DScratchResourcePool::RenderTargetLease>(
        pool.acquireTempRenderTarget2D(targetSize,
                                       ScratchFormat::RGBA16,
                                       ScratchFormat::Depth32F,
                                       RenderBackend::Vulkan));
      script.keepAlive(blurYLease);

      const auto segGlowGeom = script.raster("glow_geometry",
                                             deps ? std::initializer_list<ZVulkanLinearScript::SegmentHandle>{deps}
                                                  : std::initializer_list<ZVulkanLinearScript::SegmentHandle>{},
                                             [&]() {
                                               m_rendererBase.setActiveSurfaceWithLoadStore(*glowGeomLease,
                                                                                            LoadOp::Clear,
                                                                                            StoreOp::Store,
                                                                                            LoadOp::Clear,
                                                                                            StoreOp::Store);
                                               for (auto& att :
                                                    m_rendererBase.frameState().activeSurface.colorAttachments) {
                                                 att.finalUse = AttachmentFinalUse::Sampled;
                                               }
                                               if (m_rendererBase.frameState().activeSurface.depthAttachment) {
                                                 m_rendererBase.frameState().activeSurface.depthAttachment->finalUse =
                                                   AttachmentFinalUse::Sampled;
                                               }
                                               const auto glowGeomSurface = m_rendererBase.frameState().activeSurface;
                                               recordFilterBatchesToSurfaceUnified(
                                                 m_rendererBase,
                                                 filter,
                                                 glowGeomSurface,
                                                 [&]() {
                                                   filter->renderOpaque(eye);
                                                 },
                                                 /*propagateHookPara=*/false);
                                             });

      // Sync glow parameters from the filter to the compositor renderer.
      if (auto* mode = dynamic_cast<ZStringIntOptionParameter*>(filter->parameter("Glow Mode"))) {
        m_glowRenderer.setGlowMode(static_cast<GlowMode>(mode->associatedData()));
      }
      if (auto* radius = dynamic_cast<ZIntParameter*>(filter->parameter("Glow Blur Radius"))) {
        m_glowRenderer.setBlurRadius(radius->get());
      }
      if (auto* scale = dynamic_cast<ZFloatParameter*>(filter->parameter("Glow Blur Scale"))) {
        m_glowRenderer.setBlurScale(scale->get());
      }
      if (auto* strength = dynamic_cast<ZFloatParameter*>(filter->parameter("Glow Blur Strength"))) {
        m_glowRenderer.setBlurStrength(strength->get());
      }

      AttachmentHandle glowColorHandle{};
      glowColorHandle.backend = RenderBackend::Vulkan;
      glowColorHandle.index = 0;
      glowColorHandle.id = reinterpret_cast<uint64_t>(glowGeomLease->colorAttachment(0));
      AttachmentHandle glowDepthHandle{};
      glowDepthHandle.backend = RenderBackend::Vulkan;
      glowDepthHandle.index = 0;
      glowDepthHandle.id = reinterpret_cast<uint64_t>(glowGeomLease->depthAttachmentTexture());

      AttachmentHandle blurXColorHandle{};
      blurXColorHandle.backend = RenderBackend::Vulkan;
      blurXColorHandle.index = 0;
      blurXColorHandle.id = reinterpret_cast<uint64_t>(blurXLease->colorAttachment(0));
      AttachmentHandle blurXDepthHandle{};
      blurXDepthHandle.backend = RenderBackend::Vulkan;
      blurXDepthHandle.index = 0;
      blurXDepthHandle.id = reinterpret_cast<uint64_t>(blurXLease->depthAttachmentTexture());

      AttachmentHandle blurYColorHandle{};
      blurYColorHandle.backend = RenderBackend::Vulkan;
      blurYColorHandle.index = 0;
      blurYColorHandle.id = reinterpret_cast<uint64_t>(blurYLease->colorAttachment(0));
      AttachmentHandle blurYDepthHandle{};
      blurYDepthHandle.backend = RenderBackend::Vulkan;
      blurYDepthHandle.index = 0;
      blurYDepthHandle.id = reinterpret_cast<uint64_t>(blurYLease->depthAttachmentTexture());

      const auto segBlurX = script.raster("glow_blur_x", {segGlowGeom}, [&]() {
        m_rendererBase.setActiveSurfaceWithLoadStore(*blurXLease,
                                                     LoadOp::Clear,
                                                     StoreOp::Store,
                                                     LoadOp::Clear,
                                                     StoreOp::Store);
        for (auto& att : m_rendererBase.frameState().activeSurface.colorAttachments) {
          att.finalUse = AttachmentFinalUse::Sampled;
        }
        if (m_rendererBase.frameState().activeSurface.depthAttachment) {
          m_rendererBase.frameState().activeSurface.depthAttachment->finalUse = AttachmentFinalUse::Sampled;
        }

        TextureGlowPayload blurPayload{};
        blurPayload.stage = TextureGlowPayload::Stage::BlurX;
        blurPayload.colorAttachmentHandle = glowColorHandle;
        blurPayload.depthAttachmentHandle = glowDepthHandle;
        blurPayload.mode = m_glowRenderer.glowMode();
        blurPayload.blurRadius = m_glowRenderer.blurRadius();
        blurPayload.blurScale = m_glowRenderer.blurScale();
        blurPayload.blurStrength = m_glowRenderer.blurStrength();

        RenderBatch batch{};
        batch.eye = eye;
        batch.pass.externalImageUses.push_back(
          {glowColorHandle, ExternalImageUseKind::SampledRead, ExternalImageAspectHint::Color});
        batch.pass.externalImageUses.push_back(
          {glowDepthHandle, ExternalImageUseKind::SampledRead, ExternalImageAspectHint::Depth});
        batch.draw.topology = PrimitiveTopology::TriangleStrip;
        batch.draw.vertexCount = 4;
        batch.draw.indexCount = 0;
        batch.geometry = std::move(blurPayload);
        m_rendererBase.appendBatch(std::move(batch));
      });

      const auto segBlurY = script.raster("glow_blur_y", {segBlurX}, [&]() {
        m_rendererBase.setActiveSurfaceWithLoadStore(*blurYLease,
                                                     LoadOp::Clear,
                                                     StoreOp::Store,
                                                     LoadOp::Clear,
                                                     StoreOp::Store);
        for (auto& att : m_rendererBase.frameState().activeSurface.colorAttachments) {
          att.finalUse = AttachmentFinalUse::Sampled;
        }
        if (m_rendererBase.frameState().activeSurface.depthAttachment) {
          m_rendererBase.frameState().activeSurface.depthAttachment->finalUse = AttachmentFinalUse::Sampled;
        }

        TextureGlowPayload blurPayload{};
        blurPayload.stage = TextureGlowPayload::Stage::BlurY;
        blurPayload.colorAttachmentHandle = blurXColorHandle;
        blurPayload.depthAttachmentHandle = blurXDepthHandle;
        blurPayload.mode = m_glowRenderer.glowMode();
        blurPayload.blurRadius = m_glowRenderer.blurRadius();
        blurPayload.blurScale = m_glowRenderer.blurScale();
        blurPayload.blurStrength = m_glowRenderer.blurStrength();

        RenderBatch batch{};
        batch.eye = eye;
        batch.pass.externalImageUses.push_back(
          {blurXColorHandle, ExternalImageUseKind::SampledRead, ExternalImageAspectHint::Color});
        batch.pass.externalImageUses.push_back(
          {blurXDepthHandle, ExternalImageUseKind::SampledRead, ExternalImageAspectHint::Depth});
        batch.draw.topology = PrimitiveTopology::TriangleStrip;
        batch.draw.vertexCount = 4;
        batch.draw.indexCount = 0;
        batch.geometry = std::move(blurPayload);
        m_rendererBase.appendBatch(std::move(batch));
      });

      deps = script.raster("glow_composite", {segBlurY}, [&]() {
        // Composite glow into the scene output without clearing.
        m_rendererBase.setActiveSurfaceWithLoadStore(sceneOutLease,
                                                     LoadOp::Load,
                                                     StoreOp::Store,
                                                     LoadOp::DontCare,
                                                     StoreOp::Store,
                                                     {});

        TextureGlowPayload glowPayload{};
        glowPayload.stage = TextureGlowPayload::Stage::Composite;
        glowPayload.colorAttachmentHandle = glowColorHandle;
        glowPayload.depthAttachmentHandle = glowDepthHandle;
        glowPayload.blurColorAttachmentHandle = blurYColorHandle;
        glowPayload.blurDepthAttachmentHandle = blurYDepthHandle;
        glowPayload.mode = m_glowRenderer.glowMode();
        glowPayload.blurRadius = m_glowRenderer.blurRadius();
        glowPayload.blurScale = m_glowRenderer.blurScale();
        glowPayload.blurStrength = m_glowRenderer.blurStrength();

        RenderBatch batch{};
        batch.eye = eye;
        batch.pass.externalImageUses.push_back(
          {glowColorHandle, ExternalImageUseKind::SampledRead, ExternalImageAspectHint::Color});
        batch.pass.externalImageUses.push_back(
          {glowDepthHandle, ExternalImageUseKind::SampledRead, ExternalImageAspectHint::Depth});
        batch.pass.externalImageUses.push_back(
          {blurYColorHandle, ExternalImageUseKind::SampledRead, ExternalImageAspectHint::Color});
        batch.pass.externalImageUses.push_back(
          {blurYDepthHandle, ExternalImageUseKind::SampledRead, ExternalImageAspectHint::Depth});
        batch.draw.topology = PrimitiveTopology::TriangleStrip;
        batch.draw.vertexCount = 4;
        batch.draw.indexCount = 0;
        batch.geometry = std::move(glowPayload);
        m_rendererBase.appendBatch(std::move(batch));
      });
    }
  };

  overlayGlow(transparentFilters, segGeometry);
  overlayGlow(opaqueFilters, segGeometry);
}

void Z3DCompositor::ensureOutputTargets(const glm::uvec2& size)
{
  auto ensureLease = [&](Z3DScratchResourcePool::RenderTargetLease& lease) {
    const RenderBackend activeBackend = m_rendererBase.activeBackend();
    const bool hasVulkanImage = lease.vulkanImage != nullptr;
    const bool hasGLTarget = lease.renderTarget != nullptr;

    bool sizeMismatch = false;
    if (activeBackend == RenderBackend::Vulkan && hasVulkanImage) {
      const auto& descriptorSize = lease.descriptor.size;
      sizeMismatch = descriptorSize.x != size.x || descriptorSize.y != size.y;
    } else if (activeBackend == RenderBackend::OpenGL && hasGLTarget) {
      sizeMismatch = lease.renderTarget->size() != size;
    }

    const bool backendMismatch = lease.backend != activeBackend;
    const bool missingResource = (activeBackend == RenderBackend::Vulkan && !hasVulkanImage) ||
                                 (activeBackend == RenderBackend::OpenGL && !hasGLTarget);

    if (missingResource || backendMismatch || sizeMismatch) {
      lease.release();
      if (activeBackend == RenderBackend::Vulkan) {
        Z3DRenderGlobalState::instance().scratchPool().reclaimVulkanScratchMemory(
          Z3DScratchResourcePool::VulkanScratchReclaimMode::WaitForIdle);
      }
      m_rendererBase.acquirePersistentTempRenderTarget2D(lease, size);
      LOG(INFO) << fmt::format("ensureOutputTargets reacquired {}x{} backend={} hasVulkanImage={} hasGLTarget={}",
                               size.x,
                               size.y,
                               lease.backend == RenderBackend::Vulkan ? "Vulkan" : "OpenGL",
                               lease.vulkanImage != nullptr,
                               lease.renderTarget != nullptr);
    }
  };

  ensureLease(m_outRenderTarget1);
  ensureLease(m_outRenderTarget2);
  ensureLease(m_leftEyeOutRenderTarget1);
  ensureLease(m_leftEyeOutRenderTarget2);

  // Keep renderer viewport in sync with the active output size so batches
  // recorded immediately after a resize use a valid renderArea that matches
  // the attachments. This avoids beginRendering validation errors when the
  // viewport still reflects the previous size.
  m_rendererBase.frameState().updateViewportData(size);
}

void Z3DCompositor::switchBackend(RenderBackend backendRequest)
{
  const RenderBackend previousBackend = m_rendererBase.activeBackend();
  VLOG(1) << fmt::format("Compositor switching backend to {}", enumToString(backendRequest));

  if (previousBackend != backendRequest) {
    VLOG(1) << "Resetting picking manager render target for backend change";
    m_globalParameters.pickingManager.resetRenderTarget();
  }

  // When leaving Vulkan, release any outstanding zero-copy readback leases while the Vulkan
  // backend is still alive; the release lambdas close over the backend instance.
  if (previousBackend == RenderBackend::Vulkan && backendRequest == RenderBackend::OpenGL) {
    VLOG(1) << "Releasing outstanding Vulkan readback leases before backend switch";
    auto clearExternal = [](Z3DLocalColorBuffer* buf) {
      if (!buf) {
        return;
      }
      if (buf->externalRelease) {
        buf->externalRelease();
        buf->externalRelease = {};
      }
      buf->external = nullptr;
      buf->externalStride = 0;
    };
    clearExternal(m_monoCurrentLocalBuffer);
    clearExternal(m_monoReadyLocalBuffer);
    clearExternal(m_leftCurrentLocalBuffer);
    clearExternal(m_leftReadyLocalBuffer);
    clearExternal(m_rightCurrentLocalBuffer);
    clearExternal(m_rightReadyLocalBuffer);
  }

  Z3DBoundedFilter::switchRendererBackend(backendRequest);

  // If switching away from OpenGL, proactively dispose compositor-owned GL
  // shader programs while a valid GL context is current to avoid deleting
  // them later against a different or missing context.
  if (backendRequest == RenderBackend::Vulkan) {
    VLOG(1) << "Releasing compositor-owned GL shader programs before switching to Vulkan";
    m_screenQuadVAO.reset();
    m_ddpBlendShader.reset();
    m_ddpFinalShader.reset();
    m_waFinalShader.reset();
    m_wbFinalShader.reset();
  }

  if (backendRequest == RenderBackend::OpenGL) {
    // Rebuild compositor-owned GL shader programs in the new context.
    VLOG(1) << "Recreating compositor-owned GL shader programs after switching to OpenGL";
    m_screenQuadVAO = std::make_unique<Z3DVertexArrayObject>(1);
    m_ddpBlendShader = std::make_unique<Z3DShaderProgram>();
    m_ddpBlendShader->loadFromSourceFile("pass.vert", "dual_peeling_blend.frag", m_rendererBase.generateHeader());

    m_ddpFinalShader = std::make_unique<Z3DShaderProgram>();
    m_ddpFinalShader->loadFromSourceFile("pass.vert", "dual_peeling_final.frag", m_rendererBase.generateHeader());

    m_waFinalShader = std::make_unique<Z3DShaderProgram>();
    m_waFinalShader->loadFromSourceFile("pass.vert", "wavg_final.frag", m_rendererBase.generateHeader());

    m_wbFinalShader = std::make_unique<Z3DShaderProgram>();
    m_wbFinalShader->loadFromSourceFile("pass.vert", "wblended_final.frag", m_rendererBase.generateHeader());
  }

  VLOG(1) << "Updating axis camera for new backend";
  setupAxisCamera();

  VLOG(1) << "Invalidating compositor state after backend switch";
  invalidate(State::AllResultInvalid);
  VLOG(1) << "Compositor backend switch complete";
}

void Z3DCompositor::renderGeometries(const std::vector<Z3DBoundedFilter*>& opaqueFilters,
                                     const std::vector<Z3DBoundedFilter*>& transparentFilters,
                                     Z3DScratchResourcePool::RenderTargetLease& targetLease,
                                     Z3DEye eye)
{
  const auto transparencyMode = m_rendererBase.sceneState().transparency;
  if (transparencyMode == TransparencyMode::BlendNoDepthMask) {
    renderGeomsBlendNoDepthMask(opaqueFilters, transparentFilters, targetLease, eye);
  } else if (transparencyMode == TransparencyMode::BlendDelayed) {
    renderGeomsBlendDelayed(opaqueFilters, transparentFilters, targetLease, eye);
  } else {
    renderGeomsOIT(opaqueFilters, transparentFilters, targetLease, eye, transparencyMode);
  }
}

void Z3DCompositor::renderTransparentFilter(Z3DBoundedFilter* filter,
                                            Z3DScratchResourcePool::RenderTargetLease& targetLease,
                                            Z3DEye eye)
{
  if (!filter) {
    return;
  }
  auto* glTarget = targetLease.renderTarget;
  CHECK(glTarget != nullptr);
  const auto targetSize = glTarget->size();

  auto glowPara = filter->parameter("Glow");
  auto* glowBool = glowPara ? dynamic_cast<ZBoolParameter*>(glowPara) : nullptr;
  if (glowBool && glowBool->get()) {
    GLboolean prevDepthMask = GL_TRUE;
    glGetBooleanv(GL_DEPTH_WRITEMASK, &prevDepthMask);
    if (prevDepthMask == GL_FALSE) {
      glDepthMask(GL_TRUE);
    }
    // 1) render filter geometry to pooled glow temp target 1
    auto glowLease1 = Z3DRenderGlobalState::instance().scratchPool().acquireTempRenderTarget2D(targetSize);
    glowLease1.renderTarget->bind();
    glowLease1.renderTarget->clear();
    filter->setViewport(glowLease1.renderTarget->size());
    filter->renderOpaque(eye);
    glowLease1.renderTarget->release();

    // 2) sync glow params and render glow into temp2
    if (auto* mode = dynamic_cast<ZStringIntOptionParameter*>(filter->parameter("Glow Mode"))) {
      m_glowRenderer.setGlowMode(static_cast<GlowMode>(mode->associatedData()));
    }
    if (auto* radius = dynamic_cast<ZIntParameter*>(filter->parameter("Glow Blur Radius"))) {
      m_glowRenderer.setBlurRadius(radius->get());
    }
    if (auto* scale = dynamic_cast<ZFloatParameter*>(filter->parameter("Glow Blur Scale"))) {
      m_glowRenderer.setBlurScale(scale->get());
    }
    if (auto* strength = dynamic_cast<ZFloatParameter*>(filter->parameter("Glow Blur Strength"))) {
      m_glowRenderer.setBlurStrength(strength->get());
    }

    auto glowLease2 = Z3DRenderGlobalState::instance().scratchPool().acquireTempRenderTarget2D(targetSize);
    glowLease2.renderTarget->bind();
    glowLease2.renderTarget->clear();
    setViewport(glowLease2.renderTarget->size());
    m_glowRenderer.setColorTexture(glowLease1.renderTarget->attachment(GL_COLOR_ATTACHMENT0));
    m_glowRenderer.setDepthTexture(glowLease1.renderTarget->attachment(GL_DEPTH_ATTACHMENT));
    m_rendererBase.render(eye, m_glowRenderer);
    glowLease2.renderTarget->release();

    // Restore previous depth state
    if (prevDepthMask == GL_FALSE) {
      glDepthMask(GL_FALSE);
    }

    // 3) copy glow result directly into renderTarget (skip alpha blend)
    setViewport(targetLease.renderTarget->size());
    m_textureCopyRenderer.setColorTexture(glowLease2.renderTarget->attachment(GL_COLOR_ATTACHMENT0));
    m_textureCopyRenderer.setDepthTexture(glowLease2.renderTarget->attachment(GL_DEPTH_ATTACHMENT));
    m_rendererBase.render(eye, m_textureCopyRenderer);
  } else {
    // default path
    filter->setViewport(targetLease.renderTarget->size());
    filter->renderTransparent(eye);
  }
}

void Z3DCompositor::renderGeomsBlendDelayed(const std::vector<Z3DBoundedFilter*>& opaqueFilters,
                                            const std::vector<Z3DBoundedFilter*>& transparentFilters,
                                            Z3DScratchResourcePool::RenderTargetLease& targetLease,
                                            Z3DEye eye)
{
  auto* glTarget = targetLease.renderTarget;
  CHECK(glTarget != nullptr);

  glTarget->bind();
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

  for (auto filter : opaqueFilters) {
    filter->setViewport(glTarget->size());
    filter->renderOpaque(eye);
  }

  for (auto filter : transparentFilters) {
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    renderTransparentFilter(filter, targetLease, eye);
    glBlendFunc(GL_ONE, GL_ZERO);
    glDisable(GL_BLEND);
  }

  glTarget->release();
}

void Z3DCompositor::renderGeomsBlendNoDepthMask(const std::vector<Z3DBoundedFilter*>& opaqueFilters,
                                                const std::vector<Z3DBoundedFilter*>& transparentFilters,
                                                Z3DScratchResourcePool::RenderTargetLease& targetLease,
                                                Z3DEye eye)
{
  auto* glTarget = targetLease.renderTarget;
  CHECK(glTarget != nullptr);

  glTarget->bind();
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

  for (auto filter : opaqueFilters) {
    filter->setViewport(glTarget->size());
    filter->renderOpaque(eye);
  }

  for (auto filter : transparentFilters) {
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glDepthMask(GL_FALSE);
    renderTransparentFilter(filter, targetLease, eye);
    glBlendFunc(GL_ONE, GL_ZERO);
    glDisable(GL_BLEND);
    glDepthMask(GL_TRUE);
  }

  glTarget->release();
}

void Z3DCompositor::renderGeomsOIT(const std::vector<Z3DBoundedFilter*>& opaqueFilters,
                                   const std::vector<Z3DBoundedFilter*>& transparentFilters,
                                   Z3DScratchResourcePool::RenderTargetLease& targetLease,
                                   Z3DEye eye,
                                   TransparencyMode mode)
{
  auto* glTarget = targetLease.renderTarget;
  CHECK(glTarget != nullptr);
  const auto targetSize = glTarget->size();

  // Build per-layer lists for OIT: start with the base image pair if present
  std::vector<const Z3DTexture*> imageColorTexList;
  std::vector<const Z3DTexture*> imageDepthTexList;

  // Append each non-opaque image layer from connected image filters so they
  // participate individually in OIT, instead of a pre-merged single layer.
  auto nonOpaqueLayers = collectNonOpaqueImageLayers(eye);
  for (const auto& layer : nonOpaqueLayers) {
    if (layer.glColorTexture && layer.glDepthTexture) {
      imageColorTexList.push_back(layer.glColorTexture);
      imageDepthTexList.push_back(layer.glDepthTexture);
    }
  }

  // Precompute per-glow color/depth layers (one pair per glow-enabled filter)
  std::vector<Z3DBoundedFilter*> glowFilters;
  // Hold leases for glow results so textures remain valid through OIT blending
  std::vector<Z3DScratchResourcePool::RenderTargetLease> glowLayerLeases;
  glowFilters.reserve(transparentFilters.size());
  for (auto f : transparentFilters) {
    if (auto p = f->parameter("Glow")) {
      if (auto* b = dynamic_cast<ZBoolParameter*>(p); b && b->get()) {
        glowFilters.push_back(f);
      }
    }
  }
  if (!glowFilters.empty()) {
    glowLayerLeases.reserve(glowFilters.size());

    for (auto gf : glowFilters) {
      // Compute glow result (color+depth) for this filter into m_glowTempRenderTarget2
      // Ensure depth test/writes for geometry prepass
      GLboolean prevDepthMask = GL_TRUE;
      glGetBooleanv(GL_DEPTH_WRITEMASK, &prevDepthMask);
      GLboolean prevDepthTest = glIsEnabled(GL_DEPTH_TEST);
      if (prevDepthTest == GL_FALSE) {
        glEnable(GL_DEPTH_TEST);
      }
      if (prevDepthMask == GL_FALSE) {
        glDepthMask(GL_TRUE);
      }

      // Geometry prepass (use pooled temp)
      auto glowGeomLease = Z3DRenderGlobalState::instance().scratchPool().acquireTempRenderTarget2D(targetSize);
      // VLOG(1) << "lease acquired";
      glowGeomLease.renderTarget->bind();
      glowGeomLease.renderTarget->clear();
      gf->setViewport(glowGeomLease.renderTarget->size());
      gf->renderOpaque(eye);
      glowGeomLease.renderTarget->release();

      // Glow blur/composition for this object directly into a pooled layer RT
      glowLayerLeases.emplace_back(
        Z3DRenderGlobalState::instance().scratchPool().acquireTempRenderTarget2D(targetSize));
      // VLOG(1) << "lease acquired";
      auto* layerRT = glowLayerLeases.back().renderTarget;
      layerRT->bind();
      layerRT->clear();
      setViewport(layerRT->size());
      if (auto* glowModeParam = dynamic_cast<ZStringIntOptionParameter*>(gf->parameter("Glow Mode"))) {
        m_glowRenderer.setGlowMode(static_cast<GlowMode>(glowModeParam->associatedData()));
      }
      if (auto* radius = dynamic_cast<ZIntParameter*>(gf->parameter("Glow Blur Radius"))) {
        m_glowRenderer.setBlurRadius(radius->get());
      }
      if (auto* scale = dynamic_cast<ZFloatParameter*>(gf->parameter("Glow Blur Scale"))) {
        m_glowRenderer.setBlurScale(scale->get());
      }
      if (auto* strength = dynamic_cast<ZFloatParameter*>(gf->parameter("Glow Blur Strength"))) {
        m_glowRenderer.setBlurStrength(strength->get());
      }
      m_glowRenderer.setColorTexture(glowGeomLease.renderTarget->attachment(GL_COLOR_ATTACHMENT0));
      m_glowRenderer.setDepthTexture(glowGeomLease.renderTarget->attachment(GL_DEPTH_ATTACHMENT));
      m_rendererBase.render(eye, m_glowRenderer);
      layerRT->release();

      // Restore depth state
      if (prevDepthMask == GL_FALSE) {
        glDepthMask(GL_FALSE);
      }
      if (prevDepthTest == GL_FALSE) {
        glDisable(GL_DEPTH_TEST);
      }

      imageColorTexList.push_back(layerRT->colorTexture());
      imageDepthTexList.push_back(layerRT->depthTexture());
    }
  }

  auto dispatchTransparent = [&](Z3DScratchResourcePool::RenderTargetLease& lease, Z3DTexture* depthTexture) {
    switch (mode) {
      case TransparencyMode::DualDepthPeeling:
      case TransparencyMode::PerPixelFragmentList:
        renderTransparentDDP(transparentFilters, lease, eye, depthTexture, imageColorTexList, imageDepthTexList);
        break;
      case TransparencyMode::WeightedAverage:
        renderTransparentWA(transparentFilters, lease, eye, depthTexture, imageColorTexList, imageDepthTexList);
        break;
      case TransparencyMode::WeightedBlended:
        renderTransparentWB(transparentFilters, lease, eye, depthTexture, imageColorTexList, imageDepthTexList);
        break;
      default:
        LOG(FATAL) << "renderGeomsOIT called with unsupported transparency mode: " << static_cast<int>(mode);
    }
  };

  //  std::vector<Z3DBoundedFilter*> allFilters;
  //  allFilters.insert(allFilters.end(), opaqueFilters.begin(), opaqueFilters.end());
  //  allFilters.insert(allFilters.end(), transparentFilters.begin(), transparentFilters.end());
  //  if (mode == "Dual Depth Peeling") {
  //    renderTransparentDDP(allFilters, port, eye);
  //  } else if (mode == "Weighted Average") {
  //    renderTransparentWA(allFilters, port, eye);
  //  }
  //  return;
  if (transparentFilters.empty() && imageColorTexList.empty()) {
    renderOpaqueFilters(opaqueFilters, targetLease, eye);
  }
  //  else {
  //    if (mode == "Dual Depth Peeling") {
  //      renderTransparentDDP(renderers, port, eye);
  //    }
  //  }
  else if (opaqueFilters.empty()) {
    dispatchTransparent(targetLease, nullptr);
  } else {
    auto leaseOpaque = Z3DRenderGlobalState::instance().scratchPool().acquireTempRenderTarget2D(targetSize);
    // VLOG(1) << "lease acquired";
    renderOpaqueFilters(opaqueFilters, leaseOpaque, eye);

    auto leaseTrans = Z3DRenderGlobalState::instance().scratchPool().acquireTempRenderTarget2D(targetSize);
    // VLOG(1) << "lease acquired";
    dispatchTransparent(leaseTrans, leaseOpaque.renderTarget->depthTexture());

    glTarget->bind();
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    setViewport(glTarget->size());
    m_alphaBlendRenderer.setColorTexture1(leaseOpaque.renderTarget->colorTexture());
    m_alphaBlendRenderer.setDepthTexture1(leaseOpaque.renderTarget->depthTexture());
    m_alphaBlendRenderer.setColorTexture2(leaseTrans.renderTarget->colorTexture());
    m_alphaBlendRenderer.setDepthTexture2(leaseTrans.renderTarget->depthTexture());
    m_rendererBase.render(eye, m_alphaBlendRenderer);

    glTarget->release();
  }
}

void Z3DCompositor::renderOpaqueFilters(const std::vector<Z3DBoundedFilter*>& filters,
                                        Z3DScratchResourcePool::RenderTargetLease& targetLease,
                                        Z3DEye eye)
{
  auto* glTarget = targetLease.renderTarget;
  CHECK(glTarget != nullptr);

  glTarget->bind();
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
  for (auto filter : filters) {
    filter->setViewport(glTarget->size());
    filter->renderOpaque(eye);
  }
  glTarget->release();
}

// Vector-list overload: feeds multiple image pairs through DDP
void Z3DCompositor::renderTransparentDDP(const std::vector<Z3DBoundedFilter*>& filters,
                                         Z3DScratchResourcePool::RenderTargetLease& targetLease,
                                         Z3DEye eye,
                                         Z3DTexture* depthTexture,
                                         const std::vector<const Z3DTexture*>& imageColorTexList,
                                         const std::vector<const Z3DTexture*>& imageDepthTexList)
{
  auto* glTarget = targetLease.renderTarget;
  CHECK(glTarget != nullptr);
  const auto targetSize = glTarget->size();

  auto& ddpLease = ensureDDPRenderTarget(targetSize);
  Z3DRenderTarget& ddpRT = *ddpLease.renderTarget;
  if (depthTexture) {
    ddpRT.attachTextureToFBO(depthTexture, GL_DEPTH_ATTACHMENT, false);
    ddpRT.isFBOComplete();
  }

  const Z3DTexture* g_dualDepthTexId[2];
  g_dualDepthTexId[0] = ddpRT.attachment(GL_COLOR_ATTACHMENT0);
  g_dualDepthTexId[1] = ddpRT.attachment(GL_COLOR_ATTACHMENT3);
  const Z3DTexture* g_dualFrontBlenderTexId[2];
  g_dualFrontBlenderTexId[0] = ddpRT.attachment(GL_COLOR_ATTACHMENT1);
  g_dualFrontBlenderTexId[1] = ddpRT.attachment(GL_COLOR_ATTACHMENT4);
  const Z3DTexture* g_dualBackTempTexId[2];
  g_dualBackTempTexId[0] = ddpRT.attachment(GL_COLOR_ATTACHMENT2);
  g_dualBackTempTexId[1] = ddpRT.attachment(GL_COLOR_ATTACHMENT5);
  const Z3DTexture* g_dualBackBlenderTexId = ddpRT.attachment(GL_COLOR_ATTACHMENT6);
  const Z3DTexture* g_depthTex = ddpRT.attachment(GL_COLOR_ATTACHMENT7);
  const GLenum g_drawBuffers[] = {GL_COLOR_ATTACHMENT0,
                                  GL_COLOR_ATTACHMENT1,
                                  GL_COLOR_ATTACHMENT2,
                                  GL_COLOR_ATTACHMENT3,
                                  GL_COLOR_ATTACHMENT4,
                                  GL_COLOR_ATTACHMENT5,
                                  GL_COLOR_ATTACHMENT6};

  const GLenum g_db[] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT7};

  bool g_useOQ = true;
  size_t g_numPasses = static_cast<size_t>(std::max(1, FLAGS_atlas_ddp_max_passes));

#define MAX_DEPTH 1.0

  if (depthTexture) {
    glDepthMask(GL_FALSE);
  } else {
    glDisable(GL_DEPTH_TEST);
  }
  glEnable(GL_BLEND);

  // ---------------------------------------------------------------------
  // 1. Initialize Min-Max Depth Buffer
  // ---------------------------------------------------------------------

  ddpRT.bind();

  // Render targets 1 and 2 store the front and back colors
  // Clear to 0.0 and use MAX blending to filter written color
  // At most one front color and one back color can be written every pass
  glDrawBuffers(2, &g_drawBuffers[1]);
  glClearColor(0, 0, 0, 0);
  glClear(GL_COLOR_BUFFER_BIT);

  // Render target 0 stores (-minDepth, maxDepth, alphaMultiplier)
  glDrawBuffers(2, g_db);
  glClearColor(-MAX_DEPTH, -MAX_DEPTH, 0, 0);
  glClear(GL_COLOR_BUFFER_BIT);
  glBlendEquation(GL_MAX);

  for (auto filter : filters) {
    filter->setViewport(ddpRT.size());
    filter->setShaderHookType(Z3DRendererBase::ShaderHookType::DualDepthPeelingInit);
    filter->renderTransparent(eye);
  }
  if (!imageColorTexList.empty()) {
    setViewport(ddpRT.size());
    m_rendererBase.setShaderHookType(Z3DRendererBase::ShaderHookType::DualDepthPeelingInit);
    size_t n = std::min(imageColorTexList.size(), imageDepthTexList.size());
    for (size_t i = 0; i < n; ++i) {
      m_textureCopyRenderer.setColorTexture(imageColorTexList[i]);
      m_textureCopyRenderer.setDepthTexture(imageDepthTexList[i]);
      m_rendererBase.render(eye, m_textureCopyRenderer);
    }
  }

  // ---------------------------------------------------------------------
  // 2. Dual Depth Peeling + Blending
  // ---------------------------------------------------------------------

  // Since we cannot blend the back colors in the geometry passes,
  // we use another render target to do the alpha blending
  glDrawBuffer(g_drawBuffers[6]);
  glClearColor(0, 0, 0, 0);
  glClear(GL_COLOR_BUFFER_BIT);

  size_t currId = 0;
  size_t executedPasses = 0;
  for (size_t pass = 1; g_useOQ && pass < g_numPasses; pass++) {
    currId = pass % 2;
    auto prevId = 1 - currId;
    auto bufId = currId * 3;

    glDrawBuffers(2, &g_drawBuffers[bufId + 1]);
    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT);

    glDrawBuffer(g_drawBuffers[bufId + 0]);
    glClearColor(-MAX_DEPTH, -MAX_DEPTH, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT);

    // Render target 0: RG32F MAX blending
    // Render target 1: RGBA MAX blending
    // Render target 2: RGBA MAX blending
    glDrawBuffers(3, &g_drawBuffers[bufId + 0]);
    glBlendEquation(GL_MAX);

    for (auto filter : filters) {
      filter->setViewport(ddpRT.size());
      filter->setShaderHookType(Z3DRendererBase::ShaderHookType::DualDepthPeelingPeel);
      filter->setShaderHookParaDDPDepthBlenderTexture(g_dualDepthTexId[prevId]);
      filter->setShaderHookParaDDPFrontBlenderTexture(g_dualFrontBlenderTexId[prevId]);
      filter->renderTransparent(eye);
    }
    if (!imageColorTexList.empty()) {
      setViewport(ddpRT.size());
      m_rendererBase.setShaderHookType(Z3DRendererBase::ShaderHookType::DualDepthPeelingPeel);
      size_t n = std::min(imageColorTexList.size(), imageDepthTexList.size());
      for (size_t i = 0; i < n; ++i) {
        m_textureCopyRenderer.setColorTexture(imageColorTexList[i]);
        m_textureCopyRenderer.setDepthTexture(imageDepthTexList[i]);
        m_rendererBase.render(eye, m_textureCopyRenderer);
      }
    }

    // Full screen pass to alpha-blend the back color
    glDrawBuffer(g_drawBuffers[6]);

    glBlendEquation(GL_FUNC_ADD);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

    GLuint queryId;
    if (g_useOQ) {
      glGenQueries(1, &queryId);
      glBeginQuery(GL_SAMPLES_PASSED, queryId);
    }

    m_ddpBlendShader->bind();
    m_ddpBlendShader->bindTexture("TempTex", g_dualBackTempTexId[currId]);
    const glm::uvec2 ddpBlendSize = ddpRT.size();
    const glm::vec2 ddpBlendScreenDimRcp(ddpBlendSize.x > 0u ? 1.f / static_cast<float>(ddpBlendSize.x) : 0.f,
                                         ddpBlendSize.y > 0u ? 1.f / static_cast<float>(ddpBlendSize.y) : 0.f);
    m_ddpBlendShader->setScreenDimRCPUniform(ddpBlendScreenDimRcp);

    CHECK(m_screenQuadVAO != nullptr);
    CHECK(m_ddpBlendShader != nullptr);
    Z3DPrimitiveRenderer::renderScreenQuad(*m_screenQuadVAO, *m_ddpBlendShader);
    m_ddpBlendShader->release();

    if (g_useOQ) {
      glEndQuery(GL_SAMPLES_PASSED);
      GLuint sample_count;
      glGetQueryObjectuiv(queryId, GL_QUERY_RESULT, &sample_count);
      glDeleteQueries(1, &queryId);
      if (sample_count == 0) {
        break;
      }
    }
    executedPasses++;
  }
  VLOG(1) << fmt::format("DDP GL executed {} peel passes", executedPasses);

  if (depthTexture) {
    ddpRT.detach(GL_DEPTH_ATTACHMENT);
    ddpRT.isFBOComplete();
  }
  ddpRT.release();

  if (depthTexture) {
    glDepthMask(GL_TRUE);
  }
  glClearColor(0, 0, 0, 0);
  glBlendEquation(GL_FUNC_ADD);
  glBlendFunc(GL_ONE, GL_ZERO);
  glDisable(GL_BLEND);

  for (auto filter : filters) {
    filter->setShaderHookType(Z3DRendererBase::ShaderHookType::Normal);
  }
  if (!imageColorTexList.empty()) {
    m_rendererBase.setShaderHookType(Z3DRendererBase::ShaderHookType::Normal);
  }

  // ---------------------------------------------------------------------
  // 3. Final Pass
  // ---------------------------------------------------------------------

  // The resolve shaders output a representative depth via gl_FragDepth. Ensure
  // those values populate the output depth buffer so later overlays (selection
  // box, handles) can depth-test against transparent geometry. When there is no
  // incoming opaque depth attachment we disable depth testing during the peel
  // passes above, so we must explicitly enable depth writes here.
  glEnable(GL_DEPTH_TEST);
  glDepthMask(GL_TRUE);
  glDepthFunc(GL_ALWAYS);

  glTarget->bind();
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
  m_ddpFinalShader->bind();
  m_ddpFinalShader->bindTexture("DepthTex", g_depthTex);
  m_ddpFinalShader->bindTexture("FrontBlenderTex", g_dualFrontBlenderTexId[currId]);
  m_ddpFinalShader->bindTexture("BackBlenderTex", g_dualBackBlenderTexId);

  const glm::uvec2 ddpSize = ddpRT.size();
  const glm::vec2 ddpScreenDimRcp(ddpSize.x > 0u ? 1.f / static_cast<float>(ddpSize.x) : 0.f,
                                  ddpSize.y > 0u ? 1.f / static_cast<float>(ddpSize.y) : 0.f);
  m_ddpFinalShader->setScreenDimRCPUniform(ddpScreenDimRcp);

  CHECK(m_screenQuadVAO != nullptr);
  CHECK(m_ddpFinalShader != nullptr);
  Z3DPrimitiveRenderer::renderScreenQuad(*m_screenQuadVAO, *m_ddpFinalShader);
  m_ddpFinalShader->release();
  glTarget->release();

  glDepthFunc(GL_LESS);
  glEnable(GL_DEPTH_TEST);
}

// Removed single-image overload: use list-based API

void Z3DCompositor::renderTransparentWA(const std::vector<Z3DBoundedFilter*>& filters,
                                        Z3DScratchResourcePool::RenderTargetLease& targetLease,
                                        Z3DEye eye,
                                        Z3DTexture* depthTexture,
                                        const std::vector<const Z3DTexture*>& imageColorTexList,
                                        const std::vector<const Z3DTexture*>& imageDepthTexList)
{
  auto* glTarget = targetLease.renderTarget;
  CHECK(glTarget != nullptr);
  const auto targetSize = glTarget->size();

  auto& waLease = ensureWARenderTarget(targetSize);
  auto& waRT = *waLease.renderTarget;
  if (depthTexture) {
    waRT.attachTextureToFBO(depthTexture, GL_DEPTH_ATTACHMENT, false);
    waRT.isFBOComplete();
  }

  const Z3DTexture* g_accumulationTexId[2];
  g_accumulationTexId[0] = waRT.attachment(GL_COLOR_ATTACHMENT0);
  g_accumulationTexId[1] = waRT.attachment(GL_COLOR_ATTACHMENT1);
  const GLenum g_drawBuffers[] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1};

  if (depthTexture) {
    glDepthMask(GL_FALSE);
  } else {
    glDisable(GL_DEPTH_TEST);
  }

  waRT.bind();
  glDrawBuffers(2, g_drawBuffers);
  float clearColorZero[4] = {0.f, 0.f, 0.f, 0.f};
  glClearBufferfv(GL_COLOR, 0, clearColorZero);
  glClearBufferfv(GL_COLOR, 1, clearColorZero);

  glEnable(GL_BLEND);
  glBlendEquation(GL_FUNC_ADD);
  glBlendFunci(0, GL_ONE, GL_ONE);
  glBlendFunci(1, GL_ONE, GL_ONE);

  for (auto filter : filters) {
    filter->setViewport(waRT.size());
    filter->setShaderHookType(Z3DRendererBase::ShaderHookType::WeightedAverageInit);
    filter->renderTransparent(eye);
  }
  if (!imageColorTexList.empty()) {
    setViewport(waRT.size());
    m_rendererBase.setShaderHookType(Z3DRendererBase::ShaderHookType::WeightedAverageInit);
    size_t n = std::min(imageColorTexList.size(), imageDepthTexList.size());
    for (size_t i = 0; i < n; ++i) {
      m_textureCopyRenderer.setColorTexture(imageColorTexList[i]);
      m_textureCopyRenderer.setDepthTexture(imageDepthTexList[i]);
      m_rendererBase.render(eye, m_textureCopyRenderer);
    }
  }

  if (depthTexture) {
    waRT.detach(GL_DEPTH_ATTACHMENT);
    waRT.isFBOComplete();
  }
  waRT.release();

  if (depthTexture) {
    glDepthMask(GL_TRUE);
  }
  glBlendFunc(GL_ONE, GL_ZERO);
  glDisable(GL_BLEND);

  for (auto filter : filters) {
    filter->setShaderHookType(Z3DRendererBase::ShaderHookType::Normal);
  }
  if (!imageColorTexList.empty()) {
    m_rendererBase.setShaderHookType(Z3DRendererBase::ShaderHookType::Normal);
  }

  // The final resolve shader writes gl_FragDepth. Ensure the resolved depth is
  // recorded in the output depth buffer even when we disabled depth testing for
  // accumulation due to missing opaque depth.
  glEnable(GL_DEPTH_TEST);
  glDepthMask(GL_TRUE);
  glDepthFunc(GL_ALWAYS);

  glTarget->bind();
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
  m_waFinalShader->bind();
  m_waFinalShader->bindTexture("ColorTex0", g_accumulationTexId[0]);
  m_waFinalShader->bindTexture("ColorTex1", g_accumulationTexId[1]);

  const glm::uvec2 waSize = waRT.size();
  const glm::vec2 waScreenDimRcp(waSize.x > 0u ? 1.f / static_cast<float>(waSize.x) : 0.f,
                                 waSize.y > 0u ? 1.f / static_cast<float>(waSize.y) : 0.f);
  m_waFinalShader->setScreenDimRCPUniform(waScreenDimRcp);

  CHECK(m_screenQuadVAO != nullptr);
  CHECK(m_waFinalShader != nullptr);
  Z3DPrimitiveRenderer::renderScreenQuad(*m_screenQuadVAO, *m_waFinalShader);
  m_waFinalShader->release();
  glTarget->release();

  glDepthFunc(GL_LESS);
  glEnable(GL_DEPTH_TEST);
}

void Z3DCompositor::renderTransparentWB(const std::vector<Z3DBoundedFilter*>& filters,
                                        Z3DScratchResourcePool::RenderTargetLease& targetLease,
                                        Z3DEye eye,
                                        Z3DTexture* depthTexture,
                                        const std::vector<const Z3DTexture*>& imageColorTexList,
                                        const std::vector<const Z3DTexture*>& imageDepthTexList)
{
  auto* glTarget = targetLease.renderTarget;
  CHECK(glTarget != nullptr);
  const auto targetSize = glTarget->size();

  auto& wbLease = ensureWBRenderTarget(targetSize);
  auto& wbRT = *wbLease.renderTarget;
  if (depthTexture) {
    wbRT.attachTextureToFBO(depthTexture, GL_DEPTH_ATTACHMENT, false);
    wbRT.isFBOComplete();
  }

  const Z3DTexture* g_accumulationTexId[2];
  g_accumulationTexId[0] = wbRT.attachment(GL_COLOR_ATTACHMENT0);
  g_accumulationTexId[1] = wbRT.attachment(GL_COLOR_ATTACHMENT1);
  const GLenum g_drawBuffers[] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1};

  if (depthTexture) {
    glDepthMask(GL_FALSE);
  } else {
    glDisable(GL_DEPTH_TEST);
  }

  wbRT.bind();
  glDrawBuffers(2, g_drawBuffers);

  float clearColorZero[4] = {0.f, 0.f, 0.f, 0.f};
  float clearColorOne[4] = {1.f, 1.f, 1.f, 1.f};
  glClearBufferfv(GL_COLOR, 0, clearColorZero);
  glClearBufferfv(GL_COLOR, 1, clearColorOne);
  /* GL path: no Vulkan load/store configuration needed here */

  glEnable(GL_BLEND);
  glBlendEquation(GL_FUNC_ADD);
  glBlendFunci(0, GL_ONE, GL_ONE);
  glBlendFunci(1, GL_ZERO, GL_ONE_MINUS_SRC_COLOR);

  for (auto filter : filters) {
    filter->setViewport(wbRT.size());
    filter->setShaderHookType(Z3DRendererBase::ShaderHookType::WeightedBlendedInit);
    filter->renderTransparent(eye);
  }
  if (!imageColorTexList.empty()) {
    setViewport(wbRT.size());
    m_rendererBase.setShaderHookType(Z3DRendererBase::ShaderHookType::WeightedBlendedInit);
    size_t n = std::min(imageColorTexList.size(), imageDepthTexList.size());
    for (size_t i = 0; i < n; ++i) {
      m_textureCopyRenderer.setColorTexture(imageColorTexList[i]);
      m_textureCopyRenderer.setDepthTexture(imageDepthTexList[i]);
      m_rendererBase.render(eye, m_textureCopyRenderer);
    }
  }

  if (depthTexture) {
    wbRT.detach(GL_DEPTH_ATTACHMENT);
    wbRT.isFBOComplete();
  }
  wbRT.release();

  glBlendFunc(GL_ONE, GL_ZERO);
  glDisable(GL_BLEND);

  if (depthTexture) {
    glDepthMask(GL_TRUE);
  }

  for (auto filter : filters) {
    filter->setShaderHookType(Z3DRendererBase::ShaderHookType::Normal);
  }
  if (!imageColorTexList.empty()) {
    m_rendererBase.setShaderHookType(Z3DRendererBase::ShaderHookType::Normal);
  }

  // Like WA/DDP, the WB resolve shader writes gl_FragDepth. Enable depth writes
  // so downstream overlays can depth-test against the resolved transparent
  // content, even when we disabled depth testing during accumulation.
  glEnable(GL_DEPTH_TEST);
  glDepthMask(GL_TRUE);
  glDepthFunc(GL_ALWAYS);

  glTarget->bind();
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
  m_wbFinalShader->bind();
  m_wbFinalShader->bindTexture("ColorTex0", g_accumulationTexId[0]);
  m_wbFinalShader->bindTexture("ColorTex1", g_accumulationTexId[1]);

  const glm::uvec2 wbSize = wbRT.size();
  const glm::vec2 wbScreenDimRcp(wbSize.x > 0u ? 1.f / static_cast<float>(wbSize.x) : 0.f,
                                 wbSize.y > 0u ? 1.f / static_cast<float>(wbSize.y) : 0.f);
  m_wbFinalShader->setScreenDimRCPUniform(wbScreenDimRcp);
  const float nearClip = m_rendererBase.viewState().nearClip;
  const float farClip = m_rendererBase.viewState().farClip;
  const float clipDenom = std::max(farClip - nearClip, 1e-6f);
  const float a = farClip * nearClip / clipDenom;
  const float b = 0.5f * (farClip + nearClip) / clipDenom + 0.5f;
  m_wbFinalShader->setUniform("ze_to_zw_a", a);
  m_wbFinalShader->setUniform("ze_to_zw_b", b);
  m_wbFinalShader->setUniform("weighted_blended_depth_scale", m_rendererBase.sceneState().weightedBlendedDepthScale);

  CHECK(m_screenQuadVAO != nullptr);
  CHECK(m_wbFinalShader != nullptr);
  Z3DPrimitiveRenderer::renderScreenQuad(*m_screenQuadVAO, *m_wbFinalShader);
  m_wbFinalShader->release();
  glTarget->release();

  glDepthFunc(GL_LESS);
  glEnable(GL_DEPTH_TEST);
}

void Z3DCompositor::ensurePickingTarget(const glm::uvec2& size)
{
  if (size.x == 0u || size.y == 0u) {
    return;
  }
  if (!m_pickingTargetLease.renderTarget || m_pickingTargetLease.renderTarget->size() != size) {
    m_pickingTargetLease.release();
    m_rendererBase.acquirePersistentTempRenderTarget2D(m_pickingTargetLease,
                                                       size,
                                                       ScratchFormat::RGBA8,
                                                       ScratchFormat::Depth32F);
  }

  CHECK(m_pickingTargetLease.renderTarget != nullptr);
  m_globalParameters.pickingManager.setPickingTarget(*m_pickingTargetLease.renderTarget);
}

void Z3DCompositor::ensurePickingTargetVulkan(const glm::uvec2& size)
{
  if (size.x == 0u || size.y == 0u) {
    return;
  }
  const bool needAcquire = !m_pickingTargetLease || m_pickingTargetLease.descriptor.size != size ||
                           m_pickingTargetLease.backend != RenderBackend::Vulkan;
  if (needAcquire) {
    m_pickingTargetLease.release();
    Z3DRenderGlobalState::instance().scratchPool().reclaimVulkanScratchMemory(
      Z3DScratchResourcePool::VulkanScratchReclaimMode::WaitForIdle);
    m_rendererBase.acquirePersistentTempRenderTarget2D(m_pickingTargetLease,
                                                       size,
                                                       ScratchFormat::RGBA8,
                                                       ScratchFormat::Depth32F);
  }
  CHECK(m_pickingTargetLease);
}

Z3DScratchResourcePool::RenderTargetLease& Z3DCompositor::ensureDDPRenderTarget(const glm::uvec2& size)
{
  CHECK_GT(size.x, 0u);
  CHECK_GT(size.y, 0u);
  const bool wantVulkan = m_rendererBase.activeBackend() == RenderBackend::Vulkan;
  if (wantVulkan) {
    CHECK(!m_ddpRTLease || m_ddpRTLease.backend == RenderBackend::Vulkan)
      << "Persistent dual-depth-peel render target must be Vulkan-backed when Vulkan backend is active.";
  }
  const bool needAcquire =
    !m_ddpRTLease || (wantVulkan ? (m_ddpRTLease.descriptor.size != size)
                                 : (!m_ddpRTLease.renderTarget || m_ddpRTLease.renderTarget->size() != size));
  if (needAcquire) {
    m_ddpRTLease.release();
    if (wantVulkan) {
      Z3DRenderGlobalState::instance().scratchPool().reclaimVulkanScratchMemory(
        Z3DScratchResourcePool::VulkanScratchReclaimMode::WaitForIdle);
    }
    m_rendererBase.acquirePersistentDualDepthPeelRenderTarget(m_ddpRTLease, size);
  }
  CHECK(m_ddpRTLease);
  return m_ddpRTLease;
}

Z3DScratchResourcePool::RenderTargetLease& Z3DCompositor::ensureWARenderTarget(const glm::uvec2& size)
{
  CHECK_GT(size.x, 0u);
  CHECK_GT(size.y, 0u);
  const bool wantVulkan = m_rendererBase.activeBackend() == RenderBackend::Vulkan;
  if (wantVulkan) {
    CHECK(!m_waRTLease || m_waRTLease.backend == RenderBackend::Vulkan)
      << "Persistent weighted-average render target must be Vulkan-backed when Vulkan backend is active.";
  }
  const bool needAcquire =
    !m_waRTLease || (wantVulkan ? (m_waRTLease.descriptor.size != size)
                                : (!m_waRTLease.renderTarget || m_waRTLease.renderTarget->size() != size));
  if (needAcquire) {
    m_waRTLease.release();
    if (wantVulkan) {
      Z3DRenderGlobalState::instance().scratchPool().reclaimVulkanScratchMemory(
        Z3DScratchResourcePool::VulkanScratchReclaimMode::WaitForIdle);
    }
    m_rendererBase.acquirePersistentWeightedAverageRenderTarget(m_waRTLease, size);
  }
  CHECK(m_waRTLease);
  return m_waRTLease;
}

Z3DScratchResourcePool::RenderTargetLease& Z3DCompositor::ensureWBRenderTarget(const glm::uvec2& size)
{
  CHECK_GT(size.x, 0u);
  CHECK_GT(size.y, 0u);
  const bool wantVulkan = m_rendererBase.activeBackend() == RenderBackend::Vulkan;
  if (wantVulkan) {
    CHECK(!m_wbRTLease || m_wbRTLease.backend == RenderBackend::Vulkan)
      << "Persistent weighted-blended render target must be Vulkan-backed when Vulkan backend is active.";
  }
  const bool needAcquire =
    !m_wbRTLease || (wantVulkan ? (m_wbRTLease.descriptor.size != size)
                                : (!m_wbRTLease.renderTarget || m_wbRTLease.renderTarget->size() != size));
  if (needAcquire) {
    m_wbRTLease.release();
    if (wantVulkan) {
      Z3DRenderGlobalState::instance().scratchPool().reclaimVulkanScratchMemory(
        Z3DScratchResourcePool::VulkanScratchReclaimMode::WaitForIdle);
    }
    m_rendererBase.acquirePersistentWeightedBlendedRenderTarget(m_wbRTLease, size);
  }
  CHECK(m_wbRTLease);
  return m_wbRTLease;
}

void Z3DCompositor::renderTransparentDDPVulkan(const std::vector<Z3DBoundedFilter*>& filters,
                                               Z3DScratchResourcePool::RenderTargetLease& targetLease,
                                               Z3DEye eye,
                                               AttachmentHandle depthAttachmentHandle,
                                               const std::vector<Z3DCompositorImageLayer>& imageLayers,
                                               bool clearResolveTarget,
                                               ZVulkanLinearScript& script)
{
  const glm::uvec2 targetSize = targetLease.descriptor.size;
  auto& ddpLease = ensureDDPRenderTarget(targetSize);
  CHECK(ddpLease.backend == RenderBackend::Vulkan);

  auto ddpBindings = m_rendererBase.prepareVulkanSurface(ddpLease);
  CHECK(ddpBindings.colorHandles.size() >= 8 && ddpBindings.surface.colorAttachments.size() >= 8)
    << "Dual depth peeling Vulkan target incomplete.";

  auto makeHandle = [&](size_t idx) {
    return ddpBindings.colorHandles.at(idx);
  };

  std::array<AttachmentHandle, 2> depthPing{makeHandle(0), makeHandle(3)};
  std::array<AttachmentHandle, 2> frontPing{makeHandle(1), makeHandle(4)};
  std::array<AttachmentHandle, 2> backTempPing{makeHandle(2), makeHandle(5)};
  AttachmentHandle backBlend = makeHandle(6);
  AttachmentHandle depthTextureHandle = makeHandle(7);

  auto applyDepthAttachment = [&](RendererFrameState::ActiveSurface& surface, LoadOp loadOp) {
    // GL behavior: when an opaque depth attachment exists, DDP is depth-tested
    // against it (depth writes disabled). When no opaque depth is provided, GL
    // disables depth test entirely. For Vulkan, we keep depth test enabled in
    // DDP pipelines and emulate the "no opaque depth" case by clearing the
    // internal depth attachment to 1.0 so all fragments pass.
    const LoadOp effectiveLoadOp = depthAttachmentHandle.valid() ? loadOp : LoadOp::Clear;
    if (depthAttachmentHandle.valid()) {
      AttachmentDesc desc;
      desc.handle = depthAttachmentHandle;
      desc.loadOp = loadOp;
      desc.storeOp = StoreOp::Store;
      desc.finalUse = AttachmentFinalUse::RenderTarget;
      desc.clearValue.depth = 1.0f;
      surface.depthAttachment = desc;
    } else if (surface.depthAttachment) {
      surface.depthAttachment->loadOp = effectiveLoadOp;
      surface.depthAttachment->storeOp = StoreOp::Store;
      surface.depthAttachment->clearValue.depth = 1.0f;
    }
  };

  auto resetHooks = [&]() {
    for (auto* filter : filters) {
      if (!filter) {
        continue;
      }
      filter->setShaderHookType(Z3DRendererBase::ShaderHookType::Normal);
      filter->setShaderHookParaDDPDepthBlenderAttachment({});
      filter->setShaderHookParaDDPFrontBlenderAttachment({});
    }
    m_rendererBase.setShaderHookType(Z3DRendererBase::ShaderHookType::Normal);
    // Clear hook parameters as well; leaving stale DDP handles set can cause
    // appendBatch() to auto-inject external image uses that accidentally
    // reference current pass attachments (read-while-write feedback).
    m_rendererBase.setShaderHookParaDDPDepthBlenderAttachment({});
    m_rendererBase.setShaderHookParaDDPFrontBlenderAttachment({});
  };

  // Generic helper to render a filter into a specific surface and ingest its
  // recorded batches back into the compositor renderer. This unifies the
  // repeated patterns across DDP/WA/WB and makes the flow easier to follow.
  // When propagateHookPara is true, shader-hook parameters produced by the
  // source renderer (e.g., DDP attachment bindings) are copied into the
  // compositor renderer prior to ingesting batches.

  const glm::vec4 depthClear(-1.0f, -1.0f, 0.0f, 0.0f);
  const glm::vec4 depthTexClear(-1.0f, 0.0f, 0.0f, 0.0f);
  const glm::vec4 zeroClear(0.0f);

  // In Vulkan, fragment outputs at locations 0..N map to the active surface's
  // color attachments in order.
  //
  // DDP init shaders write 2 outputs:
  //  - location 0: depth blender (RG32F, stores -minDepth/maxDepth)
  //  - location 1: depthTex (R32F, stores -minDepth for final gl_FragDepth)
  //
  // We also attach the ping0 front/back temp targets and the back-blend buffer
  // here so we can clear them at the start of the frame (GL behavior).
  RendererFrameState::ActiveSurface initSurface;
  initSurface.colorAttachments.push_back(ddpBindings.surface.colorAttachments[0]);
  initSurface.colorAttachments.push_back(ddpBindings.surface.colorAttachments[7]); // depthTex
  initSurface.colorAttachments.push_back(ddpBindings.surface.colorAttachments[1]); // front ping0 (clear only)
  initSurface.colorAttachments.push_back(ddpBindings.surface.colorAttachments[2]); // back temp ping0 (clear only)
  // Also include the back-blend accumulation buffer (attachment 6) so we can
  // clear it at the start of the frame. Otherwise it would retain values across
  // frames and cause trail/ghost artifacts.
  initSurface.colorAttachments.push_back(ddpBindings.surface.colorAttachments[6]);
  initSurface.depthAttachment = ddpBindings.surface.depthAttachment;
  VLOG(1) << "DDP init: color handles="
          << (ddpBindings.colorHandles.size() >= 8 ? "8" : std::to_string(ddpBindings.colorHandles.size()))
          << " [0]=" << (ddpBindings.colorHandles.size() > 0 ? ddpBindings.colorHandles[0].id : 0)
          << " [1]=" << (ddpBindings.colorHandles.size() > 1 ? ddpBindings.colorHandles[1].id : 0)
          << " [2]=" << (ddpBindings.colorHandles.size() > 2 ? ddpBindings.colorHandles[2].id : 0)
          << " [3]=" << (ddpBindings.colorHandles.size() > 3 ? ddpBindings.colorHandles[3].id : 0)
          << " [4]=" << (ddpBindings.colorHandles.size() > 4 ? ddpBindings.colorHandles[4].id : 0)
          << " [5]=" << (ddpBindings.colorHandles.size() > 5 ? ddpBindings.colorHandles[5].id : 0)
          << " [6]=" << (ddpBindings.colorHandles.size() > 6 ? ddpBindings.colorHandles[6].id : 0)
          << " [7]=" << (ddpBindings.colorHandles.size() > 7 ? ddpBindings.colorHandles[7].id : 0);
  for (size_t i = 0; i < initSurface.colorAttachments.size(); ++i) {
    auto& attachment = initSurface.colorAttachments[i];
    attachment.storeOp = StoreOp::Store;
    attachment.loadOp = LoadOp::Clear;
    attachment.finalUse = AttachmentFinalUse::Sampled;
    // Clear rule:
    //  - depth blender (slot 0) uses depthClear (-1, -1)
    //  - depthTex (slot 1) uses depthTexClear (-1)
    //  - everything else cleared to 0
    if (i == 0) {
      attachment.clearValue.color = depthClear;
    } else if (i == 1) {
      attachment.clearValue.color = depthTexClear;
    } else {
      attachment.clearValue.color = zeroClear;
    }
  }
  applyDepthAttachment(initSurface, LoadOp::Load);

  // Use unified helper for DDP (propagate hook params for sampler bindings)

  auto* vulkanBackend = dynamic_cast<Z3DRendererVulkanBackend*>(m_rendererBase.backend());
  CHECK(vulkanBackend != nullptr) << "renderTransparentDDPVulkan requires Vulkan backend";

  // Geometry init step must see DualDepthPeelingInit on the compositor renderer
  m_rendererBase.setShaderHookType(Z3DRendererBase::ShaderHookType::DualDepthPeelingInit);
  const auto segInit = script.raster("transparency_ddp_init", {}, [&]() {
    m_rendererBase.setActiveSurfaceWithLoadStore(initSurface, Z3DRendererBase::Preserve);
    for (auto* filter : filters) {
      if (!filter) {
        continue;
      }
      filter->setShaderHookType(Z3DRendererBase::ShaderHookType::DualDepthPeelingInit);
      recordTransparentFilterBatchesToSurfaceUnified(m_rendererBase,
                                                     filter,
                                                     initSurface,
                                                     eye,
                                                     /*propagateHookPara=*/true);
    }
    if (!imageLayers.empty()) {
      m_rendererBase.setShaderHookType(Z3DRendererBase::ShaderHookType::DualDepthPeelingInit);
      for (const auto& layer : imageLayers) {
        const auto& colorDesc = layer.colorAttachment;
        const auto& depthDesc = layer.depthAttachment;
        if (colorDesc.handle.backend != RenderBackend::Vulkan || !colorDesc.handle.valid()) {
          continue;
        }
        if (depthDesc.handle.backend != RenderBackend::Vulkan || !depthDesc.handle.valid()) {
          continue;
        }
        // Image OIT init copy: do not flip
        m_textureCopyRenderer.setFlipY(false);
        m_textureCopyRenderer.setSourceAttachments(colorDesc.handle, depthDesc.handle);
        m_rendererBase.renderVulkan(eye, m_textureCopyRenderer);
      }
    }
  });

  resetHooks();

  const size_t kMaxPasses = static_cast<size_t>(std::max(1, FLAGS_atlas_ddp_max_passes));
  // Track the last ping buffer written by the orchestrated peel to feed the
  // final composite pass. Initialize to the same parity as the first peel
  // (pass=1 -> currId=1), and update inside the drawPass.
  size_t finalPingIdx = 1;

  const bool useIndirectCount = vulkanBackend->ddpIndirectCountEnabled();

  auto drawPass = [&](uint32_t pass, ZVulkanLinearScript::SegmentHandle deps) -> ZVulkanLinearScript::SegmentHandle {
    // Map orchestrator pass [0..N) to compositor's previous logic [1..N]
    const size_t logicalPass = static_cast<size_t>(pass) + 1u;
    const size_t currIdLocal = logicalPass % 2;
    finalPingIdx = currIdLocal;
    const size_t prevId = 1 - currIdLocal;
    const size_t bufOffset = currIdLocal * 3;

    // Route locations 0/1/2 to the active ping attachments for this pass.
    RendererFrameState::ActiveSurface peelSurface;
    peelSurface.colorAttachments.push_back(ddpBindings.surface.colorAttachments[bufOffset + 0]);
    peelSurface.colorAttachments.push_back(ddpBindings.surface.colorAttachments[bufOffset + 1]);
    peelSurface.colorAttachments.push_back(ddpBindings.surface.colorAttachments[bufOffset + 2]);
    peelSurface.depthAttachment = ddpBindings.surface.depthAttachment;
    for (size_t i = 0; i < peelSurface.colorAttachments.size(); ++i) {
      auto& attachment = peelSurface.colorAttachments[i];
      attachment.storeOp = StoreOp::Store;
      attachment.finalUse = AttachmentFinalUse::Sampled;
      attachment.clearValue.color = zeroClear;
      attachment.loadOp = LoadOp::Load;
    }
    peelSurface.colorAttachments[0].loadOp = LoadOp::Clear;
    peelSurface.colorAttachments[0].clearValue.color = depthClear;
    peelSurface.colorAttachments[1].loadOp = LoadOp::Clear;
    peelSurface.colorAttachments[1].clearValue.color = zeroClear;
    peelSurface.colorAttachments[2].loadOp = LoadOp::Clear;
    peelSurface.colorAttachments[2].clearValue.color = zeroClear;
    applyDepthAttachment(peelSurface, LoadOp::Load);
    const auto segPeel = script.raster("transparency_ddp_peel", {deps}, [&]() {
      m_rendererBase.setActiveSurfaceWithLoadStore(peelSurface, Z3DRendererBase::Preserve);

      // Geometry peel step must see DualDepthPeelingPeel on the compositor renderer.
      m_rendererBase.setShaderHookType(Z3DRendererBase::ShaderHookType::DualDepthPeelingPeel);

      // Carry the previous ping's front blender into the current ping before we
      // emit any peel geometry. This preserves GL DDP semantics even when
      // Vulkan uses indirect-count gating and may skip geometry draws once the
      // peel converges (count=0). Without this, the per-pass clear would erase
      // the accumulated front color when draws are skipped.
      {
        TextureDualPeelPayload payload;
        payload.stage = TextureDualPeelPayload::Stage::Carry;
        payload.depthAttachment = depthPing[prevId];
        payload.frontAttachment = frontPing[prevId];

        RenderBatch batch;
        batch.eye = eye;
        const glm::uvec4 viewport = m_rendererBase.frameState().viewport;
        batch.pass.viewport.origin = glm::vec2(static_cast<float>(viewport.x), static_cast<float>(viewport.y));
        batch.pass.viewport.extent = glm::vec2(static_cast<float>(viewport.z), static_cast<float>(viewport.w));
        batch.pass.viewport.minDepth = 0.0f;
        batch.pass.viewport.maxDepth = 1.0f;
        batch.pass.colorAttachments = m_rendererBase.frameState().activeSurface.colorAttachments;
        batch.pass.depthAttachment = m_rendererBase.frameState().activeSurface.depthAttachment;
        batch.pass.externalImageUses.push_back(
          {payload.depthAttachment, ExternalImageUseKind::SampledRead, ExternalImageAspectHint::Color});
        batch.pass.externalImageUses.push_back(
          {payload.frontAttachment, ExternalImageUseKind::SampledRead, ExternalImageAspectHint::Color});
        batch.draw.topology = PrimitiveTopology::TriangleStrip;
        batch.draw.vertexCount = 4;
        batch.draw.indexCount = 0;
        batch.geometry = payload;
        m_rendererBase.appendBatch(std::move(batch));
      }

      for (auto* filter : filters) {
        if (!filter) {
          continue;
        }
        filter->setShaderHookType(Z3DRendererBase::ShaderHookType::DualDepthPeelingPeel);
        filter->setShaderHookParaDDPDepthBlenderAttachment(depthPing[prevId]);
        filter->setShaderHookParaDDPFrontBlenderAttachment(frontPing[prevId]);
        recordTransparentFilterBatchesToSurfaceUnified(m_rendererBase,
                                                       filter,
                                                       peelSurface,
                                                       eye,
                                                       /*propagateHookPara=*/true);
      }
      if (!imageLayers.empty()) {
        m_rendererBase.setShaderHookType(Z3DRendererBase::ShaderHookType::DualDepthPeelingPeel);
        // Ensure image peel path samples the correct blender textures from the previous ping (prevId).
        m_rendererBase.setShaderHookParaDDPDepthBlenderAttachment(depthPing[prevId]);
        m_rendererBase.setShaderHookParaDDPFrontBlenderAttachment(frontPing[prevId]);
        for (const auto& layer : imageLayers) {
          const auto& colorDesc = layer.colorAttachment;
          const auto& depthDesc = layer.depthAttachment;
          if (colorDesc.handle.backend != RenderBackend::Vulkan || !colorDesc.handle.valid()) {
            continue;
          }
          if (depthDesc.handle.backend != RenderBackend::Vulkan || !depthDesc.handle.valid()) {
            continue;
          }
          // Image OIT peel: do not flip
          m_textureCopyRenderer.setFlipY(false);
          m_textureCopyRenderer.setSourceAttachments(colorDesc.handle, depthDesc.handle);
          m_rendererBase.renderVulkan(eye, m_textureCopyRenderer);
        }
      }
    });

    resetHooks();

    // Accumulate back colors for this pass immediately after peel.
    RendererFrameState::ActiveSurface blendSurface;
    blendSurface.colorAttachments.push_back(ddpBindings.surface.colorAttachments[6]);
    if (!blendSurface.colorAttachments.empty()) {
      blendSurface.colorAttachments[0].loadOp = LoadOp::Load;
      blendSurface.colorAttachments[0].storeOp = StoreOp::Store;
      blendSurface.colorAttachments[0].finalUse = AttachmentFinalUse::Sampled;
      blendSurface.colorAttachments[0].clearValue.color = zeroClear;
    }

    const glm::vec2 screenDimRcp(targetSize.x > 0u ? 1.f / static_cast<float>(targetSize.x) : 0.f,
                                 targetSize.y > 0u ? 1.f / static_cast<float>(targetSize.y) : 0.f);

    const auto segBlend = script.raster("transparency_ddp_blend", {segPeel}, [&]() {
      m_rendererBase.setActiveSurfaceWithLoadStore(blendSurface, Z3DRendererBase::Preserve);
      TextureDualPeelPayload payload;
      payload.stage = TextureDualPeelPayload::Stage::Blend;
      payload.tempAttachment = backTempPing[currIdLocal];
      payload.screenDimRcp = screenDimRcp;

      RenderBatch batch;
      batch.eye = eye;
      const glm::uvec4 viewport = m_rendererBase.frameState().viewport;
      batch.pass.viewport.origin = glm::vec2(static_cast<float>(viewport.x), static_cast<float>(viewport.y));
      batch.pass.viewport.extent = glm::vec2(static_cast<float>(viewport.z), static_cast<float>(viewport.w));
      batch.pass.viewport.minDepth = 0.0f;
      batch.pass.viewport.maxDepth = 1.0f;
      batch.pass.colorAttachments = m_rendererBase.frameState().activeSurface.colorAttachments;
      batch.pass.depthAttachment = std::nullopt;
      batch.pass.externalImageUses.push_back(
        {payload.tempAttachment, ExternalImageUseKind::SampledRead, ExternalImageAspectHint::Color});
      batch.draw.topology = PrimitiveTopology::TriangleStrip;
      batch.draw.vertexCount = 4;
      batch.draw.indexCount = 0;
      batch.geometry = payload;
      m_rendererBase.appendBatch(std::move(batch));
    });

    resetHooks();
    return segBlend;
  };

  const uint32_t totalOrchestratedPasses = static_cast<uint32_t>(kMaxPasses > 0 ? (kMaxPasses - 1) : 0);
  ZVulkanLinearScript::SegmentHandle segLast = segInit;

  auto recordDdpPeelPass = [&](uint32_t pass,
                               ZVulkanLinearScript::SegmentHandle dep,
                               bool firstPassInSubmission) -> ZVulkanLinearScript::SegmentHandle {
    const auto segReset =
      script.commands("transparency_ddp_reset", {dep}, [firstPassInSubmission](Z3DRendererVulkanBackend& be) {
        auto& cmd = be.commandBuffer();
        if (!firstPassInSubmission) {
          be.ddpBarrierComputeToTransfer(cmd);
        }
        be.ddpResetForPass(cmd, firstPassInSubmission);
        be.ddpBarrierTransferToFrag(cmd);
      });

    const auto segBlend = drawPass(pass, segReset);

    const bool useIndirectCountLocal = useIndirectCount;
    return script.commands("transparency_ddp_count", {segBlend}, [useIndirectCountLocal](Z3DRendererVulkanBackend& be) {
      auto& cmd = be.commandBuffer();
      be.ddpBarrierFragToCompute(cmd);
      be.ddpDispatchCountCompute(cmd);
      if (useIndirectCountLocal) {
        be.ddpBarrierComputeToIndirect(cmd);
      }
    });
  };

  if (FLAGS_atlas_vk_ddp_cpu_chunk_passes <= 0) {
    // Compatibility/debug path: record the whole peel loop in one submission.
    bool firstPassInSubmission = true;
    for (uint32_t pass = 0; pass < totalOrchestratedPasses; ++pass) {
      segLast = recordDdpPeelPass(pass, segLast, firstPassInSubmission);
      firstPassInSubmission = false;
    }
  } else {
    // Chunked path: keep each DDP submission bounded and read back the changed
    // flag between chunks so later chunks are not recorded after convergence.
    // When drawIndirectCount is supported, each chunk also uses device-side
    // gating to skip draws after convergence within that chunk.
    const uint32_t chunkPasses = static_cast<uint32_t>(std::max<int32_t>(1, FLAGS_atlas_vk_ddp_cpu_chunk_passes));
    uint32_t passBase = 0;
    while (passBase < totalOrchestratedPasses) {
      const uint32_t remaining = totalOrchestratedPasses - passBase;
      const uint32_t thisChunk = std::min(chunkPasses, remaining);
      const bool needReadback = (passBase + thisChunk < totalOrchestratedPasses);

      VLOG(1) << fmt::format("DDP Vulkan chunk: passBase={} chunk={} remaining={} needReadback={} indirectCount={}",
                             passBase,
                             thisChunk,
                             remaining,
                             needReadback,
                             useIndirectCount);

      ZVulkanLinearScript::SegmentHandle segChunkLast = segLast;
      const auto ddpChangedFlagSlot = script.makeSlot<ZVulkanBuffer*>();
      if (needReadback) {
        segChunkLast = script.preRecord("transparency_ddp_prime_changedflag",
                                        {segChunkLast},
                                        [ddpChangedFlagSlot](Z3DRendererVulkanBackend& be, Z3DRendererBase&) {
                                          ZVulkanBuffer* flag = be.ddpChangedFlagBufferObj();
                                          CHECK(flag != nullptr)
                                            << "DDP Vulkan: missing ddpChangedFlag buffer for CPU early-stop";
                                          ddpChangedFlagSlot.set(flag);
                                        });
      }

      for (uint32_t local = 0; local < thisChunk; ++local) {
        const uint32_t pass = passBase + local;
        const bool firstPassInSubmission = (local == 0);
        segChunkLast = recordDdpPeelPass(pass, segChunkLast, firstPassInSubmission);
      }

      segLast = segChunkLast;
      if (!needReadback) {
        passBase += thisChunk;
        continue;
      }

      const uint32_t changedFlag =
        script.readbackU32("vk_ddp_changedflag_readback", {segChunkLast}, ddpChangedFlagSlot, /*srcOffset=*/0);
      if (changedFlag == 0u) {
        VLOG(1) << fmt::format("DDP Vulkan chunk: converged at pass {}", passBase + thisChunk - 1);
        break;
      }

      passBase += thisChunk;
    }
  }

  auto outBindings = m_rendererBase.prepareVulkanSurface(targetLease);
  RendererFrameState::ActiveSurface outSurface = outBindings.surface;
  if (!outBindings.colorHandles.empty()) {
    VLOG(1) << "DDP final: out color handle=" << outBindings.colorHandles[0].id;
  }
  // Enforce single color attachment; keep depth so final pass can write gl_FragDepth
  if (outSurface.colorAttachments.size() > 1) {
    outSurface.colorAttachments.resize(1);
  }
  if (outSurface.depthAttachment) {
    outSurface.depthAttachment->loadOp = clearResolveTarget ? LoadOp::Clear : LoadOp::DontCare;
    outSurface.depthAttachment->storeOp = StoreOp::Store;
    outSurface.depthAttachment->clearValue.depth = 1.0f;
  }
  for (auto& attachment : outSurface.colorAttachments) {
    attachment.loadOp = clearResolveTarget ? LoadOp::Clear : LoadOp::Load;
    attachment.storeOp = StoreOp::Store;
    attachment.clearValue.color = glm::vec4(0.0f);
  }
  VLOG(1) << (clearResolveTarget ? "DDP final: Clear color for intermediate surface"
                                 : "DDP final: Load color to preserve background");

  const glm::vec2 screenDimRcp(targetSize.x > 0u ? 1.f / static_cast<float>(targetSize.x) : 0.f,
                               targetSize.y > 0u ? 1.f / static_cast<float>(targetSize.y) : 0.f);

  script.raster("transparency_ddp_final", {segLast}, [&]() {
    m_rendererBase.setActiveSurfaceWithLoadStore(outSurface, Z3DRendererBase::Preserve);
    TextureDualPeelPayload payload;
    payload.stage = TextureDualPeelPayload::Stage::Final;
    // Use the last ping written by orchestrated peel passes
    payload.frontAttachment = frontPing[finalPingIdx];
    payload.backAttachment = backBlend;
    payload.depthAttachment = depthTextureHandle;
    payload.screenDimRcp = screenDimRcp;

    RenderBatch batch;
    batch.eye = eye;
    const glm::uvec4 viewport = m_rendererBase.frameState().viewport;
    batch.pass.viewport.origin = glm::vec2(static_cast<float>(viewport.x), static_cast<float>(viewport.y));
    batch.pass.viewport.extent = glm::vec2(static_cast<float>(viewport.z), static_cast<float>(viewport.w));
    batch.pass.viewport.minDepth = 0.0f;
    batch.pass.viewport.maxDepth = 1.0f;
    batch.pass.colorAttachments = m_rendererBase.frameState().activeSurface.colorAttachments;
    batch.pass.depthAttachment = m_rendererBase.frameState().activeSurface.depthAttachment;
    batch.pass.externalImageUses.push_back(
      {payload.frontAttachment, ExternalImageUseKind::SampledRead, ExternalImageAspectHint::Color});
    batch.pass.externalImageUses.push_back(
      {payload.backAttachment, ExternalImageUseKind::SampledRead, ExternalImageAspectHint::Color});
    batch.pass.externalImageUses.push_back(
      {payload.depthAttachment, ExternalImageUseKind::SampledRead, ExternalImageAspectHint::Color});
    batch.draw.topology = PrimitiveTopology::TriangleStrip;
    batch.draw.vertexCount = 4;
    batch.draw.indexCount = 0;
    batch.geometry = payload;
    m_rendererBase.appendBatch(std::move(batch));
  });

  resetHooks();
}

void Z3DCompositor::renderTransparentPPLLVulkan(const std::vector<Z3DBoundedFilter*>& filters,
                                                Z3DScratchResourcePool::RenderTargetLease& targetLease,
                                                Z3DEye eye,
                                                AttachmentHandle depthAttachmentHandle,
                                                const std::vector<Z3DCompositorImageLayer>& imageLayers,
                                                bool clearResolveTarget,
                                                ZVulkanLinearScript& script)
{
  auto* vulkanBackend = dynamic_cast<Z3DRendererVulkanBackend*>(m_rendererBase.backend());
  CHECK(vulkanBackend != nullptr) << "renderTransparentPPLLVulkan requires Vulkan backend";
  CHECK(vulkanBackend->supportsFragmentStoresAndAtomics()) << "PPLL requires fragmentStoresAndAtomics support";

  const glm::uvec2 targetSize = targetLease.descriptor.size;
  CHECK_GT(targetSize.x, 0u);
  CHECK_GT(targetSize.y, 0u);
  const glm::uvec4 ppllViewport(0u, 0u, targetSize.x, targetSize.y);
  CHECK(m_rendererBase.frameState().viewport.z == targetSize.x &&
        m_rendererBase.frameState().viewport.w == targetSize.y)
    << fmt::format("PPLL expected compositor viewport to match target surface size (viewport={}x{} target={}x{})",
                   m_rendererBase.frameState().viewport.z,
                   m_rendererBase.frameState().viewport.w,
                   targetSize.x,
                   targetSize.y);

  auto resetHooks = [&]() {
    for (auto* filter : filters) {
      if (!filter) {
        continue;
      }
      filter->setShaderHookType(Z3DRendererBase::ShaderHookType::Normal);
    }
    m_rendererBase.setShaderHookType(Z3DRendererBase::ShaderHookType::Normal);
  };

  // Resolve target surface (color + optional depth)
  auto outBindings = m_rendererBase.prepareVulkanSurface(targetLease);
  RendererFrameState::ActiveSurface outSurface = outBindings.surface;

  // Enforce single color attachment (matches other resolve passes).
  if (outSurface.colorAttachments.size() > 1) {
    outSurface.colorAttachments.resize(1);
  }
  CHECK(!outSurface.colorAttachments.empty()) << "PPLL resolve requires a color attachment";

  // Depth-only surface for geometry count/store passes (no color attachments).
  // Depth is used only for depth testing against opaque, and is not written.
  RendererFrameState::ActiveSurface depthOnlySurface{};
  {
    if (depthAttachmentHandle.valid()) {
      AttachmentDesc desc;
      desc.handle = depthAttachmentHandle;
      desc.loadOp = LoadOp::Load;
      desc.storeOp = StoreOp::Store;
      desc.finalUse = AttachmentFinalUse::RenderTarget;
      desc.clearValue.depth = 1.0f;
      depthOnlySurface.depthAttachment = desc;
    } else if (outSurface.depthAttachment.has_value()) {
      depthOnlySurface.depthAttachment = outSurface.depthAttachment;
      depthOnlySurface.depthAttachment->loadOp = LoadOp::Clear;
      depthOnlySurface.depthAttachment->storeOp = StoreOp::Store;
      depthOnlySurface.depthAttachment->clearValue.depth = 1.0f;
      depthOnlySurface.depthAttachment->finalUse = AttachmentFinalUse::RenderTarget;
    } else {
      CHECK(false) << "PPLL Vulkan requires a depth attachment for count/store passes.";
    }
  }

  auto depthOnlySurfaceLoad = depthOnlySurface;
  if (depthOnlySurfaceLoad.depthAttachment) {
    depthOnlySurfaceLoad.depthAttachment->loadOp = LoadOp::Load;
    depthOnlySurfaceLoad.depthAttachment->storeOp = StoreOp::Store;
  }

  // ---------------------------------------------------------------------------
  // Capture transparent draw list once (filters + image layers), then replay it
  // for both the count and store submissions. This avoids re-enqueuing the same
  // batches twice (a measurable CPU cost for complex scenes).
  // ---------------------------------------------------------------------------
  auto transparentBatches = std::make_shared<RendererCPUState>();
  auto setPPLLShaderHookOnBatches = [transparentBatches](Z3DRendererBase::ShaderHookType hook) {
    CHECK(transparentBatches != nullptr);
    for (auto& batch : transparentBatches->batches) {
      CHECK(batch.shaderHook.captured) << "PPLL captured batch missing shader hook snapshot";
      batch.shaderHook.type = hook;
    }
  };
  {
    for (auto* filter : filters) {
      if (!filter) {
        continue;
      }
      captureTransparentFilterBatchesToUnifiedList(m_rendererBase,
                                                   filter,
                                                   depthOnlySurface,
                                                   eye,
                                                   /*propagateHookPara=*/false,
                                                   *transparentBatches);
    }

    if (!imageLayers.empty()) {
      const auto previousSurface = m_rendererBase.frameState().activeSurface;
      m_rendererBase.setActiveSurfaceWithLoadStore(depthOnlySurface, Z3DRendererBase::Preserve);
      m_rendererBase.resetCPUState();
      for (const auto& layer : imageLayers) {
        const auto& colorDesc = layer.colorAttachment;
        const auto& depthDesc = layer.depthAttachment;
        if (colorDesc.handle.backend != RenderBackend::Vulkan || !colorDesc.handle.valid()) {
          continue;
        }
        if (depthDesc.handle.backend != RenderBackend::Vulkan || !depthDesc.handle.valid()) {
          continue;
        }
        m_textureCopyRenderer.setFlipY(false);
        m_textureCopyRenderer.setSourceAttachments(colorDesc.handle, depthDesc.handle);
        m_rendererBase.renderVulkan(eye, m_textureCopyRenderer);
      }
      for (auto& batch : m_rendererBase.cpuState().batches) {
        transparentBatches->batches.push_back(std::move(batch));
      }
      transparentBatches->uniformBytesEstimate += m_rendererBase.cpuState().uniformBytesEstimate;
      m_rendererBase.resetCPUState();
      m_rendererBase.setActiveSurfaceWithLoadStore(previousSurface, Z3DRendererBase::Preserve);
    }
  }

  // ---------------------------------------------------------------------------
  // Submission A: count fragments + scan_local + readback block sums
  // ---------------------------------------------------------------------------
  std::vector<uint32_t> blockSums;
  uint32_t blockCount = 0u;

  {
    auto sumsBufSlot = script.makeSlot<ZVulkanBuffer*>();
    const auto segPrimeCount =
      script.preRecord("transparency_ppll_prime_count",
                       {},
                       [ppllViewport, sumsBufSlot](Z3DRendererVulkanBackend& be, Z3DRendererBase& /*renderer*/) {
                         be.primePPLLForCountPass(ppllViewport);
                         sumsBufSlot.set(be.ppllBlockSumsBufferObj());
                       });

    setPPLLShaderHookOnBatches(Z3DRendererBase::ShaderHookType::PerPixelFragmentListCount);

    const auto segResetCounts =
      script.commands("transparency_ppll_reset_counts", {segPrimeCount}, [](Z3DRendererVulkanBackend& be) {
        auto& cmd = be.commandBuffer();
        be.ppllResetCounts(cmd);
        be.ppllBarrierTransferToFrag(cmd);
      });

    const auto segCount = script.replay("transparency_ppll_count", {segResetCounts}, transparentBatches);

    const auto segScanLocal =
      script.commands("transparency_ppll_scan_local", {segCount}, [](Z3DRendererVulkanBackend& be) {
        auto& cmd = be.commandBuffer();
        be.ppllBarrierFragToCompute(cmd);
        be.ppllDispatchScanLocal(cmd);
      });

    const uint64_t pixelCount64 = static_cast<uint64_t>(ppllViewport.z) * static_cast<uint64_t>(ppllViewport.w);
    CHECK(pixelCount64 <= static_cast<uint64_t>(std::numeric_limits<uint32_t>::max()))
      << fmt::format("PPLL pixel count overflow: {}x{} = {}", ppllViewport.z, ppllViewport.w, pixelCount64);
    const uint32_t pixelCount = static_cast<uint32_t>(pixelCount64);
    blockCount = (pixelCount == 0u) ? 0u : ((pixelCount + 256u - 1u) / 256u);

    if (blockCount > 0u) {
      blockSums.resize(blockCount, 0u);
      const size_t bytes = static_cast<size_t>(blockCount) * sizeof(uint32_t);
      script.readbackBufferTo("vk_ppll_blocksums_readback",
                              {segScanLocal},
                              sumsBufSlot,
                              /*srcOffset=*/0,
                              blockSums.data(),
                              bytes);
    }
  }

  resetHooks();

  // CPU prefix sum over block sums (exclusive), producing block prefixes and total fragment count.
  std::vector<uint32_t> blockPrefixes;
  blockPrefixes.resize(blockCount, 0u);
  uint64_t totalFragments64 = 0u;
  uint32_t maxBlockSum = 0u;
  uint32_t maxBlockIdx = 0u;
  for (uint32_t i = 0u; i < blockCount; ++i) {
    CHECK(totalFragments64 <= static_cast<uint64_t>(std::numeric_limits<uint32_t>::max()))
      << fmt::format("PPLL fragment count overflow before block {}: total={}", i, totalFragments64);
    blockPrefixes[i] = static_cast<uint32_t>(totalFragments64);
    const uint32_t sum = blockSums[i];
    if (sum > maxBlockSum) {
      maxBlockSum = sum;
      maxBlockIdx = i;
    }
    totalFragments64 += static_cast<uint64_t>(sum);
  }
  CHECK(totalFragments64 <= static_cast<uint64_t>(std::numeric_limits<uint32_t>::max()))
    << fmt::format("PPLL fragment count overflow: totalFragments={}", totalFragments64);

  if (VLOG_IS_ON(1)) {
    const glm::uvec4 vp = ppllViewport;
    VLOG(1) << fmt::format(
      "PPLL stats: viewport={}x{} pixelCount={} blocks={} totalFragments={} maxBlockSum={} (block {})",
      vp.z,
      vp.w,
      static_cast<uint64_t>(vp.z) * static_cast<uint64_t>(vp.w),
      blockCount,
      totalFragments64,
      maxBlockSum,
      maxBlockIdx);
  }

  // ---------------------------------------------------------------------------
  // Submission B: scan_add + store fragments + resolve
  // ---------------------------------------------------------------------------
  const auto segPrimeStore =
    script.preRecord("transparency_ppll_prime_store",
                     {},
                     [ppllViewport, totalFragments64](Z3DRendererVulkanBackend& be, Z3DRendererBase& /*renderer*/) {
                       be.primePPLLForStorePass(ppllViewport, totalFragments64);
                     });

  // Store pass should load the opaque depth buffer (if present). When we capture
  // the transparent draw list above, it is recorded against depthOnlySurface,
  // which may Clear the implicit depth attachment (when no explicit depth handle
  // exists). Patch the load/store ops to match the store submission surface.
  if (depthOnlySurfaceLoad.depthAttachment.has_value()) {
    const uint64_t sharedDepthId = depthOnlySurfaceLoad.depthAttachment->handle.id;
    for (auto& batch : transparentBatches->batches) {
      if (!batch.pass.depthAttachment.has_value()) {
        continue;
      }
      if (batch.pass.depthAttachment->handle.id != sharedDepthId) {
        continue;
      }
      batch.pass.depthAttachment->loadOp = depthOnlySurfaceLoad.depthAttachment->loadOp;
      batch.pass.depthAttachment->storeOp = depthOnlySurfaceLoad.depthAttachment->storeOp;
    }
  }

  setPPLLShaderHookOnBatches(Z3DRendererBase::ShaderHookType::PerPixelFragmentListStore);
  {
    auto blockPrefixesShared = std::make_shared<std::vector<uint32_t>>(std::move(blockPrefixes));
    const auto segScanAdd =
      script.commands("transparency_ppll_scan_add",
                      {segPrimeStore},
                      [blockPrefixesShared](Z3DRendererVulkanBackend& be) {
                        auto& cmd = be.commandBuffer();

                        if (blockPrefixesShared && !blockPrefixesShared->empty()) {
                          be.ppllWriteBlockPrefixes(blockPrefixesShared->data(), blockPrefixesShared->size());
                        }

                        be.ppllDispatchScanAdd(cmd);
                        be.ppllBarrierComputeToFrag(cmd);

                        be.ppllResetCursors(cmd);
                        be.ppllBarrierTransferToFrag(cmd);
                      });

    // Store geometry fragments (replay transparent draw list).
    const auto segStore = script.replay("transparency_ppll_store", {segScanAdd}, transparentBatches);

    const auto segFragBarrier =
      script.commands("transparency_ppll_barrier_frag", {segStore}, [](Z3DRendererVulkanBackend& be) {
        auto& cmd = be.commandBuffer();
        be.ppllBarrierFragToFrag(cmd);
      });

    // Resolve into output surface (fullscreen composite).
    if (outSurface.depthAttachment) {
      outSurface.depthAttachment->loadOp = clearResolveTarget ? LoadOp::Clear : LoadOp::DontCare;
      outSurface.depthAttachment->storeOp = StoreOp::Store;
      outSurface.depthAttachment->clearValue.depth = 1.0f;
    }
    for (auto& attachment : outSurface.colorAttachments) {
      attachment.loadOp = clearResolveTarget ? LoadOp::Clear : LoadOp::Load;
      attachment.storeOp = StoreOp::Store;
      attachment.clearValue.color = glm::vec4(0.0f);
    }

    script.raster("transparency_ppll_resolve", {segFragBarrier}, [&]() {
      m_rendererBase.setActiveSurfaceWithLoadStore(outSurface, Z3DRendererBase::Preserve);
      TexturePPLLResolvePayload payload{};
      payload.opaqueDepthAttachment = depthAttachmentHandle;

      RenderBatch batch;
      batch.eye = eye;
      batch.pass.viewport.origin = glm::vec2(static_cast<float>(ppllViewport.x), static_cast<float>(ppllViewport.y));
      batch.pass.viewport.extent = glm::vec2(static_cast<float>(ppllViewport.z), static_cast<float>(ppllViewport.w));
      batch.pass.viewport.minDepth = 0.0f;
      batch.pass.viewport.maxDepth = 1.0f;
      batch.pass.colorAttachments = m_rendererBase.frameState().activeSurface.colorAttachments;
      batch.pass.depthAttachment = m_rendererBase.frameState().activeSurface.depthAttachment;
      if (payload.opaqueDepthAttachment.valid()) {
        batch.pass.externalImageUses.push_back(
          {payload.opaqueDepthAttachment, ExternalImageUseKind::SampledRead, ExternalImageAspectHint::Depth});
      }
      batch.draw.topology = PrimitiveTopology::TriangleStrip;
      batch.draw.vertexCount = 4;
      batch.draw.indexCount = 0;
      batch.geometry = payload;
      m_rendererBase.appendBatch(std::move(batch));
    });
  }

  resetHooks();
}

void Z3DCompositor::renderTransparentWAVulkan(const std::vector<Z3DBoundedFilter*>& filters,
                                              Z3DScratchResourcePool::RenderTargetLease& targetLease,
                                              Z3DEye eye,
                                              AttachmentHandle depthAttachmentHandle,
                                              const std::vector<Z3DCompositorImageLayer>& imageLayers,
                                              bool clearResolveTarget,
                                              ZVulkanLinearScript& script)
{
  const glm::uvec2 targetSize = targetLease.descriptor.size;
  auto& waLease = ensureWARenderTarget(targetSize);
  CHECK(waLease.backend == RenderBackend::Vulkan);

  auto waBindings = m_rendererBase.prepareVulkanSurface(waLease);
  CHECK(waBindings.colorHandles.size() >= 2 && waBindings.surface.colorAttachments.size() >= 2)
    << "Weighted average Vulkan target incomplete.";

  // Diagnostics: summarize WA init targets and inputs
  VLOG(1) << fmt::format("WA init: waLease colors[0]=0x{:x} colors[1]=0x{:x} size={}x{} depthParam=0x{:x}",
                         waBindings.colorHandles[0].id,
                         waBindings.colorHandles[1].id,
                         targetSize.x,
                         targetSize.y,
                         depthAttachmentHandle.valid() ? depthAttachmentHandle.id : 0ull);

  auto configureSurface = [&](RendererFrameState::ActiveSurface surface) {
    for (auto& attachment : surface.colorAttachments) {
      attachment.loadOp = LoadOp::Clear;
      attachment.storeOp = StoreOp::Store;
      attachment.finalUse = AttachmentFinalUse::Sampled;
      attachment.clearValue.color = glm::vec4(0.0f);
    }
    if (depthAttachmentHandle.valid()) {
      AttachmentDesc depthDesc;
      depthDesc.handle = depthAttachmentHandle;
      depthDesc.loadOp = LoadOp::Load;
      depthDesc.storeOp = StoreOp::Store;
      depthDesc.finalUse = AttachmentFinalUse::RenderTarget;
      depthDesc.clearValue.depth = 1.0f;
      surface.depthAttachment = depthDesc;
    } else {
      surface.depthAttachment.reset();
    }
    return surface;
  };

  auto resetHooks = [&]() {
    for (auto* filter : filters) {
      if (filter) {
        filter->setShaderHookType(Z3DRendererBase::ShaderHookType::Normal);
      }
    }
    m_rendererBase.setShaderHookType(Z3DRendererBase::ShaderHookType::Normal);
  };

  auto waInitSurface = configureSurface(waBindings.surface);

  // Geometry init step must see WeightedAverageInit on the compositor renderer
  m_rendererBase.setShaderHookType(Z3DRendererBase::ShaderHookType::WeightedAverageInit);
  const auto segInit = script.raster("transparency_wa_init", {}, [&]() {
    m_rendererBase.setActiveSurfaceWithLoadStore(waInitSurface, Z3DRendererBase::Preserve);
    // 1) Geometry filters
    for (auto* filter : filters) {
      if (!filter) {
        continue;
      }
      filter->setShaderHookType(Z3DRendererBase::ShaderHookType::WeightedAverageInit);
      recordFilterBatchesToSurfaceUnified(
        m_rendererBase,
        filter,
        waInitSurface,
        [&]() {
          filter->renderTransparent(eye);
        },
        /*propagateHookPara=*/false);
    }
    // 2) Image layers: sample from filters' transparent leases using WA image init copy
    if (!imageLayers.empty()) {
      m_rendererBase.setShaderHookType(Z3DRendererBase::ShaderHookType::WeightedAverageInit);
      VLOG(1) << fmt::format("WA init: merging {} image layer(s)", imageLayers.size());
      for (const auto& layer : imageLayers) {
        const auto& colorDesc = layer.colorAttachment;
        const auto& depthDesc = layer.depthAttachment;
        if (colorDesc.handle.backend != RenderBackend::Vulkan || !colorDesc.handle.valid()) {
          continue;
        }
        if (depthDesc.handle.backend != RenderBackend::Vulkan || !depthDesc.handle.valid()) {
          continue;
        }
        VLOG(1) << fmt::format("WA init: texture_copy layer color=0x{:x} depth=0x{:x}",
                               colorDesc.handle.id,
                               depthDesc.handle.id);
        // WA image init copy: do not flip
        m_textureCopyRenderer.setFlipY(false);
        m_textureCopyRenderer.setSourceAttachments(colorDesc.handle, depthDesc.handle);
        m_rendererBase.renderVulkan(eye, m_textureCopyRenderer);
      }
    }
  });

  resetHooks();

  auto outBindings = m_rendererBase.prepareVulkanSurface(targetLease);
  RendererFrameState::ActiveSurface outSurface = outBindings.surface;
  // Enforce single color attachment; keep depth so WA can write gl_FragDepth
  if (outSurface.colorAttachments.size() > 1) {
    outSurface.colorAttachments.resize(1);
  }
  if (outSurface.depthAttachment) {
    outSurface.depthAttachment->loadOp = clearResolveTarget ? LoadOp::Clear : LoadOp::DontCare;
    outSurface.depthAttachment->storeOp = StoreOp::Store;
    outSurface.depthAttachment->clearValue.depth = 1.0f;
  }
  for (auto& attachment : outSurface.colorAttachments) {
    attachment.loadOp = clearResolveTarget ? LoadOp::Clear : (m_showBackground.get() ? LoadOp::Load : LoadOp::Clear);
    attachment.storeOp = StoreOp::Store;
    attachment.clearValue.color = glm::vec4(0.0f);
  }

  const glm::vec2 screenDimRcp(targetSize.x > 0u ? 1.f / static_cast<float>(targetSize.x) : 0.f,
                               targetSize.y > 0u ? 1.f / static_cast<float>(targetSize.y) : 0.f);

  script.raster("transparency_wa_resolve", {segInit}, [&]() {
    m_rendererBase.setActiveSurfaceWithLoadStore(outSurface, Z3DRendererBase::Preserve);
    TextureWeightedAveragePayload payload;
    payload.accumulationAttachment = waBindings.colorHandles[0];
    payload.momentsAttachment = waBindings.colorHandles[1];
    payload.screenDimRcp = screenDimRcp;

    RenderBatch batch;
    batch.eye = eye;
    const glm::uvec4 viewport = m_rendererBase.frameState().viewport;

    batch.pass.viewport.origin = glm::vec2(static_cast<float>(viewport.x), static_cast<float>(viewport.y));
    batch.pass.viewport.extent = glm::vec2(static_cast<float>(viewport.z), static_cast<float>(viewport.w));
    batch.pass.viewport.minDepth = 0.0f;
    batch.pass.viewport.maxDepth = 1.0f;
    batch.pass.colorAttachments = m_rendererBase.frameState().activeSurface.colorAttachments;
    batch.pass.depthAttachment = m_rendererBase.frameState().activeSurface.depthAttachment;
    batch.pass.externalImageUses.push_back(
      {payload.accumulationAttachment, ExternalImageUseKind::SampledRead, ExternalImageAspectHint::Color});
    batch.pass.externalImageUses.push_back(
      {payload.momentsAttachment, ExternalImageUseKind::SampledRead, ExternalImageAspectHint::Color});
    batch.draw.topology = PrimitiveTopology::TriangleStrip;
    batch.draw.vertexCount = 4;
    batch.draw.indexCount = 0;
    batch.geometry = payload;
    m_rendererBase.appendBatch(std::move(batch));
  });

  resetHooks();
}

void Z3DCompositor::renderTransparentWBVulkan(const std::vector<Z3DBoundedFilter*>& filters,
                                              Z3DScratchResourcePool::RenderTargetLease& targetLease,
                                              Z3DEye eye,
                                              AttachmentHandle depthAttachmentHandle,
                                              const std::vector<Z3DCompositorImageLayer>& imageLayers,
                                              bool clearResolveTarget,
                                              ZVulkanLinearScript& script)
{
  const glm::uvec2 targetSize = targetLease.descriptor.size;
  auto& wbLease = ensureWBRenderTarget(targetSize);
  CHECK(wbLease.backend == RenderBackend::Vulkan);

  auto wbBindings = m_rendererBase.prepareVulkanSurface(wbLease);
  CHECK(wbBindings.colorHandles.size() >= 2 && wbBindings.surface.colorAttachments.size() >= 2)
    << "Weighted blended Vulkan target incomplete.";

  auto configureSurface = [&](RendererFrameState::ActiveSurface surface) {
    if (surface.colorAttachments.size() > 0) {
      surface.colorAttachments[0].loadOp = LoadOp::Clear;
      surface.colorAttachments[0].storeOp = StoreOp::Store;
      surface.colorAttachments[0].finalUse = AttachmentFinalUse::Sampled;
      surface.colorAttachments[0].clearValue.color = glm::vec4(0.0f);
    }
    if (surface.colorAttachments.size() > 1) {
      surface.colorAttachments[1].loadOp = LoadOp::Clear;
      surface.colorAttachments[1].storeOp = StoreOp::Store;
      surface.colorAttachments[1].finalUse = AttachmentFinalUse::Sampled;
      surface.colorAttachments[1].clearValue.color = glm::vec4(1.0f);
    }
    if (depthAttachmentHandle.valid()) {
      AttachmentDesc depthDesc;
      depthDesc.handle = depthAttachmentHandle;
      depthDesc.loadOp = LoadOp::Load;
      depthDesc.storeOp = StoreOp::Store;
      depthDesc.finalUse = AttachmentFinalUse::RenderTarget;
      depthDesc.clearValue.depth = 1.0f;
      surface.depthAttachment = depthDesc;
    } else {
      surface.depthAttachment.reset();
    }
    return surface;
  };

  auto resetHooks = [&]() {
    for (auto* filter : filters) {
      if (filter) {
        filter->setShaderHookType(Z3DRendererBase::ShaderHookType::Normal);
      }
    }
    m_rendererBase.setShaderHookType(Z3DRendererBase::ShaderHookType::Normal);
  };

  auto wbInitSurface = configureSurface(wbBindings.surface);

  // Geometry init step must see WeightedBlendedInit on the compositor renderer
  m_rendererBase.setShaderHookType(Z3DRendererBase::ShaderHookType::WeightedBlendedInit);
  const auto segInit = script.raster("transparency_wb_init", {}, [&]() {
    m_rendererBase.setActiveSurfaceWithLoadStore(wbInitSurface, Z3DRendererBase::Preserve);
    for (auto* filter : filters) {
      if (!filter) {
        continue;
      }
      filter->setShaderHookType(Z3DRendererBase::ShaderHookType::WeightedBlendedInit);
      recordFilterBatchesToSurfaceUnified(
        m_rendererBase,
        filter,
        wbInitSurface,
        [&]() {
          filter->renderTransparent(eye);
        },
        /*propagateHookPara=*/false);
    }
    if (!imageLayers.empty()) {
      m_rendererBase.setShaderHookType(Z3DRendererBase::ShaderHookType::WeightedBlendedInit);
      for (const auto& layer : imageLayers) {
        const auto& colorDesc = layer.colorAttachment;
        const auto& depthDesc = layer.depthAttachment;
        if (colorDesc.handle.backend != RenderBackend::Vulkan || !colorDesc.handle.valid()) {
          continue;
        }
        if (depthDesc.handle.backend != RenderBackend::Vulkan || !depthDesc.handle.valid()) {
          continue;
        }
        // WB image init copy: do not flip
        m_textureCopyRenderer.setFlipY(false);
        m_textureCopyRenderer.setSourceAttachments(colorDesc.handle, depthDesc.handle);
        m_rendererBase.renderVulkan(eye, m_textureCopyRenderer);
      }
    }
  });

  resetHooks();

  auto outBindings = m_rendererBase.prepareVulkanSurface(targetLease);
  RendererFrameState::ActiveSurface outSurface = outBindings.surface;
  // Enforce single color attachment; depth is optional and allows the resolve
  // shader to emit a representative gl_FragDepth when provided.
  if (outSurface.colorAttachments.size() > 1) {
    outSurface.colorAttachments.resize(1);
  }
  if (outSurface.depthAttachment) {
    outSurface.depthAttachment->loadOp = clearResolveTarget ? LoadOp::Clear : LoadOp::DontCare;
    outSurface.depthAttachment->storeOp = StoreOp::Store;
    outSurface.depthAttachment->clearValue.depth = 1.0f;
  }
  for (auto& attachment : outSurface.colorAttachments) {
    attachment.loadOp = clearResolveTarget ? LoadOp::Clear : (m_showBackground.get() ? LoadOp::Load : LoadOp::Clear);
    attachment.storeOp = StoreOp::Store;
    attachment.clearValue.color = glm::vec4(0.0f);
  }
  // Depth load/store configured above when the caller supplied an attachment.

  const glm::vec2 screenDimRcp(targetSize.x > 0u ? 1.f / static_cast<float>(targetSize.x) : 0.f,
                               targetSize.y > 0u ? 1.f / static_cast<float>(targetSize.y) : 0.f);

  script.raster("transparency_wb_resolve", {segInit}, [&]() {
    m_rendererBase.setActiveSurfaceWithLoadStore(outSurface, Z3DRendererBase::Preserve);
    TextureWeightedBlendedPayload payload;
    payload.accumulationAttachment = wbBindings.colorHandles[0];
    payload.transmittanceAttachment = wbBindings.colorHandles[1];
    payload.screenDimRcp = screenDimRcp;

    RenderBatch batch;
    batch.eye = eye;
    const glm::uvec4 viewport = m_rendererBase.frameState().viewport;

    batch.pass.viewport.origin = glm::vec2(static_cast<float>(viewport.x), static_cast<float>(viewport.y));
    batch.pass.viewport.extent = glm::vec2(static_cast<float>(viewport.z), static_cast<float>(viewport.w));
    batch.pass.viewport.minDepth = 0.0f;
    batch.pass.viewport.maxDepth = 1.0f;
    batch.pass.colorAttachments = m_rendererBase.frameState().activeSurface.colorAttachments;
    batch.pass.depthAttachment = m_rendererBase.frameState().activeSurface.depthAttachment;
    batch.pass.externalImageUses.push_back(
      {payload.accumulationAttachment, ExternalImageUseKind::SampledRead, ExternalImageAspectHint::Color});
    batch.pass.externalImageUses.push_back(
      {payload.transmittanceAttachment, ExternalImageUseKind::SampledRead, ExternalImageAspectHint::Color});
    batch.draw.topology = PrimitiveTopology::TriangleStrip;
    batch.draw.vertexCount = 4;
    batch.draw.indexCount = 0;
    batch.geometry = payload;
    m_rendererBase.appendBatch(std::move(batch));
  });

  resetHooks();
}

glm::uvec4 Z3DCompositor::axisViewportFor(const glm::uvec4& baseViewport) const
{
  if (m_region[0] > 0.f || m_region[2] > 0.f) {
    return glm::uvec4(0u);
  }

  if (std::abs(m_region[1]) < std::numeric_limits<float>::epsilon() ||
      std::abs(m_region[3]) < std::numeric_limits<float>::epsilon()) {
    return glm::uvec4(0u);
  }

  const double startX = baseViewport.x + static_cast<double>(baseViewport.z) / m_region[1] * m_region[0];
  const double startY = baseViewport.y + static_cast<double>(baseViewport.w) / m_region[3] * m_region[2];
  const uint32_t axisSize =
    static_cast<uint32_t>(std::max(1.0f, std::min<float>(baseViewport.z, baseViewport.w) * m_axisRegionRatio.get()));
  const int axisX = static_cast<int>(baseViewport.x) - static_cast<int>(std::floor(startX));
  const int axisY = static_cast<int>(baseViewport.y) - static_cast<int>(std::floor(startY));

  return glm::uvec4(static_cast<uint32_t>(std::max(axisX, 0)),
                    static_cast<uint32_t>(std::max(axisY, 0)),
                    axisSize,
                    axisSize);
}

void Z3DCompositor::ensureAxisCameraBackend(RenderBackend backend)
{
  if (m_axisCamera.getBackend() != backend) {
    setupAxisCamera();
  }
}

void Z3DCompositor::renderAxisVulkan(Z3DEye eye,
                                     Z3DScratchResourcePool::RenderTargetLease& sceneOutLease,
                                     ZVulkanLinearScript& script)
{
  if (!sceneOutLease || sceneOutLease.backend != RenderBackend::Vulkan) {
    return;
  }

  ensureAxisCameraBackend(RenderBackend::Vulkan);
  prepareAxisData(eye);

  const glm::mat4 axisTransform = glm::mat4(m_globalParameters.camera.get().rotateMatrix(eye));
  const glm::uvec4 baseViewport = viewport();
  const glm::uvec4 axisViewport = axisViewportFor(baseViewport);
  if (axisViewport.z == 0u || axisViewport.w == 0u) {
    return;
  }
  VLOG(1) << "Axis overlay viewport=" << axisViewport.x << "," << axisViewport.y << " " << axisViewport.z << "x"
          << axisViewport.w;

  auto axisSurface = m_rendererBase.describeSurface(sceneOutLease);
  CHECK(!(axisSurface.colorAttachments.empty() && !axisSurface.depthAttachment.has_value()))
    << "Vulkan axis overlay skipped: compositor output lease has no Vulkan attachments.";

  auto& params = m_rendererBase.parameterState();
  const glm::mat4 previousTransform = params.coordTransform;
  const RendererViewState previousViewState = m_rendererBase.viewState();
  const glm::uvec4 previousViewport = m_rendererBase.frameState().viewport;
  const auto previousSurface = m_rendererBase.frameState().activeSurface;
  const auto previousHook = m_rendererBase.shaderHookType();
  auto stateGuard = folly::makeGuard([&]() {
    params.coordTransform = previousTransform;
    m_rendererBase.restoreViewState(previousViewState);
    m_rendererBase.frameState().updateViewportData(previousViewport);
    m_rendererBase.setActiveSurfaceWithLoadStore(previousSurface, Z3DRendererBase::Preserve);
    m_rendererBase.setShaderHookType(previousHook);
  });

  params.coordTransform = axisTransform;
  auto axisViewState = std::make_shared<RendererViewState>(Z3DRendererBase::buildViewStateFromCamera(m_axisCamera));
  m_rendererBase.restoreViewState(*axisViewState);
  m_rendererBase.frameState().updateViewportData(axisViewport);
  m_rendererBase.setShaderHookType(Z3DRendererBase::ShaderHookType::Normal);

  RendererCPUState axisBatches = m_rendererBase.captureVulkanBatches(
    [&]() {
      m_rendererBase.setActiveSurfaceWithLoadStore(axisSurface,
                                                   LoadOp::Load,
                                                   StoreOp::Store,
                                                   LoadOp::Clear,
                                                   StoreOp::Store);
      if (m_axisMode.get() == "Arrow") {
        m_rendererBase.renderVulkan(eye, m_arrowRenderer, m_fontRenderer);
      } else {
        m_rendererBase.renderVulkan(eye, m_lineRenderer, m_fontRenderer);
      }
    },
    "axis_overlay");

  if (axisBatches.batches.empty()) {
    return;
  }

  for (auto& batch : axisBatches.batches) {
    batch.viewStateOverride = axisViewState;
  }

  script.replay("axis_overlay", {}, std::make_shared<RendererCPUState>(std::move(axisBatches)));
}

void Z3DCompositor::renderAxis(Z3DEye eye)
{
  ensureAxisCameraBackend(RenderBackend::OpenGL);
  prepareAxisData(eye);
  {
    const glm::mat4 axisTransform = glm::mat4(m_globalParameters.camera.get().rotateMatrix(eye));
    const glm::uvec4 baseViewport = viewport();
    const glm::uvec4 axisViewport = axisViewportFor(baseViewport);
    if (axisViewport.z == 0u || axisViewport.w == 0u) {
      return;
    }

    const glm::uvec4 previousViewport = m_rendererBase.frameState().viewport;
    auto viewportGuard = folly::makeGuard([&]() {
      m_rendererBase.frameState().updateViewportData(previousViewport);
    });
    m_rendererBase.frameState().updateViewportData(axisViewport);

    glViewport(static_cast<GLint>(axisViewport.x),
               static_cast<GLint>(axisViewport.y),
               static_cast<GLsizei>(axisViewport.z),
               static_cast<GLsizei>(axisViewport.w));
    glScissor(static_cast<GLint>(axisViewport.x),
              static_cast<GLint>(axisViewport.y),
              static_cast<GLsizei>(axisViewport.z),
              static_cast<GLsizei>(axisViewport.w));
    glEnable(GL_SCISSOR_TEST);
    glClear(GL_DEPTH_BUFFER_BIT);

    if (m_axisMode.get() == "Arrow") {
      renderWithStateAndCameraAndCoordTransform(eye, m_axisCamera, axisTransform, m_arrowRenderer, m_fontRenderer);
    } else {
      renderWithStateAndCameraAndCoordTransform(eye, m_axisCamera, axisTransform, m_lineRenderer, m_fontRenderer);
    }

    glViewport(static_cast<GLint>(baseViewport.x),
               static_cast<GLint>(baseViewport.y),
               static_cast<GLsizei>(baseViewport.z),
               static_cast<GLsizei>(baseViewport.w));
    glScissor(static_cast<GLint>(baseViewport.x),
              static_cast<GLint>(baseViewport.y),
              static_cast<GLsizei>(baseViewport.z),
              static_cast<GLsizei>(baseViewport.w));
    glDisable(GL_SCISSOR_TEST);
  }
}

void Z3DCompositor::prepareAxisData(Z3DEye eye)
{
  m_textPositions.clear();
  glm::mat3 rotMatrix = m_globalParameters.camera.get().rotateMatrix(eye);
  m_XEnd = rotMatrix * glm::vec3(256.f, 0.f, 0.f);
  m_YEnd = rotMatrix * glm::vec3(0.f, 256.f, 0.f);
  m_ZEnd = rotMatrix * glm::vec3(0.f, 0.f, 256.f);

  m_textPositions.push_back(m_XEnd * glm::vec3(0.93));
  m_textPositions.push_back(m_YEnd * glm::vec3(0.93));
  m_textPositions.push_back(m_ZEnd * glm::vec3(0.93));

  QStringList texts;
  texts.push_back("X");
  texts.push_back("Y");
  texts.push_back("Z");

  m_fontRenderer.setData(&m_textPositions, texts);
}

void Z3DCompositor::setupAxisCamera()
{
  Z3DCamera camera(m_rendererBase.activeBackend());
  glm::vec3 center(0.f);
  camera.setFieldOfView(glm::radians(10.f));

  float radius = 300.f;

  float distance = radius / std::sin(camera.fieldOfView() * 0.5f);
  glm::vec3 vn(0, 0, 1); // plane normal
  glm::vec3 position = center + vn * distance;
  camera.setCamera(position, center, glm::vec3(0.0, 1.0, 0.0));
  camera.setNearDist(distance - radius - 1);
  camera.setFarDist(distance + radius);

  m_axisCamera = camera;

  m_tailPosAndTailRadius.clear();
  m_headPosAndHeadRadius.clear();
  m_lineColors.clear();
  m_lines.clear();
  m_textColors.clear();
  m_textPositions.clear();
  m_XEnd = glm::vec3(256.f, 0.f, 0.f);
  m_YEnd = glm::vec3(0.f, 256.f, 0.f);
  m_ZEnd = glm::vec3(0.f, 0.f, 256.f);
  glm::vec3 origin(0.f);
  m_lines.push_back(origin);
  m_lineColors.push_back(m_XAxisColor.get());
  m_lines.push_back(m_XEnd * glm::vec3(0.88));
  m_lineColors.push_back(m_XAxisColor.get());
  m_lines.push_back(origin);
  m_lineColors.push_back(m_YAxisColor.get());
  m_lines.push_back(m_YEnd * glm::vec3(0.88));
  m_lineColors.push_back(m_YAxisColor.get());
  m_lines.push_back(origin);
  m_lineColors.push_back(m_ZAxisColor.get());
  m_lines.push_back(m_ZEnd * glm::vec3(0.88));
  m_lineColors.push_back(m_ZAxisColor.get());

  m_textPositions.push_back(m_XEnd * glm::vec3(0.93));
  m_textPositions.push_back(m_YEnd * glm::vec3(0.93));
  m_textPositions.push_back(m_ZEnd * glm::vec3(0.93));
  m_textColors.push_back(m_XAxisColor.get());
  m_textColors.push_back(m_YAxisColor.get());
  m_textColors.push_back(m_ZAxisColor.get());

  float tailRadius = 5.f;
  float headRadius = 10.f;

  m_tailPosAndTailRadius.emplace_back(origin, tailRadius);
  m_headPosAndHeadRadius.emplace_back(m_XEnd * glm::vec3(0.88), headRadius);

  m_tailPosAndTailRadius.emplace_back(origin, tailRadius);
  m_headPosAndHeadRadius.emplace_back(m_YEnd * glm::vec3(0.88), headRadius);

  m_tailPosAndTailRadius.emplace_back(origin, tailRadius);
  m_headPosAndHeadRadius.emplace_back(m_ZEnd * glm::vec3(0.88), headRadius);

  m_lineRenderer.setData(std::move(m_lines));
  m_lineRenderer.setDataColors(std::move(m_lineColors));
  m_lineRenderer.setLineWidth(6.f);
  // Axis overlay is an output-sized (post-resolve) UI element even when the
  // main scene uses supersampling. Do not apply supersampling width
  // compensation here, otherwise the lines appear too thick in 2x2 mode.
  m_lineRenderer.setFollowSupersampling(false);
  m_arrowRenderer.setArrowData(&m_tailPosAndTailRadius, &m_headPosAndHeadRadius, .1f);
  m_arrowRenderer.setArrowColors(&m_textColors);
  m_fontRenderer.setDataColors(&m_textColors);
}

// Collect non-opaque image layers (color/depth) from connected image filters
std::vector<Z3DCompositorImageLayer> Z3DCompositor::collectNonOpaqueImageLayers(Z3DEye eye)
{
  std::vector<Z3DCompositorImageLayer> layers;
  const bool useVulkan = m_rendererBase.activeBackend() == RenderBackend::Vulkan;

  for (auto* vf : m_volumeFilters) {
    if (!vf || !vf->isReady(eye) || !vf->hasTransparent(eye)) {
      continue;
    }

    Z3DCompositorImageLayer layer;

    if (useVulkan) {
      const auto& lease = vf->transparentLease(eye);
      CHECK(lease) << "Image filter reported Vulkan transparent output but returned an empty lease.";
      CHECK(lease.backend == RenderBackend::Vulkan)
        << "Image filter transparent lease must be Vulkan-backed in Vulkan compositor path.";
      auto surface = m_rendererBase.describeSurface(lease);
      if (surface.colorAttachments.empty()) {
        continue;
      }
      layer.colorAttachment = surface.colorAttachments[0];
      if (surface.depthAttachment) {
        layer.depthAttachment = *surface.depthAttachment;
      }
    } else {
      const auto& target = vf->transparentTarget(eye);
      layer.glColorTexture = target.attachment(GL_COLOR_ATTACHMENT0);
      layer.glDepthTexture = target.attachment(GL_DEPTH_ATTACHMENT);
    }

    if (useVulkan) {
      VLOG(1) << fmt::format("ImageLayer collected: filter={} color=0x{:x} depth=0x{:x}",
                             vf->className().toStdString(),
                             layer.colorAttachment.handle.id,
                             layer.depthAttachment.handle.id);
    }
    layers.push_back(layer);
  }
  return layers;
}

// Merge a list of image layers using the same shader/path as renderImages
bool Z3DCompositor::mergeImageLayers(const std::vector<Z3DCompositorImageLayer>& layers,
                                     Z3DEye eye,
                                     Z3DScratchResourcePool::RenderTargetLease& targetLease,
                                     const Z3DTexture*& colorTex,
                                     const Z3DTexture*& depthTex)
{
  auto* glTarget = targetLease.renderTarget;
  CHECK(glTarget != nullptr);
  const auto targetSize = glTarget->size();

  colorTex = nullptr;
  depthTex = nullptr;
  if (layers.empty()) {
    return false;
  }
  if (layers.size() == 1) {
    colorTex = layers[0].glColorTexture;
    depthTex = layers[0].glDepthTexture;
    return true;
  }

  auto imgLease1 = Z3DRenderGlobalState::instance().scratchPool().acquireTempRenderTarget2D(targetSize);
  auto imgLease2 = Z3DRenderGlobalState::instance().scratchPool().acquireTempRenderTarget2D(targetSize);

  // Blend first two layers into imgLease1
  imgLease1.renderTarget->bind();
  imgLease1.renderTarget->clear();
  setViewport(imgLease1.renderTarget->size());
  m_MIPImageAlphaBlendRenderer.setColorTexture1(layers[0].glColorTexture);
  m_MIPImageAlphaBlendRenderer.setDepthTexture1(layers[0].glDepthTexture);
  m_MIPImageAlphaBlendRenderer.setColorTexture2(layers[1].glColorTexture);
  m_MIPImageAlphaBlendRenderer.setDepthTexture2(layers[1].glDepthTexture);
  m_rendererBase.render(eye, m_MIPImageAlphaBlendRenderer);
  imgLease1.renderTarget->release();
  auto* resLease = &imgLease1;
  auto* nextLease = &imgLease2;
  auto* resRT = resLease->renderTarget;
  auto* nextRT = nextLease->renderTarget;
  for (size_t i = 2; i < layers.size(); ++i) {
    nextRT->bind();
    nextRT->clear();
    setViewport(nextRT->size());
    m_MIPImageAlphaBlendRenderer.setColorTexture1(resRT->colorTexture());
    m_MIPImageAlphaBlendRenderer.setDepthTexture1(resRT->depthTexture());
    m_MIPImageAlphaBlendRenderer.setColorTexture2(layers[i].glColorTexture);
    m_MIPImageAlphaBlendRenderer.setDepthTexture2(layers[i].glDepthTexture);
    m_rendererBase.render(eye, m_MIPImageAlphaBlendRenderer);
    nextRT->release();
    std::swap(resLease, nextLease);
    resRT = resLease->renderTarget;
    nextRT = nextLease->renderTarget;
  }

  colorTex = resRT->colorTexture();
  depthTex = resRT->depthTexture();
  return true;
}

void Z3DCompositor::downloadTextureToLocalColorBuffer(const Z3DTexture& tex, Z3DLocalColorBuffer& localColorBuffer)
{
  // Set up format and type
  GLenum dataFormat = GL_BGRA;
  GLenum dataType = GL_UNSIGNED_INT_8_8_8_8_REV;

  // Allocate buffer for texture data
  auto desiredSize = Z3DTexture::bypePerPixel(dataFormat, dataType) * tex.numPixels();
  if (localColorBuffer.data.size() < desiredSize) {
    localColorBuffer.data.resize(desiredSize);
  }

#if 0
  if (true) {
    m_PBO.bind(GL_PIXEL_PACK_BUFFER);
    glBufferData(GL_PIXEL_PACK_BUFFER, desiredSize, nullptr, GL_STREAM_READ);
    tex.downloadTextureToBuffer(dataFormat, dataType, nullptr);
    // Map PBO to client memory
    auto ptr = glMapBuffer(GL_PIXEL_PACK_BUFFER, GL_READ_ONLY);
    if (ptr) {
      std::memcpy(localColorBuffer.data.data(), ptr, desiredSize);
      glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
    } else {
      LOG(ERROR) << "glMapBuffer failed on PBO";
      tex.downloadTextureToBuffer(dataFormat, dataType, localColorBuffer.data.data());
    }
    m_PBO.release(GL_PIXEL_PACK_BUFFER);
  } else {
    // Download texture data to buffer
    tex.downloadTextureToBuffer(dataFormat, dataType, localColorBuffer.data.data());
  }
#else
  tex.downloadTextureToBuffer(dataFormat, dataType, localColorBuffer.data.data());
#endif
  localColorBuffer.width = tex.width();
  localColorBuffer.height = tex.height();
}

} // namespace nim
