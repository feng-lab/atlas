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
#include "zvulkantexture.h"
#include "z3drenderervulkanbackend.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <folly/ScopeGuard.h>
#include <functional>
#include <cmath>
#include <limits>
#include <span>
#include <unordered_set>
#include <optional>

DEFINE_bool(atlas_vk_copy_yflip_in_shader, true, "Use y-flip in Vulkan final copy shader instead of UI flip");

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

// Record batches within a Vulkan frame, opening/closing the frame if needed.
// Keeps per-pass priming/reset semantics by scoping begin/end to the call site.
inline void
recordInVulkanFrame(Z3DRendererBase& renderer, const std::function<void()>& recordBatches, std::string_view label)
{
  // Surfaces should already be applied by callers
  // Apply any pending surface before beginning the frame so the backend can
  // observe attachments from the very first segment during beginRender.
  // (pending already applied above)
  const bool startedHere = !renderer.isVulkanFrameActive();
  if (startedHere) {
    renderer.beginVulkanFrame(label);
  }
  auto endGuard = folly::makeGuard([&renderer, startedHere]() {
    if (startedHere) {
      renderer.endVulkanFrame();
    }
  });
  renderer.recordVulkanBatchesInActiveFrame(recordBatches, label);
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
  //, m_renderGeometries("Render Geometries", true)
  , m_gPPort("GeometryFilters", true, this)
  , m_vPPort("VolumeFilters", true, this)
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
  , m_screenQuadVAO(1)
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

  addPort(m_gPPort);
  addPort(m_vPPort);

  m_textureCopyRenderer.setDiscardTransparent(true);
  m_backgroundFirstColor.setStyle("COLOR");
  m_backgroundSecondColor.setStyle("COLOR");
  m_backgroundMode.clearOptions();
  m_backgroundMode.addOptionsWithData(
    std::make_pair(enumToQString(BackgroundMode::Uniform), static_cast<int>(BackgroundMode::Uniform)),
    std::make_pair(enumToQString(BackgroundMode::Gradient), static_cast<int>(BackgroundMode::Gradient)));
  m_backgroundMode.select(enumToQString(BackgroundMode::Gradient));
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

  m_waFinalShader = std::make_unique<Z3DShaderProgram>();
  m_waFinalShader->loadFromSourceFile("pass.vert", "wavg_final.frag", m_rendererBase.generateHeader());

  m_wbFinalShader = std::make_unique<Z3DShaderProgram>();
  m_wbFinalShader->loadFromSourceFile("pass.vert", "wblended_final.frag", m_rendererBase.generateHeader());

  m_ddpBlendShader = std::make_unique<Z3DShaderProgram>();
  m_ddpBlendShader->loadFromSourceFile("pass.vert", "dual_peeling_blend.frag", m_rendererBase.generateHeader());

  m_ddpFinalShader = std::make_unique<Z3DShaderProgram>();
  m_ddpFinalShader->loadFromSourceFile("pass.vert", "dual_peeling_final.frag", m_rendererBase.generateHeader());

  ensurePickingTarget(glm::uvec2(32u, 32u));
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

void Z3DCompositor::setOutputSize(const glm::uvec2& size)
{
  if (size == m_outputSize) {
    return;
  }

  CHECK_GT(size.x, 0u);
  CHECK_GT(size.y, 0u);

  m_outputSize = size;
  ensureOutputTargets(m_outputSize);

  if (size != m_vPPort.expectedSize()) {
    m_vPPort.setExpectedSize(size);
    m_globalParameters.camera.viewportChanged(size);
    Q_EMIT requestUpstreamSizeChange(this);
  }
}

glm::uvec2 Z3DCompositor::outputSize() const
{
  return m_outputSize;
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

  std::vector<Z3DGeometryFilter*> filters = m_gPPort.connectedFilters();
  std::vector<Z3DImgFilter*> vFilters = m_vPPort.connectedFilters();
  // VLOG(1) << filters.size() << " " << vFilters.size();
  std::vector<Z3DBoundedFilter*> onTopOpaqueFilters;
  std::vector<Z3DBoundedFilter*> onTopTransparentFilters;
  std::vector<Z3DBoundedFilter*> normalOpaqueFilters;
  std::vector<Z3DBoundedFilter*> normalTransparentFilters;
  std::vector<Z3DBoundedFilter*> selectedFilters;
  std::vector<Z3DBoundedFilter*> showHandleFilters;

  const auto transparencyMode = m_rendererBase.sceneState().transparency;
  const bool multisample2x2 = (m_rendererBase.sceneState().multisample == GeometryMSAAMode::MSAA2x2);
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

  glEnable(GL_DEPTH_TEST);

  if (transparencyMode == TransparencyMode::BlendNoDepthMask || transparencyMode == TransparencyMode::BlendDelayed) {
    if (!anyVolumeReady) { // no volume, only geometrys to render
      if (numNormalFilters == 0 || numOnTopFilters == 0) {
        // Acquire temp for geometry-only path (optionally twice the size)
        Z3DScratchResourcePool::RenderTargetLease temp1Lease =
          Z3DRenderGlobalState::instance().scratchPool().acquireTempRenderTarget2D(
            multisample2x2 ? (currentOutRenderTarget.size() * 2_u32) : currentOutRenderTarget.size());
        // VLOG(1) << "lease acquired";

        if (numOnTopFilters == 0) {
          renderGeometries(normalOpaqueFilters, normalTransparentFilters, temp1Lease, eye);
        } else {
          renderGeometries(onTopOpaqueFilters, onTopTransparentFilters, temp1Lease, eye);
        }

        // copy to outport
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
        if (m_showAxis.get()) {
          if (!m_showBackground.get()) {
            glEnable(GL_BLEND);
            glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
          }
          renderAxis(eye);
        }
        if (m_showBackground.get() || m_showAxis.get()) {
          glBlendFunc(GL_ONE, GL_ZERO);
          glDisable(GL_BLEND);
        }

        currentOutRenderTarget.release();
      } else {
        auto tempSize = multisample2x2 ? (currentOutRenderTarget.size() * 2_u32) : currentOutRenderTarget.size();
        Z3DScratchResourcePool::RenderTargetLease temp1Lease =
          Z3DRenderGlobalState::instance().scratchPool().acquireTempRenderTarget2D(tempSize);
        // VLOG(1) << "lease acquired";
        Z3DScratchResourcePool::RenderTargetLease temp2Lease =
          Z3DRenderGlobalState::instance().scratchPool().acquireTempRenderTarget2D(tempSize);
        // VLOG(1) << "lease acquired";

        // render normal geometries to tempport
        renderGeometries(normalOpaqueFilters, normalTransparentFilters, temp1Lease, eye);

        // render on top geometries to tempport2
        renderGeometries(onTopOpaqueFilters, onTopTransparentFilters, temp2Lease, eye);

        // blend to output
        currentOutRenderTarget.bind();
        currentOutRenderTarget.clear();
        setViewport(currentOutRenderTarget.size());

        if (m_showBackground.get()) {
          m_rendererBase.render(eye, m_backgroundRenderer);
          glEnable(GL_BLEND);
          glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        }
        m_firstOnTopBlendRenderer.setColorTexture1(temp2Lease.renderTarget->colorTexture());
        m_firstOnTopBlendRenderer.setDepthTexture1(temp2Lease.renderTarget->depthTexture());
        m_firstOnTopBlendRenderer.setColorTexture2(temp1Lease.renderTarget->colorTexture());
        m_firstOnTopBlendRenderer.setDepthTexture2(temp1Lease.renderTarget->depthTexture());
        m_rendererBase.render(eye, m_firstOnTopBlendRenderer);
        if (m_showAxis.get()) {
          if (!m_showBackground.get()) {
            glEnable(GL_BLEND);
            glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
          }
          renderAxis(eye);
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
      if (numNormalFilters == 0 && numOnTopFilters == 0) { // directly copy inport image to outport
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
        if (m_showAxis.get()) {
          if (!m_showBackground.get()) {
            glEnable(GL_BLEND);
            glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
          }
          renderAxis(eye);
        }
        if (m_showBackground.get() || m_showAxis.get()) {
          glBlendFunc(GL_ONE, GL_ZERO);
          glDisable(GL_BLEND);
        }

        currentOutRenderTarget.release();
      } else if (numNormalFilters == 0 ||
                 numOnTopFilters == 0) { // render geometries into one temp port then blend with volume
        Z3DScratchResourcePool::RenderTargetLease tempGeoLease =
          Z3DRenderGlobalState::instance().scratchPool().acquireTempRenderTarget2D(
            multisample2x2 ? (currentOutRenderTarget.size() * 2_u32) : currentOutRenderTarget.size());
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

        if (m_showAxis.get()) {
          if (!m_showBackground.get()) {
            glEnable(GL_BLEND);
            glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
          }
          renderAxis(eye);
        }
        if (m_showBackground.get() || m_showAxis.get()) {
          glBlendFunc(GL_ONE, GL_ZERO);
          glDisable(GL_BLEND);
        }

        currentOutRenderTarget.release();
      } else { // render normal geometries into tempport, then blend inport and tempport into tempport2, then render on
               // top geometries into tempport, then
        // blend temport and temport2 into outport
        auto tempSize2 = multisample2x2 ? (currentOutRenderTarget.size() * 2_u32) : currentOutRenderTarget.size();
        Z3DScratchResourcePool::RenderTargetLease temp1LeaseA =
          Z3DRenderGlobalState::instance().scratchPool().acquireTempRenderTarget2D(tempSize2);
        // VLOG(1) << "lease acquired";
        Z3DScratchResourcePool::RenderTargetLease temp2LeaseA =
          Z3DRenderGlobalState::instance().scratchPool().acquireTempRenderTarget2D(tempSize2);
        // VLOG(1) << "lease acquired";

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

        // blend temport and temport2 into outport
        currentOutRenderTarget.bind();
        currentOutRenderTarget.clear();
        setViewport(currentOutRenderTarget.size());

        if (m_showBackground.get()) {
          m_rendererBase.render(eye, m_backgroundRenderer);
          glEnable(GL_BLEND);
          glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        }
        m_firstOnTopBlendRenderer.setColorTexture1(temp1LeaseA.renderTarget->colorTexture());
        m_firstOnTopBlendRenderer.setDepthTexture1(temp1LeaseA.renderTarget->depthTexture());
        m_firstOnTopBlendRenderer.setColorTexture2(temp2LeaseA.renderTarget->colorTexture());
        m_firstOnTopBlendRenderer.setDepthTexture2(temp2LeaseA.renderTarget->depthTexture());
        m_rendererBase.render(eye, m_firstOnTopBlendRenderer);

        if (m_showAxis.get()) {
          if (!m_showBackground.get()) {
            glEnable(GL_BLEND);
            glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
          }
          renderAxis(eye);
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
          multisample2x2 ? (currentOutRenderTarget.size() * 2_u32) : currentOutRenderTarget.size());
      // VLOG(1) << "lease acquired";

      if (numOnTopFilters == 0) {
        renderGeometries(normalOpaqueFilters, normalTransparentFilters, temp1Lease, eye);
      } else {
        renderGeometries(onTopOpaqueFilters, onTopTransparentFilters, temp1Lease, eye);
      }

      // copy to outport
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
      if (m_showAxis.get()) {
        if (!m_showBackground.get()) {
          glEnable(GL_BLEND);
          glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        }
        renderAxis(eye);
      }
      if (m_showBackground.get() || m_showAxis.get()) {
        glBlendFunc(GL_ONE, GL_ZERO);
        glDisable(GL_BLEND);
      }

      currentOutRenderTarget.release();
    } else {
      auto tempSize = multisample2x2 ? (currentOutRenderTarget.size() * 2_u32) : currentOutRenderTarget.size();
      Z3DScratchResourcePool::RenderTargetLease temp1Lease2 =
        Z3DRenderGlobalState::instance().scratchPool().acquireTempRenderTarget2D(tempSize);
      // VLOG(1) << "lease acquired";
      Z3DScratchResourcePool::RenderTargetLease temp2Lease2 =
        Z3DRenderGlobalState::instance().scratchPool().acquireTempRenderTarget2D(tempSize);
      // VLOG(1) << "lease acquired";

      // render normal geometries to tempport
      renderGeometries(normalOpaqueFilters, normalTransparentFilters, temp1Lease2, eye);

      // render on top geometries to tempport2
      renderGeometries(onTopOpaqueFilters, onTopTransparentFilters, temp2Lease2, eye);

      // blend to output
      currentOutRenderTarget.bind();
      currentOutRenderTarget.clear();
      setViewport(currentOutRenderTarget.size());

      if (m_showBackground.get()) {
        m_rendererBase.render(eye, m_backgroundRenderer);
        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
      }
      m_firstOnTopBlendRenderer.setColorTexture1(temp2Lease2.renderTarget->colorTexture());
      m_firstOnTopBlendRenderer.setDepthTexture1(temp2Lease2.renderTarget->depthTexture());
      m_firstOnTopBlendRenderer.setColorTexture2(temp1Lease2.renderTarget->colorTexture());
      m_firstOnTopBlendRenderer.setDepthTexture2(temp1Lease2.renderTarget->depthTexture());
      m_rendererBase.render(eye, m_firstOnTopBlendRenderer);
      if (m_showAxis.get()) {
        if (!m_showBackground.get()) {
          glEnable(GL_BLEND);
          glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        }
        renderAxis(eye);
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
    const uint64_t curCreate = pool.creationCounter();
    const uint64_t curChange = pool.changeCounter();
    if (curCreate != s_lastCreate || curChange != s_lastChange) {
      VLOG(1) << pool.describeMemoryUsage(true);
      s_lastCreate = curCreate;
      s_lastChange = curChange;
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
  std::vector<Z3DGeometryFilter*> gFilters = m_gPPort.connectedFilters();
  std::vector<Z3DImgFilter*> vFilters = m_vPPort.connectedFilters();
  std::vector<Z3DBoundedFilter*> opaqueFilters;
  std::vector<Z3DBoundedFilter*> transparentFilters;
  std::vector<Z3DBoundedFilter*> onTopOpaqueFilters;
  std::vector<Z3DBoundedFilter*> onTopTransparentFilters;

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
  }

  ensureOutputTargets(m_outputSize);
  // Use primary output lease according to eye
  Z3DScratchResourcePool::RenderTargetLease* outLease = nullptr;
  if (eye == MonoEye) {
    outLease = &m_outRenderTarget1;
  } else if (eye == LeftEye) {
    outLease = &m_leftEyeOutRenderTarget1;
  } else { // RightEye
    outLease = &m_outRenderTarget1; // mirrors current right-eye mapping in ctor
  }
  if (!outLease || !*outLease) {
    return 0.0;
  }

  // NOTE: Do not keep a Vulkan frame open across the compositor passes.
  // Rationale:
  // - The compositor orchestrates many heterogeneous passes (background, opaque,
  //   OIT init/peel/resolve, glow, picking) and multiple pipeline contexts
  //   (mesh/sphere/cone/texture). The Vulkan backend primes descriptor sets,
  //   UBOs and state per pass inside beginRender().
  // - Forcing a single long-lived frame here prevents those per-pass priming
  //   and resets from occurring at the correct time, and allows state to leak
  //   between passes. In practice this breaks lighting/transparency for common
  //   geometry (e.g., spheres/cones) and can yield wrong colors.
  // - Per-pass execute/record helpers (executeCompositorPass/recordInVulkanFrame) already manage frame
  //   begin/end after the target surface and load/store ops are staged. Let
  //   them own frame lifetime in the compositor.
  // If you ever need a single command buffer here, you must pre-prime all
  // contexts before recording (ensureDescriptorSets/ensureOITResources), avoid
  // descriptor writes during recording, and emulate per-pass resets — which is
  // fragile and not recommended. Therefore we intentionally disable keep-open.

  // Supersample 2x2 parity (render to 2x scene lease, then downsample)
  const bool supersample2x2 = (m_rendererBase.sceneState().multisample == GeometryMSAAMode::MSAA2x2);

  auto& pool = Z3DRenderGlobalState::instance().scratchPool();
  Z3DScratchResourcePool::RenderTargetLease sceneLease;
  Z3DScratchResourcePool::RenderTargetLease* sceneOutLease = outLease;
  if (supersample2x2) {
    const glm::uvec2 ssSize = m_outputSize * 2_u32;
    sceneLease =
      pool.acquireTempRenderTarget2D(ssSize, ScratchFormat::RGBA16, ScratchFormat::Depth32F, RenderBackend::Vulkan);
    sceneOutLease = &sceneLease;
  }

  // Frame lifetime is managed per pass via recordInVulkanFrame/executeCompositorPass;
  // do not keep the frame open across compositor passes.
  // Stage 3: background is recorded via the pass-graph driver below

  // Decide OIT usage and collect non-opaque image layers (volumes/slices) once
  const auto transparencyMode = m_rendererBase.sceneState().transparency;
  const bool useOIT = transparencyMode == TransparencyMode::DualDepthPeeling ||
                      transparencyMode == TransparencyMode::WeightedAverage ||
                      transparencyMode == TransparencyMode::WeightedBlended;
  const auto nonOpaqueLayers = collectNonOpaqueImageLayers(eye);
  bool imagesIntegratedViaOIT = false;

  // Only engage OIT when there is actual transparency to resolve (either
  // geometry with transparent fragments or non-opaque image layers). If we
  // only have opaque geometry, rendering via OIT paths can overwrite the
  // background with cleared textures (e.g. WA/WB accumulators), yielding a
  // white or incorrect background.
  if (useOIT && (!transparentFilters.empty() || !nonOpaqueLayers.empty())) {
    // Record background only; geometry/transparency handled by OIT path below
    executeCompositorPassesVulkan({},
                                  {},
                                  *sceneOutLease,
                                  eye,
                                  /*includeGeometry*/ false,
                                  /*clearAtStart*/ true,
                                  /*drawBackground*/ true);
    auto dispatchOIT =
      [&](Z3DScratchResourcePool::RenderTargetLease& lease, AttachmentHandle depthHandle, bool clearResolve) {
        switch (transparencyMode) {
          case TransparencyMode::DualDepthPeeling:
            renderTransparentDDPVulkan(transparentFilters, lease, eye, depthHandle, nonOpaqueLayers, clearResolve);
            break;
          case TransparencyMode::WeightedAverage:
            renderTransparentWAVulkan(transparentFilters, lease, eye, depthHandle, nonOpaqueLayers, clearResolve);
            break;
          case TransparencyMode::WeightedBlended:
            renderTransparentWBVulkan(transparentFilters, lease, eye, depthHandle, nonOpaqueLayers, clearResolve);
            break;
          default:
            break;
        }
      };

    if (opaqueFilters.empty()) {
      // Resolve directly to the final scene surface; preserve background
      dispatchOIT(*sceneOutLease, {}, /*clearResolve=*/false);
    } else {
      const glm::uvec2 targetSize = sceneOutLease->descriptor.size;
      auto leaseOpaque = pool.acquireTempRenderTarget2D(targetSize,
                                                        ScratchFormat::RGBA16,
                                                        ScratchFormat::Depth32F,
                                                        RenderBackend::Vulkan);
      // Render opaque geometry into an opaque-only intermediate without drawing background
      executeCompositorPassesVulkan(opaqueFilters,
                                    {},
                                    leaseOpaque,
                                    eye,
                                    /*includeGeometry*/ true,
                                    /*clearAtStart*/ true,
                                    /*drawBackground*/ false);

      auto leaseTrans = pool.acquireTempRenderTarget2D(targetSize,
                                                       ScratchFormat::RGBA16,
                                                       ScratchFormat::Depth32F,
                                                       RenderBackend::Vulkan);
      AttachmentHandle depthHandle;
      if (auto* depthTex = leaseOpaque.depthAttachmentTexture()) {
        depthHandle.backend = AttachmentBackend::Vulkan;
        depthHandle.index = 0;
        depthHandle.id = reinterpret_cast<uint64_t>(depthTex);
      }
      // Write OIT results into a temporary lease; clear it first
      dispatchOIT(leaseTrans, depthHandle, /*clearResolve=*/true);

      // Compose transparent OIT result over opaque using premultiplied alpha.
      // Transparent contributions behind opaque were already culled during the
      // OIT init/peel passes via the provided depth attachment, so the final
      // resolve does not require an additional depth test here.
      AttachmentHandle opaqueColor{};
      opaqueColor.backend = AttachmentBackend::Vulkan;
      opaqueColor.index = 0;
      opaqueColor.id = reinterpret_cast<uint64_t>(leaseOpaque.colorAttachment(0));
      AttachmentHandle opaqueDepth{};
      opaqueDepth.backend = AttachmentBackend::Vulkan;
      opaqueDepth.index = 0;
      opaqueDepth.id = reinterpret_cast<uint64_t>(leaseOpaque.depthAttachmentTexture());
      AttachmentHandle transColor{};
      transColor.backend = AttachmentBackend::Vulkan;
      transColor.index = 0;
      transColor.id = reinterpret_cast<uint64_t>(leaseTrans.colorAttachment(0));
      AttachmentHandle transDepth{};
      transDepth.backend = AttachmentBackend::Vulkan;
      transDepth.index = 0;
      transDepth.id = reinterpret_cast<uint64_t>(leaseTrans.depthAttachmentTexture());

      // First-on-top blending expects source 0 to be the overlay and source 1
      // the base. Put transparent (overlay) in slot 0 and opaque (base) in 1.
      m_alphaBlendRenderer.setSourceAttachments0(transColor, transDepth);
      m_alphaBlendRenderer.setSourceAttachments1(opaqueColor, opaqueDepth);

      // Preserve the previously rendered background when composing OIT result
      // (GL draws background first and then overlays; mimic by loading color).
      m_rendererBase.setActiveSurfaceWithLoadStore(*sceneOutLease,
                                                   m_showBackground.get() ? LoadOp::Load : LoadOp::Clear,
                                                   StoreOp::Store,
                                                   LoadOp::Clear,
                                                   StoreOp::Store);
      vlogVulkanLease("transparency_resolve", *sceneOutLease);
      recordInVulkanFrame(
        m_rendererBase,
        [&]() {
          m_rendererBase.renderVulkan(eye, m_alphaBlendRenderer);
        },
        "transparency_resolve");
    }

    imagesIntegratedViaOIT = true;
  } else {
    // No OIT path: background + geometry via single driver call
    executeCompositorPassesVulkan(opaqueFilters,
                                  transparentFilters,
                                  *sceneOutLease,
                                  eye,
                                  /*includeGeometry*/ true,
                                  /*clearAtStart*/ true,
                                  /*drawBackground*/ true);
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
      auto glowGeomLease = pool.acquireTempRenderTarget2D(targetSize,
                                                          ScratchFormat::RGBA16,
                                                          ScratchFormat::Depth32F,
                                                          RenderBackend::Vulkan);
      // Clear temp attachments, record filter geometry

      m_rendererBase.setActiveSurfaceWithLoadStore(glowGeomLease,
                                                   LoadOp::Clear,
                                                   StoreOp::Store,
                                                   LoadOp::Clear,
                                                   StoreOp::Store);
      vlogVulkanLease("glow_geometry", glowGeomLease);
      recordInVulkanFrame(
        m_rendererBase,
        [&]() {
          filter->renderOpaque(eye);
        },
        "glow_geometry");

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
      colorHandle.backend = AttachmentBackend::Vulkan;
      colorHandle.index = 0;
      colorHandle.id = reinterpret_cast<uint64_t>(glowGeomLease.colorAttachment(0));
      AttachmentHandle depthHandle;
      depthHandle.backend = AttachmentBackend::Vulkan;
      depthHandle.index = 0;
      depthHandle.id = reinterpret_cast<uint64_t>(glowGeomLease.depthAttachmentTexture());

      // Do not clear when overlaying glow; enqueue a glow batch directly
      m_rendererBase.setActiveSurfaceWithLoadStore(*sceneOutLease,
                                                   LoadOp::Load,
                                                   StoreOp::Store,
                                                   LoadOp::DontCare,
                                                   StoreOp::Store);
      vlogVulkanLease("glow_composite", *sceneOutLease);
      recordInVulkanFrame(
        m_rendererBase,
        [&]() {
          TextureGlowPayload glowPayload;
          glowPayload.colorAttachmentHandle = colorHandle;
          glowPayload.depthAttachmentHandle = depthHandle;
          glowPayload.mode = m_glowRenderer.glowMode();
          glowPayload.blurRadius = m_glowRenderer.blurRadius();
          glowPayload.blurScale = m_glowRenderer.blurScale();
          glowPayload.blurStrength = m_glowRenderer.blurStrength();

          RenderBatch batch;
          batch.eye = eye;
          // Let backend fill current active surface into batch if not set explicitly
          batch.draw.topology = PrimitiveTopology::TriangleStrip;
          batch.draw.vertexCount = 4;
          batch.draw.indexCount = 0;
          batch.geometry = std::move(glowPayload);
          m_rendererBase.appendBatch(std::move(batch));
        },
        "glow_composite");
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
    layers.reserve(vFilters.size());

    const auto imageLayers = collectNonOpaqueImageLayers(eye);
    for (const auto& layer : imageLayers) {
      const auto& colorDesc = layer.colorAttachment;
      const auto& depthDesc = layer.depthAttachment;
      if (colorDesc.handle.backend != AttachmentBackend::Vulkan || !colorDesc.handle.valid()) {
        continue;
      }
      if (depthDesc.handle.backend != AttachmentBackend::Vulkan || !depthDesc.handle.valid()) {
        continue;
      }
      layers.emplace_back(colorDesc.handle, depthDesc.handle);
    }

    if (!layers.empty()) {
      const glm::uvec2 tgtSize = outLease->descriptor.size;

      // Helper to build handles from a lease
      auto handlesFromLease = [](const Z3DScratchResourcePool::RenderTargetLease& lease) -> LayerHandles {
        AttachmentHandle c{};
        c.backend = AttachmentBackend::Vulkan;
        c.index = 0;
        c.id = reinterpret_cast<uint64_t>(lease.colorAttachment(0));
        AttachmentHandle d{};
        d.backend = AttachmentBackend::Vulkan;
        d.index = 0;
        d.id = reinterpret_cast<uint64_t>(lease.depthAttachmentTexture());
        return {c, d};
      };

      if (layers.size() == 1) {
        // Blend geometry (current out) with the single image layer
        AttachmentHandle outC{};
        outC.backend = AttachmentBackend::Vulkan;
        outC.index = 0;
        outC.id = reinterpret_cast<uint64_t>(sceneOutLease->colorAttachment(0));
        AttachmentHandle outD{};
        outD.backend = AttachmentBackend::Vulkan;
        outD.index = 0;
        outD.id = reinterpret_cast<uint64_t>(sceneOutLease->depthAttachmentTexture());

        m_rendererBase.setActiveSurfaceWithLoadStore(*sceneOutLease, Z3DRendererBase::Preserve);
        recordInVulkanFrame(
          m_rendererBase,
          [&]() {
            m_alphaBlendRenderer.setSourceAttachments0(outC, outD);
            m_alphaBlendRenderer.setSourceAttachments1(layers[0].first, layers[0].second);
            m_rendererBase.renderVulkan(eye, m_alphaBlendRenderer);
          },
          "image_blend_single");
      } else {
        // Merge N image layers pairwise into an intermediate lease, then blend onto out
        auto mergeLeaseA = pool.acquireTempRenderTarget2D(tgtSize,
                                                          ScratchFormat::RGBA16,
                                                          ScratchFormat::Depth32F,
                                                          RenderBackend::Vulkan);
        auto mergeLeaseB = pool.acquireTempRenderTarget2D(tgtSize,
                                                          ScratchFormat::RGBA16,
                                                          ScratchFormat::Depth32F,
                                                          RenderBackend::Vulkan);

        // First merge: layers[0] and layers[1] -> A
        m_rendererBase.setActiveSurfaceWithLoadStore(mergeLeaseA,
                                                     LoadOp::Clear,
                                                     StoreOp::Store,
                                                     LoadOp::Clear,
                                                     StoreOp::Store);
        recordInVulkanFrame(
          m_rendererBase,
          [&]() {
            m_MIPImageAlphaBlendRenderer.setSourceAttachments0(layers[0].first, layers[0].second);
            m_MIPImageAlphaBlendRenderer.setSourceAttachments1(layers[1].first, layers[1].second);
            m_rendererBase.renderVulkan(eye, m_MIPImageAlphaBlendRenderer);
          },
          "image_merge_initial");

        // Subsequent merges: (A + layers[i]) -> B, then swap
        auto resHandles = handlesFromLease(mergeLeaseA);
        for (size_t i = 2; i < layers.size(); ++i) {
          m_rendererBase.setActiveSurfaceWithLoadStore(mergeLeaseB,
                                                       LoadOp::Clear,
                                                       StoreOp::Store,
                                                       LoadOp::Clear,
                                                       StoreOp::Store);
          recordInVulkanFrame(
            m_rendererBase,
            [&]() {
              m_MIPImageAlphaBlendRenderer.setSourceAttachments0(resHandles.first, resHandles.second);
              m_MIPImageAlphaBlendRenderer.setSourceAttachments1(layers[i].first, layers[i].second);
              m_rendererBase.renderVulkan(eye, m_MIPImageAlphaBlendRenderer);
            },
            "image_merge_iter");
          // swap A<->B and update resHandles
          std::swap(mergeLeaseA, mergeLeaseB);
          resHandles = handlesFromLease(mergeLeaseA);
        }

        // Blend geometry (out) with final merged image (res in mergeLeaseA)
        AttachmentHandle outC{};
        outC.backend = AttachmentBackend::Vulkan;
        outC.index = 0;
        outC.id = reinterpret_cast<uint64_t>(sceneOutLease->colorAttachment(0));
        AttachmentHandle outD{};
        outD.backend = AttachmentBackend::Vulkan;
        outD.index = 0;
        outD.id = reinterpret_cast<uint64_t>(sceneOutLease->depthAttachmentTexture());

        m_rendererBase.setActiveSurfaceWithLoadStore(*sceneOutLease, Z3DRendererBase::Preserve);
        // No clear when blending onto output
        recordInVulkanFrame(
          m_rendererBase,
          [&]() {
            m_alphaBlendRenderer.setSourceAttachments0(outC, outD);
            m_alphaBlendRenderer.setSourceAttachments1(resHandles.first, resHandles.second);
            m_rendererBase.render(eye, m_alphaBlendRenderer);
          },
          "image_merge_finalize");
      }
    }
  }

  // Render stay-on-top filters by composing them into a separate lease,
  // then alpha-blending the result on top of the final scene. This ensures
  // on-top content is not depth-tested against previously rendered geometry.
  if (!onTopOpaqueFilters.empty() || !onTopTransparentFilters.empty()) {
    const glm::uvec2 targetSize = sceneOutLease->descriptor.size;
    const bool useOITOnTop = m_rendererBase.sceneState().transparency == TransparencyMode::DualDepthPeeling ||
                             m_rendererBase.sceneState().transparency == TransparencyMode::WeightedAverage ||
                             m_rendererBase.sceneState().transparency == TransparencyMode::WeightedBlended;

    if (!useOITOnTop) {
      // Non-OIT: render all on-top (opaque+transparent) into a temp lease, then overlay.
      auto onTopLease = pool.acquireTempRenderTarget2D(targetSize,
                                                       ScratchFormat::RGBA16,
                                                       ScratchFormat::Depth32F,
                                                       RenderBackend::Vulkan);
      Z3DCompositorPass pass;
      pass.targetLease = &onTopLease;
      pass.surface = m_rendererBase.describeSurface(onTopLease);
      pass.eye = eye;
      pass.transparency = m_rendererBase.sceneState().transparency;
      pass.msaaMode = m_rendererBase.sceneState().multisample;
      pass.clearColor = true;
      pass.clearDepth = true;
      pass.clearStencil = false;
      pass.clearValue.color = glm::vec4(0.0f);
      pass.clearValue.depth = 1.0f;
      pass.clearValue.stencil = 0u;
      pass.opaqueFilters.assign(onTopOpaqueFilters.begin(), onTopOpaqueFilters.end());
      pass.transparentFilters.reserve(onTopTransparentFilters.size());
      for (auto* f : onTopTransparentFilters) {
        Z3DCompositorTransparentBatch tb;
        tb.filter = f;
        tb.glowEnabled = false;
        pass.transparentFilters.push_back(tb);
      }
      pass.debugLabel = "geometry_on_top_blend";
      m_rendererBase.executeCompositorPass(pass);

      // Overlay onto scene output using "first on top" blending
      AttachmentHandle overlayC{};
      overlayC.backend = AttachmentBackend::Vulkan;
      overlayC.index = 0;
      overlayC.id = reinterpret_cast<uint64_t>(onTopLease.colorAttachment(0));
      AttachmentHandle overlayD{};
      overlayD.backend = AttachmentBackend::Vulkan;
      overlayD.index = 0;
      overlayD.id = reinterpret_cast<uint64_t>(onTopLease.depthAttachmentTexture());

      AttachmentHandle outC{};
      outC.backend = AttachmentBackend::Vulkan;
      outC.index = 0;
      outC.id = reinterpret_cast<uint64_t>(sceneOutLease->colorAttachment(0));
      AttachmentHandle outD{};
      outD.backend = AttachmentBackend::Vulkan;
      outD.index = 0;
      outD.id = reinterpret_cast<uint64_t>(sceneOutLease->depthAttachmentTexture());

      m_rendererBase.setActiveSurfaceWithLoadStore(*sceneOutLease,
                                                   LoadOp::Load,
                                                   StoreOp::Store,
                                                   LoadOp::Clear,
                                                   StoreOp::Store);

      recordInVulkanFrame(
        m_rendererBase,
        [&]() {
          m_firstOnTopBlendRenderer.setSourceAttachments0(overlayC, overlayD);
          m_firstOnTopBlendRenderer.setSourceAttachments1(outC, outD);
          m_rendererBase.renderVulkan(eye, m_firstOnTopBlendRenderer);
        },
        "on_top_overlay_blend");

      // Apply glow for on-top filters
      applyGlowOverlay(onTopTransparentFilters);
      applyGlowOverlay(onTopOpaqueFilters);
    } else {
      // OIT path: split opaque/transparent, run OIT for transparent with opaque depth, then overlay
      Z3DScratchResourcePool::RenderTargetLease onTopOpaqueLease;
      if (!onTopOpaqueFilters.empty()) {
        onTopOpaqueLease = pool.acquireTempRenderTarget2D(targetSize,
                                                          ScratchFormat::RGBA16,
                                                          ScratchFormat::Depth32F,
                                                          RenderBackend::Vulkan);
        Z3DCompositorPass pass;
        pass.targetLease = &onTopOpaqueLease;
        pass.surface = m_rendererBase.describeSurface(onTopOpaqueLease);
        pass.eye = eye;
        pass.transparency = m_rendererBase.sceneState().transparency;
        pass.msaaMode = m_rendererBase.sceneState().multisample;
        pass.clearColor = true;
        pass.clearDepth = true;
        pass.clearStencil = false;
        pass.clearValue.color = glm::vec4(0.0f);
        pass.clearValue.depth = 1.0f;
        pass.clearValue.stencil = 0u;
        pass.opaqueFilters.assign(onTopOpaqueFilters.begin(), onTopOpaqueFilters.end());
        pass.transparentFilters.clear();
        pass.debugLabel = "geometry_on_top_opaque";
        m_rendererBase.executeCompositorPass(pass);
      }

      auto handlesFromLease = [](const Z3DScratchResourcePool::RenderTargetLease& lease) {
        AttachmentHandle c{};
        c.backend = AttachmentBackend::Vulkan;
        c.index = 0;
        c.id = reinterpret_cast<uint64_t>(lease.colorAttachment(0));
        AttachmentHandle d{};
        d.backend = AttachmentBackend::Vulkan;
        d.index = 0;
        d.id = reinterpret_cast<uint64_t>(lease.depthAttachmentTexture());
        return std::make_pair(c, d);
      };

      // OIT for on-top transparent filters into a temporary lease
      Z3DScratchResourcePool::RenderTargetLease onTopTransLease;
      const auto transparencyModeOnTop = m_rendererBase.sceneState().transparency;
      const bool onTopHasTrans = !onTopTransparentFilters.empty();
      const bool onTopHasOpaque = !onTopOpaqueFilters.empty();
      if (onTopHasTrans) {
        onTopTransLease = pool.acquireTempRenderTarget2D(targetSize,
                                                         ScratchFormat::RGBA16,
                                                         ScratchFormat::Depth32F,
                                                         RenderBackend::Vulkan);
        AttachmentHandle depthHandle{};
        if (onTopHasOpaque) {
          depthHandle.backend = AttachmentBackend::Vulkan;
          depthHandle.index = 0;
          depthHandle.id = reinterpret_cast<uint64_t>(onTopOpaqueLease.depthAttachmentTexture());
        }
        // Restrict image layers to stay-on-top volumes
        std::vector<Z3DCompositorImageLayer> onTopImageLayers;
        for (auto* vf : m_vPPort.connectedFilters()) {
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
                                       onTopTransLease,
                                       eye,
                                       depthHandle,
                                       onTopImageLayers,
                                       /*clearResolveTarget=*/true);
            break;
          case TransparencyMode::WeightedAverage:
            renderTransparentWAVulkan(onTopTransparentFilters,
                                      onTopTransLease,
                                      eye,
                                      depthHandle,
                                      onTopImageLayers,
                                      /*clearResolveTarget=*/true);
            break;
          case TransparencyMode::WeightedBlended:
            renderTransparentWBVulkan(onTopTransparentFilters,
                                      onTopTransLease,
                                      eye,
                                      depthHandle,
                                      onTopImageLayers,
                                      /*clearResolveTarget=*/true);
            break;
          default:
            break;
        }
      }

      // Determine the overlay handles (opaque-only, trans-only, or merged both)
      std::pair<AttachmentHandle, AttachmentHandle> overlayHandles{};
      if (onTopHasOpaque && onTopHasTrans) {
        auto mergeLease = pool.acquireTempRenderTarget2D(targetSize,
                                                         ScratchFormat::RGBA16,
                                                         ScratchFormat::Depth32F,
                                                         RenderBackend::Vulkan);
        // Clear merge target
        m_rendererBase.setActiveSurfaceWithLoadStore(mergeLease,
                                                     LoadOp::Clear,
                                                     StoreOp::Store,
                                                     LoadOp::Clear,
                                                     StoreOp::Store);
        const auto opaqueHandles = handlesFromLease(onTopOpaqueLease);
        const auto transHandles = handlesFromLease(onTopTransLease);

        recordInVulkanFrame(
          m_rendererBase,
          [&]() {
            // Depth-tested alpha blend to pick per-pixel front-most between opaque and transparent
            m_alphaBlendRenderer.setSourceAttachments0(opaqueHandles.first, opaqueHandles.second);
            m_alphaBlendRenderer.setSourceAttachments1(transHandles.first, transHandles.second);
            m_rendererBase.renderVulkan(eye, m_alphaBlendRenderer);
          },
          "on_top_merge");

        overlayHandles = handlesFromLease(mergeLease);
      } else if (onTopHasTrans) {
        overlayHandles = handlesFromLease(onTopTransLease);
      } else if (onTopHasOpaque) {
        overlayHandles = handlesFromLease(onTopOpaqueLease);
      }

      // Blend overlayHandles onto the final scene surface
      if (overlayHandles.first.valid()) {
        AttachmentHandle outC{};
        outC.backend = AttachmentBackend::Vulkan;
        outC.index = 0;
        outC.id = reinterpret_cast<uint64_t>(sceneOutLease->colorAttachment(0));
        AttachmentHandle outD{};
        outD.backend = AttachmentBackend::Vulkan;
        outD.index = 0;
        outD.id = reinterpret_cast<uint64_t>(sceneOutLease->depthAttachmentTexture());

        // Preserve existing color, clear depth
        m_rendererBase.setActiveSurfaceWithLoadStore(*sceneOutLease,
                                                     LoadOp::Load,
                                                     StoreOp::Store,
                                                     LoadOp::Clear,
                                                     StoreOp::Store);

        recordInVulkanFrame(
          m_rendererBase,
          [&]() {
            // First-on-top blending: overlay (0) over scene (1)
            m_firstOnTopBlendRenderer.setSourceAttachments0(overlayHandles.first, overlayHandles.second);
            m_firstOnTopBlendRenderer.setSourceAttachments1(outC, outD);
            m_rendererBase.renderVulkan(eye, m_firstOnTopBlendRenderer);
          },
          "on_top_overlay");
      }

      // Apply glow for on-top filters
      applyGlowOverlay(onTopTransparentFilters);
      applyGlowOverlay(onTopOpaqueFilters);
    }
  }

  if (m_showAxis.get()) {
    renderAxisVulkan(eye, *sceneOutLease);
  }

  // Vulkan picking (render to RGBA8+Depth24 Vulkan scratch image)
  {
    std::vector<Z3DBoundedFilter*> showHandleFilters;
    showHandleFilters.reserve(gFilters.size() + vFilters.size());
    for (auto* v : vFilters) {
      if (v && v->isSelected() && v->isTransformEnabled()) {
        showHandleFilters.push_back(v);
      }
    }
    for (auto* gf : gFilters) {
      if (gf && gf->isSelected() && gf->isTransformEnabled()) {
        showHandleFilters.push_back(gf);
      }
    }

    const glm::uvec2 pickSize = sceneOutLease->descriptor.size;
    ensurePickingTargetVulkan(pickSize);

    // Collect picking batches from a filter's own renderer base and append
    // them to the compositor's renderer base under the current active surface.
    auto recordFilterPickingBatches = [&](Z3DBoundedFilter* filter, auto&& renderFn) {
      if (!filter) {
        return;
      }
      auto& source = filter->rendererBase();

      const glm::uvec4 previousViewport = source.frameState().viewport;
      const auto previousSurface = source.frameState().activeSurface;
      const glm::uvec4 pickViewport(0u, 0u, pickSize.x, pickSize.y);
      const auto surfaceCopy = m_rendererBase.frameState().activeSurface;

      source.frameState().updateViewportData(pickViewport);
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
          batch.pass.viewport.extent = glm::vec2(static_cast<float>(pickSize.x), static_cast<float>(pickSize.y));
          batch.pass.viewport.minDepth = 0.0f;
          batch.pass.viewport.maxDepth = 1.0f;
        }
        m_rendererBase.appendBatch(std::move(batch));
      }
      source.resetCPUState();

      source.frameState().updateViewportData(previousViewport);
      source.setActiveSurfaceWithLoadStore(previousSurface, Z3DRendererBase::Preserve);
    };

    if (gFilters.empty() && !showHandleFilters.empty()) {
      m_rendererBase.setActiveSurfaceWithLoadStore(m_pickingTargetLease,
                                                   LoadOp::Clear,
                                                   StoreOp::Store,
                                                   LoadOp::Clear,
                                                   StoreOp::Store);
      vlogVulkanLease("picking_handles", m_pickingTargetLease);

      recordInVulkanFrame(
        m_rendererBase,
        [&]() {
          for (auto* f : showHandleFilters) {
            f->setViewport(pickSize);
            recordFilterPickingBatches(f, [&]() {
              f->renderHandlePicking(eye);
            });
          }
        },
        "picking_handles");

    } else if (showHandleFilters.empty() && !gFilters.empty()) {
      m_rendererBase.setActiveSurfaceWithLoadStore(m_pickingTargetLease,
                                                   LoadOp::Clear,
                                                   StoreOp::Store,
                                                   LoadOp::Clear,
                                                   StoreOp::Store);
      vlogVulkanLease("picking_geometry", m_pickingTargetLease);

      recordInVulkanFrame(
        m_rendererBase,
        [&]() {
          for (auto* gf : gFilters) {
            if (gf && gf->isReady(eye)) {
              gf->setViewport(pickSize);
              recordFilterPickingBatches(gf, [&]() {
                gf->renderPicking(eye);
              });
            }
          }
        },
        "picking_geometry");

    } else if (!gFilters.empty() && !showHandleFilters.empty()) {
      auto leaseHandles =
        pool.acquireTempRenderTarget2D(pickSize, ScratchFormat::RGBA16, ScratchFormat::Depth32F, RenderBackend::Vulkan);
      auto leaseGeoms =
        pool.acquireTempRenderTarget2D(pickSize, ScratchFormat::RGBA16, ScratchFormat::Depth32F, RenderBackend::Vulkan);

      // Record handle picking

      m_rendererBase.setActiveSurfaceWithLoadStore(leaseHandles,
                                                   LoadOp::Clear,
                                                   StoreOp::Store,
                                                   LoadOp::Clear,
                                                   StoreOp::Store);
      vlogVulkanLease("picking_handles_temp", leaseHandles);

      recordInVulkanFrame(
        m_rendererBase,
        [&]() {
          for (auto* f : showHandleFilters) {
            f->setViewport(pickSize);
            recordFilterPickingBatches(f, [&]() {
              f->renderHandlePicking(eye);
            });
          }
        },
        "picking_handles_temp");

      // Record geometry picking
      m_rendererBase.setActiveSurfaceWithLoadStore(leaseGeoms,
                                                   LoadOp::Clear,
                                                   StoreOp::Store,
                                                   LoadOp::Clear,
                                                   StoreOp::Store);
      vlogVulkanLease("picking_geometry_temp", leaseGeoms);

      recordInVulkanFrame(
        m_rendererBase,
        [&]() {
          for (auto* gf : gFilters) {
            if (gf && gf->isReady(eye)) {
              gf->setViewport(pickSize);
              recordFilterPickingBatches(gf, [&]() {
                gf->renderPicking(eye);
              });
            }
          }
        },
        "picking_geometry_temp");

      // Composite into picking target using first-on-top blend
      AttachmentHandle handlesColor{};
      handlesColor.backend = AttachmentBackend::Vulkan;
      handlesColor.index = 0;
      handlesColor.id = reinterpret_cast<uint64_t>(leaseHandles.colorAttachment(0));
      AttachmentHandle handlesDepth{};
      handlesDepth.backend = AttachmentBackend::Vulkan;
      handlesDepth.index = 0;
      handlesDepth.id = reinterpret_cast<uint64_t>(leaseHandles.depthAttachmentTexture());
      AttachmentHandle geomsColor{};
      geomsColor.backend = AttachmentBackend::Vulkan;
      geomsColor.index = 0;
      geomsColor.id = reinterpret_cast<uint64_t>(leaseGeoms.colorAttachment(0));
      AttachmentHandle geomsDepth{};
      geomsDepth.backend = AttachmentBackend::Vulkan;
      geomsDepth.index = 0;
      geomsDepth.id = reinterpret_cast<uint64_t>(leaseGeoms.depthAttachmentTexture());

      m_firstOnTopRenderer.setSourceAttachments0(handlesColor, handlesDepth);
      m_firstOnTopRenderer.setSourceAttachments1(geomsColor, geomsDepth);

      m_rendererBase.setActiveSurfaceWithLoadStore(m_pickingTargetLease,
                                                   LoadOp::Clear,
                                                   StoreOp::Store,
                                                   LoadOp::Clear,
                                                   StoreOp::Store);
      recordInVulkanFrame(
        m_rendererBase,
        [&]() {
          m_rendererBase.renderVulkan(eye, m_firstOnTopRenderer);
        },
        "picking_composite");
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

  // Downsample supersampled scene into the compositor out surface
  if (supersample2x2) {
    m_rendererBase.setActiveSurfaceWithLoadStore(*outLease,
                                                 LoadOp::Clear,
                                                 StoreOp::Store,
                                                 LoadOp::Clear,
                                                 StoreOp::Store);
    recordInVulkanFrame(
      m_rendererBase,
      [&]() {
        AttachmentHandle srcColor{};
        srcColor.backend = AttachmentBackend::Vulkan;
        srcColor.index = 0;
        srcColor.id = reinterpret_cast<uint64_t>(sceneOutLease->colorAttachment(0));
        AttachmentHandle srcDepth{};
        srcDepth.backend = AttachmentBackend::Vulkan;
        srcDepth.index = 0;
        srcDepth.id = reinterpret_cast<uint64_t>(sceneOutLease->depthAttachmentTexture());
        // Internal resolve: no Y-flip
        m_textureCopyRenderer.setFlipY(false);
        m_textureCopyRenderer.setSourceAttachments(srcColor, srcDepth);
        m_rendererBase.renderVulkan(eye, m_textureCopyRenderer);
      },
      "supersample_resolve");
  }

  // Per-pass recording ends any frame it began

  // Finalize: enqueue end-of-frame readback to staging and perform CPU copy after fence.
  {
    ZVulkanTexture* finalColor = outLease->colorAttachment(0);
    // Request readback while a Vulkan frame is active so backend can insert the copy before endRender.
    // If final color is not RGBA8, first render a copy to an RGBA8 scratch surface, then enqueue the readback inside
    // the same frame.
    if (finalColor && finalColor->format() != vk::Format::eR8G8B8A8Unorm) {
      auto rgba8Lease = pool.acquireTempRenderTarget2D(m_outputSize,
                                                       ScratchFormat::RGBA8,
                                                       ScratchFormat::Depth32F,
                                                       RenderBackend::Vulkan);
      if (rgba8Lease && rgba8Lease.backend == RenderBackend::Vulkan) {
        m_rendererBase.setActiveSurfaceWithLoadStore(rgba8Lease,
                                                     LoadOp::Clear,
                                                     StoreOp::Store,
                                                     LoadOp::Clear,
                                                     StoreOp::Store);

        recordInVulkanFrame(
          m_rendererBase,
          [&]() {
            // GPU copy to RGBA8 first
            AttachmentHandle srcColor{};
            srcColor.backend = AttachmentBackend::Vulkan;
            srcColor.index = 0;
            srcColor.id = reinterpret_cast<uint64_t>(finalColor);
            AttachmentHandle srcDepth{};
            srcDepth.backend = AttachmentBackend::Vulkan;
            srcDepth.index = 0;
            if (outLease && outLease->depthAttachmentTexture()) {
              srcDepth.id = reinterpret_cast<uint64_t>(outLease->depthAttachmentTexture());
            }
            CHECK(srcColor.id != 0 && srcDepth.id != 0) << "final_rgba8_copy: missing source attachments";
            VLOG(1) << fmt::format("VK final_rgba8_copy srcColor=0x{:x} srcDepth=0x{:x}",
                                   static_cast<uint64_t>(srcColor.id),
                                   static_cast<uint64_t>(srcDepth.id));
            m_textureCopyRenderer.setFlipY(FLAGS_atlas_vk_copy_yflip_in_shader);
            m_textureCopyRenderer.setSourceAttachments(srcColor, srcDepth);
            m_rendererBase.renderVulkan(eye, m_textureCopyRenderer);
            // Enqueue readback while this frame is active
            if (auto* backend = Z3DRendererVulkanBackend::current()) {
              ZVulkanTexture* rgba8Tex = rgba8Lease.colorAttachment(0);
              auto* localPtr = (eye == MonoEye)   ? m_monoCurrentLocalBuffer
                               : (eye == LeftEye) ? m_leftCurrentLocalBuffer
                                                  : m_rightCurrentLocalBuffer;
              const Z3DEye eyeCopy = eye;
              VLOG(1) << fmt::format("VK enqueue final readback tex=0x{:x} size={}x{} eye={}",
                                     reinterpret_cast<uint64_t>(rgba8Tex),
                                     rgba8Tex ? rgba8Tex->width() : 0,
                                     rgba8Tex ? rgba8Tex->height() : 0,
                                     static_cast<int>(eyeCopy));
              backend->requestEndOfFrameColorReadback(
                *rgba8Tex,
                eyeCopy,
                [this, localPtr, eyeCopy, &pool](const void* mapped,
                                                 size_t /*bytes*/,
                                                 vk::Format /*fmt*/,
                                                 glm::uvec2 size,
                                                 std::function<void()> releaseSlot) {
                  if (localPtr->externalRelease) {
                    localPtr->externalRelease();
                    localPtr->externalRelease = {};
                  }
                  localPtr->external = static_cast<const uint8_t*>(mapped);
                  localPtr->externalStride = static_cast<size_t>(size.x) * 4u;
                  localPtr->externalRelease = std::move(releaseSlot);
                  localPtr->width = size.x;
                  localPtr->height = size.y;
                  VLOG(1) << fmt::format("VK final readback ready mapped={} size={}x{} eye={}",
                                         mapped,
                                         size.x,
                                         size.y,
                                         static_cast<int>(eyeCopy));
                  if (eyeCopy == MonoEye) {
                    {
                      const std::scoped_lock lock(m_globalParameters.targetSwitchMutex);
                      std::swap(m_monoReadyLocalBuffer, m_monoCurrentLocalBuffer);
                      std::swap(m_monoReadyTarget, m_monoCurrentTarget);
                    }
                    m_globalParameters.hasNewRendering = true;
                    static uint64_t s_lastCreate = 0, s_lastChange = 0;
                    const uint64_t curCreate = pool.creationCounter();
                    const uint64_t curChange = pool.changeCounter();
                    if (curCreate != s_lastCreate || curChange != s_lastChange) {
                      VLOG(1) << pool.describeMemoryUsage(true);
                      s_lastCreate = curCreate;
                      s_lastChange = curChange;
                    }
                    VLOG(1) << fmt::format("VK renderingFinished (mono) readyBuffer={}", (void*)m_monoReadyLocalBuffer);
                    Q_EMIT renderingFinished();
                  } else if (eyeCopy == LeftEye) {
                    const std::scoped_lock lock(m_globalParameters.targetSwitchMutex);
                    std::swap(m_leftReadyLocalBuffer, m_leftCurrentLocalBuffer);
                    std::swap(m_leftReadyTarget, m_leftCurrentTarget);
                  } else {
                    {
                      const std::scoped_lock lock(m_globalParameters.targetSwitchMutex);
                      std::swap(m_rightReadyLocalBuffer, m_rightCurrentLocalBuffer);
                      std::swap(m_rightReadyTarget, m_rightCurrentTarget);
                    }
                    m_globalParameters.hasNewRendering = true;
                    VLOG(1) << "VK renderingFinished (right)";
                    Q_EMIT renderingFinished();
                  }
                });
            }
          },
          "final_rgba8_copy_and_readback");
      }
    } else if (finalColor) {
      // No conversion needed; open a mini frame to enqueue readback

      recordInVulkanFrame(
        m_rendererBase,
        [&]() {
          if (auto* backend = Z3DRendererVulkanBackend::current()) {
            auto* localPtr = (eye == MonoEye)   ? m_monoCurrentLocalBuffer
                             : (eye == LeftEye) ? m_leftCurrentLocalBuffer
                                                : m_rightCurrentLocalBuffer;
            const Z3DEye eyeCopy = eye;
            VLOG(1) << fmt::format("VK enqueue final readback (no copy) tex=0x{:x} size={}x{} eye={}",
                                   reinterpret_cast<uint64_t>(finalColor),
                                   finalColor ? finalColor->width() : 0,
                                   finalColor ? finalColor->height() : 0,
                                   static_cast<int>(eyeCopy));
            backend->requestEndOfFrameColorReadback(
              *finalColor,
              eyeCopy,
              [this, localPtr, eyeCopy, &pool](const void* mapped,
                                               size_t /*bytes*/,
                                               vk::Format /*fmt*/,
                                               glm::uvec2 size,
                                               std::function<void()> releaseSlot) {
                if (localPtr->externalRelease) {
                  localPtr->externalRelease();
                  localPtr->externalRelease = {};
                }
                localPtr->external = static_cast<const uint8_t*>(mapped);
                localPtr->externalStride = static_cast<size_t>(size.x) * 4u;
                localPtr->externalRelease = std::move(releaseSlot);
                localPtr->width = size.x;
                localPtr->height = size.y;
                VLOG(1) << fmt::format("VK final readback ready (no copy) mapped={} size={}x{} eye={}",
                                       mapped,
                                       size.x,
                                       size.y,
                                       static_cast<int>(eyeCopy));
                if (eyeCopy == MonoEye) {
                  {
                    const std::scoped_lock lock(m_globalParameters.targetSwitchMutex);
                    std::swap(m_monoReadyLocalBuffer, m_monoCurrentLocalBuffer);
                    std::swap(m_monoReadyTarget, m_monoCurrentTarget);
                  }
                  m_globalParameters.hasNewRendering = true;
                  static uint64_t s_lastCreate = 0, s_lastChange = 0;
                  const uint64_t curCreate = pool.creationCounter();
                  const uint64_t curChange = pool.changeCounter();
                  if (curCreate != s_lastCreate || curChange != s_lastChange) {
                    VLOG(1) << pool.describeMemoryUsage(true);
                    s_lastCreate = curCreate;
                    s_lastChange = curChange;
                  }
                  VLOG(1) << fmt::format("VK renderingFinished (mono, no copy) readyBuffer={}",
                                         (void*)m_monoReadyLocalBuffer);
                  Q_EMIT renderingFinished();
                } else if (eyeCopy == LeftEye) {
                  const std::scoped_lock lock(m_globalParameters.targetSwitchMutex);
                  std::swap(m_leftReadyLocalBuffer, m_leftCurrentLocalBuffer);
                  std::swap(m_leftReadyTarget, m_leftCurrentTarget);
                } else {
                  {
                    const std::scoped_lock lock(m_globalParameters.targetSwitchMutex);
                    std::swap(m_rightReadyLocalBuffer, m_rightCurrentLocalBuffer);
                    std::swap(m_rightReadyTarget, m_rightCurrentTarget);
                  }
                  m_globalParameters.hasNewRendering = true;
                  VLOG(1) << "VK renderingFinished (right, no copy)";
                  Q_EMIT renderingFinished();
                }
              });
          }
        },
        "readback_enqueue");
    }
    // Do not enqueue picking readback; rely on synchronous 1x1 reads on demand.
  }

  return 1.0;
}

void Z3DCompositor::updateSize()
{
  for (auto port : m_inputPorts) {
    port->setExpectedSize(outputSize());
  }

  invalidate(State::AllResultInvalid);
}

void Z3DCompositor::executeCompositorPassesVulkan(const std::vector<Z3DBoundedFilter*>& opaqueFilters,
                                                  const std::vector<Z3DBoundedFilter*>& transparentFilters,
                                                  Z3DScratchResourcePool::RenderTargetLease& sceneOutLease,
                                                  Z3DEye eye,
                                                  bool includeGeometry,
                                                  bool clearAtStart,
                                                  bool drawBackground)
{
  // Set initial surface with optional clear at start
  m_rendererBase.setActiveSurfaceWithLoadStore(sceneOutLease,
                                               clearAtStart ? LoadOp::Clear : LoadOp::Load,
                                               StoreOp::Store,
                                               clearAtStart ? LoadOp::Clear : LoadOp::Load,
                                               StoreOp::Store);

  // First node: background (clear then draw or uniform clear when hidden)
  if (drawBackground && m_showBackground.get()) {
    vlogVulkanLease("background", sceneOutLease);
    recordInVulkanFrame(
      m_rendererBase,
      [&]() {
        m_rendererBase.renderVulkan(eye, m_backgroundRenderer);
      },
      "background");
  }

  if (includeGeometry) {
    // Build a compositor pass to record opaque + transparent filters in one submission.
    if (!opaqueFilters.empty() || !transparentFilters.empty()) {
      Z3DCompositorPass pass;
      pass.kind = Z3DCompositorPass::Kind::Geometry;
      pass.targetLease = &sceneOutLease;
      pass.surface = m_rendererBase.describeSurface(sceneOutLease);
      pass.eye = eye;
      pass.transparency = m_rendererBase.sceneState().transparency;
      pass.msaaMode = m_rendererBase.sceneState().multisample;
      // If we didn't draw a background into this target but the caller asked
      // for a clear at the start, clear color here so stale contents from a
      // previous frame are not blended under geometry (e.g., opaque-only
      // intermediates before OIT resolution).
      pass.clearColor = (clearAtStart && !drawBackground);
      // Clear depth before drawing scene geometry to avoid any stale
      // depth values from previous frames showing up when content changes
      // (e.g., color edits) but geometry/targets remain the same. Background
      // rendering does not write depth in Vulkan, so clearing here is safe
      // and ensures the very next frame after an edit is fully visible.
      pass.clearDepth = true;
      pass.clearStencil = false;
      pass.clearValue = {};
      pass.opaqueFilters.assign(opaqueFilters.begin(), opaqueFilters.end());
      pass.transparentFilters.reserve(transparentFilters.size());
      for (auto* f : transparentFilters) {
        Z3DCompositorTransparentBatch tb;
        tb.filter = f;
        tb.glowEnabled = false;
        pass.transparentFilters.push_back(tb);
      }
      pass.debugLabel = "geometry";
      vlogVulkanLease("geometry", sceneOutLease);
      m_rendererBase.executeCompositorPass(pass);
    }

    // Finally, glow overlays (applied over the base scene)
    // Helper to collect a filter's batches into the compositor using a specific lease
    auto recordFilterBatchesToLease =
      [&](Z3DBoundedFilter* filter, const Z3DScratchResourcePool::RenderTargetLease& lease, auto&& renderFn) {
        if (!filter) {
          return;
        }
        auto& source = filter->rendererBase();
        const glm::uvec4 previousViewport = source.frameState().viewport;
        const auto previousSurface = source.frameState().activeSurface;
        const auto surfaceCopy = m_rendererBase.describeSurface(lease);
        source.frameState().updateViewportData(m_rendererBase.frameState().viewport);
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
          if (batch.pass.viewport.extent == glm::vec2(0.0f) && m_rendererBase.frameState().viewport.z > 0u &&
              m_rendererBase.frameState().viewport.w > 0u) {
            batch.pass.viewport.origin = glm::vec2(static_cast<float>(m_rendererBase.frameState().viewport.x),
                                                   static_cast<float>(m_rendererBase.frameState().viewport.y));
            batch.pass.viewport.extent = glm::vec2(static_cast<float>(m_rendererBase.frameState().viewport.z),
                                                   static_cast<float>(m_rendererBase.frameState().viewport.w));
            batch.pass.viewport.minDepth = 0.0f;
            batch.pass.viewport.maxDepth = 1.0f;
          }
          m_rendererBase.appendBatch(std::move(batch));
        }
        source.resetCPUState();
        source.frameState().updateViewportData(previousViewport);
        source.setActiveSurfaceWithLoadStore(previousSurface, Z3DRendererBase::Preserve);
      };
    auto overlayGlow = [&](const std::vector<Z3DBoundedFilter*>& filters) {
      if (filters.empty()) {
        return;
      }
      const glm::uvec2 targetSize = sceneOutLease.descriptor.size;
      auto& pool = Z3DRenderGlobalState::instance().scratchPool();
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
        auto glowGeomLease = pool.acquireTempRenderTarget2D(targetSize,
                                                            ScratchFormat::RGBA16,
                                                            ScratchFormat::Depth32F,
                                                            RenderBackend::Vulkan);
        // Clear temp attachments, record filter geometry (submit immediately)
        m_rendererBase.setActiveSurfaceWithLoadStore(glowGeomLease,
                                                     LoadOp::Clear,
                                                     StoreOp::Store,
                                                     LoadOp::Clear,
                                                     StoreOp::Store);
        recordInVulkanFrame(
          m_rendererBase,
          [&]() {
            recordFilterBatchesToLease(filter, glowGeomLease, [&]() {
              filter->renderOpaque(eye);
            });
          },
          "glow_geometry");

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
        colorHandle.backend = AttachmentBackend::Vulkan;
        colorHandle.index = 0;
        colorHandle.id = reinterpret_cast<uint64_t>(glowGeomLease.colorAttachment(0));
        AttachmentHandle depthHandle;
        depthHandle.backend = AttachmentBackend::Vulkan;
        depthHandle.index = 0;
        depthHandle.id = reinterpret_cast<uint64_t>(glowGeomLease.depthAttachmentTexture());

        m_rendererBase.setActiveSurfaceWithLoadStore(sceneOutLease,
                                                     LoadOp::Load,
                                                     StoreOp::Store,
                                                     LoadOp::DontCare,
                                                     StoreOp::Store,
                                                     {});
        recordInVulkanFrame(
          m_rendererBase,
          [&]() {
            TextureGlowPayload glowPayload;
            glowPayload.colorAttachmentHandle = colorHandle;
            glowPayload.depthAttachmentHandle = depthHandle;
            glowPayload.mode = m_glowRenderer.glowMode();
            glowPayload.blurRadius = m_glowRenderer.blurRadius();
            glowPayload.blurScale = m_glowRenderer.blurScale();
            glowPayload.blurStrength = m_glowRenderer.blurStrength();

            RenderBatch batch;
            batch.eye = eye;
            // Fill surface if not already set through active surface
            batch.draw.topology = PrimitiveTopology::TriangleStrip;
            batch.draw.vertexCount = 4;
            batch.draw.indexCount = 0;
            batch.geometry = std::move(glowPayload);
            m_rendererBase.appendBatch(std::move(batch));
          },
          "glow_composite");
      }
    };

    overlayGlow(transparentFilters);
    overlayGlow(opaqueFilters);
  }
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
    m_ddpBlendShader.reset();
    m_ddpFinalShader.reset();
    m_waFinalShader.reset();
    m_wbFinalShader.reset();
  }

  if (backendRequest == RenderBackend::OpenGL) {
    // Rebuild compositor-owned GL shader programs in the new context.
    VLOG(1) << "Recreating compositor-owned GL shader programs after switching to OpenGL";
    m_ddpBlendShader = std::make_unique<Z3DShaderProgram>();
    m_ddpBlendShader->loadFromSourceFile("pass.vert", "dual_peeling_blend.frag", m_rendererBase.generateHeader());

    m_ddpFinalShader = std::make_unique<Z3DShaderProgram>();
    m_ddpFinalShader->loadFromSourceFile("pass.vert", "dual_peeling_final.frag", m_rendererBase.generateHeader());

    m_waFinalShader = std::make_unique<Z3DShaderProgram>();
    m_waFinalShader->loadFromSourceFile("pass.vert", "wavg_final.frag", m_rendererBase.generateHeader());

    m_wbFinalShader = std::make_unique<Z3DShaderProgram>();
    m_wbFinalShader->loadFromSourceFile("pass.vert", "wblended_final.frag", m_rendererBase.generateHeader());
  }

  std::unordered_set<Z3DBoundedFilter*> seen;
  auto registerFilter = [&](Z3DBoundedFilter* filter) {
    if (filter && seen.insert(filter).second) {
      VLOG(1) << fmt::format("Propagating backend to filter {}", static_cast<const void*>(filter));
      filter->switchRendererBackend(backendRequest);
    }
  };

  const auto geometryFilters = m_gPPort.connectedFilters();
  const auto volumeFilters = m_vPPort.connectedFilters();
  VLOG(1) << fmt::format("Notifying {} geometry filters and {} volume filters of backend change",
                         geometryFilters.size(),
                         volumeFilters.size());

  for (auto* filter : geometryFilters) {
    registerFilter(filter);
  }
  for (auto* filter : volumeFilters) {
    registerFilter(filter);
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

    // blend temport3 and temport4 into outport
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
  size_t g_numPasses = 100;

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

    Z3DPrimitiveRenderer::renderScreenQuad(m_screenQuadVAO, *m_ddpBlendShader);
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

  Z3DPrimitiveRenderer::renderScreenQuad(m_screenQuadVAO, *m_ddpFinalShader);
  m_ddpFinalShader->release();
  glTarget->release();

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
  glClearColor(0, 0, 0, 0);
  glClear(GL_COLOR_BUFFER_BIT);

  glBlendFunc(GL_ONE, GL_ONE);
  glEnable(GL_BLEND);

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

  glTarget->bind();
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
  m_waFinalShader->bind();
  m_waFinalShader->bindTexture("ColorTex0", g_accumulationTexId[0]);
  m_waFinalShader->bindTexture("ColorTex1", g_accumulationTexId[1]);

  const glm::uvec2 waSize = waRT.size();
  const glm::vec2 waScreenDimRcp(waSize.x > 0u ? 1.f / static_cast<float>(waSize.x) : 0.f,
                                 waSize.y > 0u ? 1.f / static_cast<float>(waSize.y) : 0.f);
  m_waFinalShader->setScreenDimRCPUniform(waScreenDimRcp);

  Z3DPrimitiveRenderer::renderScreenQuad(m_screenQuadVAO, *m_waFinalShader);
  m_waFinalShader->release();
  glTarget->release();

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

  Z3DPrimitiveRenderer::renderScreenQuad(m_screenQuadVAO, *m_wbFinalShader);
  m_wbFinalShader->release();
  glTarget->release();

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
                                               bool clearResolveTarget)
{
  const glm::uvec2 targetSize = targetLease.descriptor.size;
  auto& ddpLease = ensureDDPRenderTarget(targetSize);
  CHECK(ddpLease.backend == RenderBackend::Vulkan);

  auto* vkBackend = dynamic_cast<Z3DRendererVulkanBackend*>(m_rendererBase.backend());
  const bool occlusionSupported = (vkBackend != nullptr) && vkBackend->supportsOcclusionQueries();

  auto ddpBindings = m_rendererBase.prepareVulkanSurface(ddpLease);
  CHECK(ddpBindings.colorHandles.size() >= 8 && ddpBindings.surface.colorAttachments.size() >= 7)
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
    if (depthAttachmentHandle.valid()) {
      AttachmentDesc desc;
      desc.handle = depthAttachmentHandle;
      desc.loadOp = loadOp;
      desc.storeOp = StoreOp::Store;
      desc.clearValue.depth = 1.0f;
      surface.depthAttachment = desc;
    } else if (surface.depthAttachment) {
      surface.depthAttachment->loadOp = loadOp;
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
  };

  const glm::vec4 depthClear(-1.0f, -1.0f, 0.0f, 0.0f);
  const glm::vec4 zeroClear(0.0f);

  // In Vulkan, fragment outputs at locations 0..N map to the subpass' color
  // attachments in order. For DDP, we write only 3 outputs (depth blender,
  // front blender, back temp). Select exactly those attachments for the
  // current subpass so that locations 0/1/2 route to the desired targets.
  RendererFrameState::ActiveSurface initSurface;
  initSurface.colorAttachments.push_back(ddpBindings.surface.colorAttachments[0]);
  initSurface.colorAttachments.push_back(ddpBindings.surface.colorAttachments[1]);
  initSurface.colorAttachments.push_back(ddpBindings.surface.colorAttachments[2]);
  // Also include the back-blend accumulation buffer (attachment 6) in the
  // first subpass so we can clear it at the start of the frame. Otherwise it
  // would retain values across frames and cause trail/ghost artifacts.
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
    // Clear rule:
    //  - depth blender (0) uses depthClear (-1 in .x)
    //  - front blender (1) and back temp (2) cleared to zero
    //  - back blender (6) must be cleared to zero at the start of frame
    // Note: indexes 3..5 are not part of initSurface; index here is relative
    // to initSurface order, so check actual handle id if needed. Since we
    // explicitly appended [0,1,2,6], map by position:
    const bool isDepthBlender = (i == 0);
    attachment.clearValue.color = isDepthBlender ? depthClear : zeroClear;
  }
  applyDepthAttachment(initSurface, LoadOp::Load);

  m_rendererBase.setActiveSurfaceWithLoadStore(initSurface, Z3DRendererBase::Preserve);

  // Helper to record a filter's batches into the compositor using a specific surface
  auto recordFilterBatchesToSurface =
    [&](Z3DBoundedFilter* filter, const RendererFrameState::ActiveSurface& surface, auto&& renderFn) {
      if (!filter) {
        return;
      }
      auto& source = filter->rendererBase();

      const glm::uvec4 previousViewport = source.frameState().viewport;
      const auto previousSurface = source.frameState().activeSurface;

      source.frameState().updateViewportData(m_rendererBase.frameState().viewport);
      source.setActiveSurfaceWithLoadStore(surface, Z3DRendererBase::Preserve);
      renderFn();
      auto& batches = source.cpuState().batches;
      // Propagate shader-hook parameters (e.g., DDP sampler attachments) from the
      // source filter renderer to the compositor renderer so Vulkan pipelines
      // can access them while recording. Without this, peel stages may sample
      // placeholder textures, producing incorrect colors (e.g., white silhouettes).
      m_rendererBase.shaderHookPara() = source.shaderHookPara();
      for (auto& batch : batches) {
        if (batch.pass.colorAttachments.empty() && !surface.colorAttachments.empty()) {
          batch.pass.colorAttachments = surface.colorAttachments;
        }
        if (!batch.pass.depthAttachment.has_value() && surface.depthAttachment.has_value()) {
          batch.pass.depthAttachment = surface.depthAttachment;
        }
        if (batch.pass.viewport.extent == glm::vec2(0.0f) && m_rendererBase.frameState().viewport.z > 0u &&
            m_rendererBase.frameState().viewport.w > 0u) {
          batch.pass.viewport.origin = glm::vec2(static_cast<float>(m_rendererBase.frameState().viewport.x),
                                                 static_cast<float>(m_rendererBase.frameState().viewport.y));
          batch.pass.viewport.extent = glm::vec2(static_cast<float>(m_rendererBase.frameState().viewport.z),
                                                 static_cast<float>(m_rendererBase.frameState().viewport.w));
          batch.pass.viewport.minDepth = 0.0f;
          batch.pass.viewport.maxDepth = 1.0f;
        }
        m_rendererBase.appendBatch(std::move(batch));
      }
      source.resetCPUState();
      source.frameState().updateViewportData(previousViewport);
      source.setActiveSurfaceWithLoadStore(previousSurface, Z3DRendererBase::Preserve);
    };

  // Geometry init step must see DualDepthPeelingInit on the compositor renderer
  m_rendererBase.setShaderHookType(Z3DRendererBase::ShaderHookType::DualDepthPeelingInit);
  recordInVulkanFrame(
    m_rendererBase,
    [&]() {
      for (auto* filter : filters) {
        if (!filter) {
          continue;
        }
        filter->setViewport(targetSize);
        filter->setShaderHookType(Z3DRendererBase::ShaderHookType::DualDepthPeelingInit);
        recordFilterBatchesToSurface(filter, initSurface, [&]() {
          filter->renderTransparent(eye);
        });
      }
      if (!imageLayers.empty()) {
        m_rendererBase.setShaderHookType(Z3DRendererBase::ShaderHookType::DualDepthPeelingInit);
        for (const auto& layer : imageLayers) {
          const auto& colorDesc = layer.colorAttachment;
          const auto& depthDesc = layer.depthAttachment;
          if (colorDesc.handle.backend != AttachmentBackend::Vulkan || !colorDesc.handle.valid()) {
            continue;
          }
          if (depthDesc.handle.backend != AttachmentBackend::Vulkan || !depthDesc.handle.valid()) {
            continue;
          }
          // Image OIT init copy: do not flip
          m_textureCopyRenderer.setFlipY(false);
          m_textureCopyRenderer.setSourceAttachments(colorDesc.handle, depthDesc.handle);
          m_rendererBase.renderVulkan(eye, m_textureCopyRenderer);
        }
      }
    },
    "transparency_ddp_init");

  resetHooks();

  constexpr size_t kMaxPasses = 100;
  size_t currId = 0;
  size_t executedPasses = 0;
  for (size_t pass = 1; pass < kMaxPasses; ++pass) {
    currId = pass % 2;
    const size_t prevId = 1 - currId;
    const size_t bufOffset = currId * 3;

    // Route locations 0/1/2 to the active ping attachments for this pass.
    RendererFrameState::ActiveSurface peelSurface;
    peelSurface.colorAttachments.push_back(ddpBindings.surface.colorAttachments[bufOffset + 0]);
    peelSurface.colorAttachments.push_back(ddpBindings.surface.colorAttachments[bufOffset + 1]);
    peelSurface.colorAttachments.push_back(ddpBindings.surface.colorAttachments[bufOffset + 2]);
    peelSurface.depthAttachment = ddpBindings.surface.depthAttachment;
    for (size_t i = 0; i < peelSurface.colorAttachments.size(); ++i) {
      auto& attachment = peelSurface.colorAttachments[i];
      attachment.storeOp = StoreOp::Store;
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

    m_rendererBase.setActiveSurfaceWithLoadStore(peelSurface, Z3DRendererBase::Preserve);

    // Geometry peel step must see DualDepthPeelingPeel on the compositor renderer
    m_rendererBase.setShaderHookType(Z3DRendererBase::ShaderHookType::DualDepthPeelingPeel);
    recordInVulkanFrame(
      m_rendererBase,
      [&]() {
        for (auto* filter : filters) {
          if (!filter) {
            continue;
          }
          filter->setViewport(targetSize);
          filter->setShaderHookType(Z3DRendererBase::ShaderHookType::DualDepthPeelingPeel);
          filter->setShaderHookParaDDPDepthBlenderAttachment(depthPing[prevId]);
          filter->setShaderHookParaDDPFrontBlenderAttachment(frontPing[prevId]);
          recordFilterBatchesToSurface(filter, peelSurface, [&]() {
            filter->renderTransparent(eye);
          });
        }
        if (!imageLayers.empty()) {
          m_rendererBase.setShaderHookType(Z3DRendererBase::ShaderHookType::DualDepthPeelingPeel);
          // Ensure image peel path samples the correct blender textures
          // from the previous ping (prevId). Without this, the copy pipeline
          // will bind placeholders and contribute nothing.
          m_rendererBase.setShaderHookParaDDPDepthBlenderAttachment(depthPing[prevId]);
          m_rendererBase.setShaderHookParaDDPFrontBlenderAttachment(frontPing[prevId]);
          for (const auto& layer : imageLayers) {
            const auto& colorDesc = layer.colorAttachment;
            const auto& depthDesc = layer.depthAttachment;
            if (colorDesc.handle.backend != AttachmentBackend::Vulkan || !colorDesc.handle.valid()) {
              continue;
            }
            if (depthDesc.handle.backend != AttachmentBackend::Vulkan || !depthDesc.handle.valid()) {
              continue;
            }
            // Image OIT peel: do not flip
            m_textureCopyRenderer.setFlipY(false);
            m_textureCopyRenderer.setSourceAttachments(colorDesc.handle, depthDesc.handle);
            m_rendererBase.renderVulkan(eye, m_textureCopyRenderer);
          }
        }
      },
      "transparency_ddp_peel");

    resetHooks();

    RendererFrameState::ActiveSurface blendSurface;
    blendSurface.colorAttachments.push_back(ddpBindings.surface.colorAttachments[6]);
    // Accumulate back colors across peel passes within the same frame.
    // Use Load so each pass adds into the existing buffer; we already clear
    // it at frame start during the init subpass above.
    if (!blendSurface.colorAttachments.empty()) {
      blendSurface.colorAttachments[0].loadOp = LoadOp::Load;
      blendSurface.colorAttachments[0].storeOp = StoreOp::Store;
      blendSurface.colorAttachments[0].clearValue.color = zeroClear;
    }

    m_rendererBase.setActiveSurfaceWithLoadStore(blendSurface, Z3DRendererBase::Preserve);

    const glm::vec2 screenDimRcp(targetSize.x > 0u ? 1.f / static_cast<float>(targetSize.x) : 0.f,
                                 targetSize.y > 0u ? 1.f / static_cast<float>(targetSize.y) : 0.f);

    uint32_t occlusionQueryIndex = TextureDualPeelPayload::kInvalidQueryIndex;
    if (occlusionSupported) {
      if (auto queryOpt = vkBackend->allocateOcclusionQuery()) {
        occlusionQueryIndex = *queryOpt;
      }
    }

    recordInVulkanFrame(
      m_rendererBase,
      [&]() {
        TextureDualPeelPayload payload;
        payload.stage = TextureDualPeelPayload::Stage::Blend;
        payload.tempAttachment = backTempPing[currId];
        payload.screenDimRcp = screenDimRcp;
        payload.occlusionQueryIndex = occlusionQueryIndex;

        RenderBatch batch;
        batch.eye = eye;
        const glm::uvec4 viewport = m_rendererBase.frameState().viewport;
        batch.pass.viewport.origin = glm::vec2(static_cast<float>(viewport.x), static_cast<float>(viewport.y));
        batch.pass.viewport.extent = glm::vec2(static_cast<float>(viewport.z), static_cast<float>(viewport.w));
        batch.pass.viewport.minDepth = 0.0f;
        batch.pass.viewport.maxDepth = 1.0f;
        batch.pass.colorAttachments = m_rendererBase.frameState().activeSurface.colorAttachments;
        batch.pass.depthAttachment = std::nullopt;
        batch.draw.topology = PrimitiveTopology::TriangleStrip;
        batch.draw.vertexCount = 4;
        batch.draw.indexCount = 0;
        batch.geometry = payload;
        m_rendererBase.appendBatch(std::move(batch));
      },
      "transparency_ddp_blend");

    resetHooks();

    executedPasses++;

    if (occlusionSupported && occlusionQueryIndex != TextureDualPeelPayload::kInvalidQueryIndex) {
      const uint64_t samples = vkBackend->lastOcclusionQueryResult(occlusionQueryIndex);
      if (samples == 0u) {
        break;
      }
    }
  }

  VLOG(1) << fmt::format("DDP Vulkan executed {} peel passes", executedPasses);
  // TODO: Consider option (2) for Vulkan dual-depth peeling. Record each peel pass in a separate command buffer, flush,
  // read occlusion query results immediately, and stop submitting once a pass reports zero samples. This would mirror
  // GL behaviour but requires reworking command buffer submission per pass. (See discussion about matching GL pass
  // counts.)

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

  m_rendererBase.setActiveSurfaceWithLoadStore(std::move(outSurface), Z3DRendererBase::Preserve);

  const glm::vec2 screenDimRcp(targetSize.x > 0u ? 1.f / static_cast<float>(targetSize.x) : 0.f,
                               targetSize.y > 0u ? 1.f / static_cast<float>(targetSize.y) : 0.f);

  recordInVulkanFrame(
    m_rendererBase,
    [&]() {
      TextureDualPeelPayload payload;
      payload.stage = TextureDualPeelPayload::Stage::Final;
      payload.frontAttachment = frontPing[currId];
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
      batch.draw.topology = PrimitiveTopology::TriangleStrip;
      batch.draw.vertexCount = 4;
      batch.draw.indexCount = 0;
      batch.geometry = payload;
      m_rendererBase.appendBatch(std::move(batch));
    },
    "transparency_ddp_final");

  resetHooks();
}

void Z3DCompositor::renderTransparentWAVulkan(const std::vector<Z3DBoundedFilter*>& filters,
                                              Z3DScratchResourcePool::RenderTargetLease& targetLease,
                                              Z3DEye eye,
                                              AttachmentHandle depthAttachmentHandle,
                                              const std::vector<Z3DCompositorImageLayer>& imageLayers,
                                              bool clearResolveTarget)
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
      attachment.clearValue.color = glm::vec4(0.0f);
    }
    if (depthAttachmentHandle.valid()) {
      AttachmentDesc depthDesc;
      depthDesc.handle = depthAttachmentHandle;
      depthDesc.loadOp = LoadOp::Load;
      depthDesc.storeOp = StoreOp::Store;
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

  m_rendererBase.setActiveSurfaceWithLoadStore(waInitSurface, Z3DRendererBase::Preserve);

  // Geometry init step must see WeightedAverageInit on the compositor renderer
  m_rendererBase.setShaderHookType(Z3DRendererBase::ShaderHookType::WeightedAverageInit);
  recordInVulkanFrame(
    m_rendererBase,
    [&]() {
      // Use the same helper pattern as DDP to avoid nested begin/end
      auto recordFilterBatchesToSurface =
        [&](Z3DBoundedFilter* filter, const RendererFrameState::ActiveSurface& surface, auto&& renderFn) {
          if (!filter) {
            return;
          }
          auto& source = filter->rendererBase();

          const glm::uvec4 previousViewport = source.frameState().viewport;
          const auto previousSurface = source.frameState().activeSurface;

          // Mirror the compositor viewport into the filter's renderer so batches inherit it
          source.frameState().updateViewportData(m_rendererBase.frameState().viewport);
          source.setActiveSurfaceWithLoadStore(surface, Z3DRendererBase::Preserve);
          renderFn();
          auto& batches = source.cpuState().batches;
          for (auto& batch : batches) {
            if (batch.pass.colorAttachments.empty() && !surface.colorAttachments.empty()) {
              batch.pass.colorAttachments = surface.colorAttachments;
            }
            if (!batch.pass.depthAttachment.has_value() && surface.depthAttachment.has_value()) {
              batch.pass.depthAttachment = surface.depthAttachment;
            }
            // If a batch doesn't set a viewport, populate it from the compositor's viewport
            if (batch.pass.viewport.extent == glm::vec2(0.0f) && m_rendererBase.frameState().viewport.z > 0u &&
                m_rendererBase.frameState().viewport.w > 0u) {
              batch.pass.viewport.origin = glm::vec2(static_cast<float>(m_rendererBase.frameState().viewport.x),
                                                     static_cast<float>(m_rendererBase.frameState().viewport.y));
              batch.pass.viewport.extent = glm::vec2(static_cast<float>(m_rendererBase.frameState().viewport.z),
                                                     static_cast<float>(m_rendererBase.frameState().viewport.w));
              batch.pass.viewport.minDepth = 0.0f;
              batch.pass.viewport.maxDepth = 1.0f;
            }
            m_rendererBase.appendBatch(std::move(batch));
          }
          source.resetCPUState();

          source.frameState().updateViewportData(previousViewport);
          source.setActiveSurfaceWithLoadStore(previousSurface, Z3DRendererBase::Preserve);
        };

      // 1) Geometry filters
      for (auto* filter : filters) {
        if (!filter) {
          continue;
        }
        filter->setViewport(targetSize);
        filter->setShaderHookType(Z3DRendererBase::ShaderHookType::WeightedAverageInit);
        recordFilterBatchesToSurface(filter, waInitSurface, [&]() {
          filter->renderTransparent(eye);
        });
      }
      // 2) Image layers: sample from filters' transparent leases using WA image init copy
      if (!imageLayers.empty()) {
        m_rendererBase.setShaderHookType(Z3DRendererBase::ShaderHookType::WeightedAverageInit);
        VLOG(1) << fmt::format("WA init: merging {} image layer(s)", imageLayers.size());
        for (const auto& layer : imageLayers) {
          const auto& colorDesc = layer.colorAttachment;
          const auto& depthDesc = layer.depthAttachment;
          if (colorDesc.handle.backend != AttachmentBackend::Vulkan || !colorDesc.handle.valid()) {
            continue;
          }
          if (depthDesc.handle.backend != AttachmentBackend::Vulkan || !depthDesc.handle.valid()) {
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
    },
    "transparency_wa_init");

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
  // Depth disabled by invariant; no depth load/store

  m_rendererBase.setActiveSurfaceWithLoadStore(std::move(outSurface), Z3DRendererBase::Preserve);

  const glm::vec2 screenDimRcp(targetSize.x > 0u ? 1.f / static_cast<float>(targetSize.x) : 0.f,
                               targetSize.y > 0u ? 1.f / static_cast<float>(targetSize.y) : 0.f);

  recordInVulkanFrame(
    m_rendererBase,
    [&]() {
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
      batch.draw.topology = PrimitiveTopology::TriangleStrip;
      batch.draw.vertexCount = 4;
      batch.draw.indexCount = 0;
      batch.geometry = payload;
      m_rendererBase.appendBatch(std::move(batch));
    },
    "transparency_wa_resolve");

  resetHooks();
}

void Z3DCompositor::renderTransparentWBVulkan(const std::vector<Z3DBoundedFilter*>& filters,
                                              Z3DScratchResourcePool::RenderTargetLease& targetLease,
                                              Z3DEye eye,
                                              AttachmentHandle depthAttachmentHandle,
                                              const std::vector<Z3DCompositorImageLayer>& imageLayers,
                                              bool clearResolveTarget)
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
      surface.colorAttachments[0].clearValue.color = glm::vec4(0.0f);
    }
    if (surface.colorAttachments.size() > 1) {
      surface.colorAttachments[1].loadOp = LoadOp::Clear;
      surface.colorAttachments[1].storeOp = StoreOp::Store;
      surface.colorAttachments[1].clearValue.color = glm::vec4(1.0f);
    }
    if (depthAttachmentHandle.valid()) {
      AttachmentDesc depthDesc;
      depthDesc.handle = depthAttachmentHandle;
      depthDesc.loadOp = LoadOp::Load;
      depthDesc.storeOp = StoreOp::Store;
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

  m_rendererBase.setActiveSurfaceWithLoadStore(wbInitSurface, Z3DRendererBase::Preserve);

  // Geometry init step must see WeightedBlendedInit on the compositor renderer
  m_rendererBase.setShaderHookType(Z3DRendererBase::ShaderHookType::WeightedBlendedInit);
  recordInVulkanFrame(
    m_rendererBase,
    [&]() {
      // Reuse helper from WA block above
      auto recordFilterBatchesToSurface =
        [&](Z3DBoundedFilter* filter, const RendererFrameState::ActiveSurface& surface, auto&& renderFn) {
          if (!filter) {
            return;
          }
          auto& source = filter->rendererBase();
          const glm::uvec4 previousViewport = source.frameState().viewport;
          const auto previousSurface = source.frameState().activeSurface;
          source.frameState().updateViewportData(m_rendererBase.frameState().viewport);
          source.setActiveSurfaceWithLoadStore(surface, Z3DRendererBase::Preserve);
          renderFn();
          auto& batches = source.cpuState().batches;
          for (auto& batch : batches) {
            if (batch.pass.colorAttachments.empty() && !surface.colorAttachments.empty()) {
              batch.pass.colorAttachments = surface.colorAttachments;
            }
            if (!batch.pass.depthAttachment.has_value() && surface.depthAttachment.has_value()) {
              batch.pass.depthAttachment = surface.depthAttachment;
            }
            if (batch.pass.viewport.extent == glm::vec2(0.0f) && m_rendererBase.frameState().viewport.z > 0u &&
                m_rendererBase.frameState().viewport.w > 0u) {
              batch.pass.viewport.origin = glm::vec2(static_cast<float>(m_rendererBase.frameState().viewport.x),
                                                     static_cast<float>(m_rendererBase.frameState().viewport.y));
              batch.pass.viewport.extent = glm::vec2(static_cast<float>(m_rendererBase.frameState().viewport.z),
                                                     static_cast<float>(m_rendererBase.frameState().viewport.w));
              batch.pass.viewport.minDepth = 0.0f;
              batch.pass.viewport.maxDepth = 1.0f;
            }
            m_rendererBase.appendBatch(std::move(batch));
          }
          source.resetCPUState();
          source.frameState().updateViewportData(previousViewport);
          source.setActiveSurfaceWithLoadStore(previousSurface, Z3DRendererBase::Preserve);
        };

      for (auto* filter : filters) {
        if (!filter) {
          continue;
        }
        filter->setViewport(targetSize);
        filter->setShaderHookType(Z3DRendererBase::ShaderHookType::WeightedBlendedInit);
        recordFilterBatchesToSurface(filter, wbInitSurface, [&]() {
          filter->renderTransparent(eye);
        });
      }
      if (!imageLayers.empty()) {
        m_rendererBase.setShaderHookType(Z3DRendererBase::ShaderHookType::WeightedBlendedInit);
        for (const auto& layer : imageLayers) {
          const auto& colorDesc = layer.colorAttachment;
          const auto& depthDesc = layer.depthAttachment;
          if (colorDesc.handle.backend != AttachmentBackend::Vulkan || !colorDesc.handle.valid()) {
            continue;
          }
          if (depthDesc.handle.backend != AttachmentBackend::Vulkan || !depthDesc.handle.valid()) {
            continue;
          }
          // WB image init copy: do not flip
          m_textureCopyRenderer.setFlipY(false);
          m_textureCopyRenderer.setSourceAttachments(colorDesc.handle, depthDesc.handle);
          m_rendererBase.renderVulkan(eye, m_textureCopyRenderer);
        }
      }
    },
    "transparency_wb_init");

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

  m_rendererBase.setActiveSurfaceWithLoadStore(std::move(outSurface), Z3DRendererBase::Preserve);

  const glm::vec2 screenDimRcp(targetSize.x > 0u ? 1.f / static_cast<float>(targetSize.x) : 0.f,
                               targetSize.y > 0u ? 1.f / static_cast<float>(targetSize.y) : 0.f);

  recordInVulkanFrame(
    m_rendererBase,
    [&]() {
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
      batch.draw.topology = PrimitiveTopology::TriangleStrip;
      batch.draw.vertexCount = 4;
      batch.draw.indexCount = 0;
      batch.geometry = payload;
      m_rendererBase.appendBatch(std::move(batch));
    },
    "transparency_wb_resolve");

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
  const auto expected = backend == RenderBackend::Vulkan ? Z3DCoordinateSystem::Vulkan : Z3DCoordinateSystem::OpenGL;
  if (m_axisCamera.getCoordinateSystem() != expected) {
    setupAxisCamera();
  }
}

void Z3DCompositor::renderAxisVulkan(Z3DEye eye, Z3DScratchResourcePool::RenderTargetLease& sceneOutLease)
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

  const glm::uvec4 prevViewport = m_rendererBase.frameState().viewport;
  const auto prevSurface = m_rendererBase.frameState().activeSurface;
  const auto prevHook = m_rendererBase.shaderHookType();
  const bool hookWasNormal = prevHook == Z3DRendererBase::ShaderHookType::Normal;

  auto& params = m_rendererBase.parameterState();
  const glm::mat4 previousTransform = params.coordTransform;
  params.coordTransform = axisTransform;
  auto transformGuard = folly::makeGuard([&params, previousTransform]() {
    params.coordTransform = previousTransform;
  });

  const auto previousViewState = m_rendererBase.pushViewStateFromCamera(m_axisCamera);
  auto viewGuard = folly::makeGuard([this, previousViewState]() {
    m_rendererBase.restoreViewState(previousViewState);
  });

  m_rendererBase.frameState().updateViewportData(axisViewport);
  if (!hookWasNormal) {
    m_rendererBase.setShaderHookType(Z3DRendererBase::ShaderHookType::Normal);
  }

  m_rendererBase.setActiveSurfaceWithLoadStore(std::move(axisSurface),
                                               LoadOp::Load,
                                               StoreOp::Store,
                                               LoadOp::Clear,
                                               StoreOp::Store);

  recordInVulkanFrame(
    m_rendererBase,
    [&]() {
      if (m_axisMode.get() == "Arrow") {
        m_rendererBase.renderVulkan(eye, m_arrowRenderer, m_fontRenderer);
      } else {
        m_rendererBase.renderVulkan(eye, m_lineRenderer, m_fontRenderer);
      }
    },
    "axis_overlay");

  m_rendererBase.frameState().updateViewportData(prevViewport);
  m_rendererBase.setActiveSurfaceWithLoadStore(prevSurface, Z3DRendererBase::Preserve);
  if (!hookWasNormal) {
    m_rendererBase.setShaderHookType(prevHook);
  }
}

void Z3DCompositor::renderAxis(Z3DEye eye)
{
  prepareAxisData(eye);
  {
    const glm::mat4 axisTransform = glm::mat4(m_globalParameters.camera.get().rotateMatrix(eye));
    const glm::uvec4& vp = viewport();

    if (m_region[0] <= 0.f && m_region[2] <= 0.f) {
      double startX = vp.x + vp.z / m_region[1] * m_region[0];
      double startY = vp.y + vp.w / m_region[3] * m_region[2];

      GLsizei size = std::min(vp.z, vp.w) * m_axisRegionRatio.get();
      glViewport(vp.x - std::floor(startX), vp.y - std::floor(startY), size, size);
      glScissor(vp.x - std::floor(startX), vp.y - std::floor(startY), size, size);
      glEnable(GL_SCISSOR_TEST);
      glClear(GL_DEPTH_BUFFER_BIT);

      if (m_axisMode.get() == "Arrow") {
        renderWithStateAndCameraAndCoordTransform(eye, m_axisCamera, axisTransform, m_arrowRenderer, m_fontRenderer);
      } else {
        renderWithStateAndCameraAndCoordTransform(eye, m_axisCamera, axisTransform, m_lineRenderer, m_fontRenderer);
      }

      glViewport(vp.x, vp.y, vp.z, vp.w);
      glScissor(vp.x, vp.y, vp.z, vp.w);
      glDisable(GL_SCISSOR_TEST);
    }
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
  Z3DCamera camera(m_rendererBase.activeBackend() == RenderBackend::Vulkan ? Z3DCoordinateSystem::Vulkan
                                                                           : Z3DCoordinateSystem::OpenGL);
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
  m_arrowRenderer.setArrowData(&m_tailPosAndTailRadius, &m_headPosAndHeadRadius, .1f);
  m_arrowRenderer.setArrowColors(&m_textColors);
  m_fontRenderer.setDataColors(&m_textColors);
}

// Collect non-opaque image layers (color/depth) from connected image filters
std::vector<Z3DCompositorImageLayer> Z3DCompositor::collectNonOpaqueImageLayers(Z3DEye eye)
{
  std::vector<Z3DCompositorImageLayer> layers;
  const bool useVulkan = m_rendererBase.activeBackend() == RenderBackend::Vulkan;

  auto vFilters = m_vPPort.connectedFilters();
  for (auto* vf : vFilters) {
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
