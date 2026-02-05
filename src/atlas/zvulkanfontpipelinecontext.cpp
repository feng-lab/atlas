#include "zvulkanfontpipelinecontext.h"

#include "z3drendererbase.h"
#include "z3drenderervulkanbackend.h"
#include "z3dfontrenderer.h"
#include "z3dsdfont.h"
#include "zsysteminfo.h"
#include "zvulkandevice.h"
#include "zvulkancontext.h"
#include "zvulkanpipeline.h"
#include "zvulkanshader.h"
#include "zvulkantexture.h"
#include "zvulkandescriptorset.h"
#include "zvulkanbuffer.h"
#include "zvulkanrenderconversions.h"
#include "zvulkanpipelinecontext_raii.h"
#include "zlog.h"

#include <array>

namespace nim {

ZVulkanFontPipelineContext::ZVulkanFontPipelineContext(Z3DRendererVulkanBackend& backend)
  : m_backend(backend)
{}

ZVulkanFontPipelineContext::~ZVulkanFontPipelineContext() = default;

void ZVulkanFontPipelineContext::resetFrame()
{
  m_vertexCount = 0;
  m_indexCount = 0;
  m_vertexUploadBuffer = nullptr;
  m_vertexUploadOffset = 0;
  m_indexUploadBuffer = nullptr;
  m_indexUploadOffset = 0;
  resetDescriptors();
  m_staticCopyPendingKeys.clear();
}

void ZVulkanFontPipelineContext::evictStream(uint64_t streamKey)
{
  if (streamKey == 0) {
    return;
  }

  for (auto it = m_staticCopyPendingKeys.begin(); it != m_staticCopyPendingKeys.end();) {
    if (it->streamKey == streamKey) {
      it = m_staticCopyPendingKeys.erase(it);
    } else {
      ++it;
    }
  }

  for (auto it = m_staticCache.begin(); it != m_staticCache.end();) {
    if (it->first.streamKey != streamKey) {
      ++it;
      continue;
    }
    auto& entry = it->second;
    m_backend.releaseStaticSlice(entry.vb);
    m_backend.releaseStaticSlice(entry.ib);
    it = m_staticCache.erase(it);
  }
}

void ZVulkanFontPipelineContext::resetDescriptors()
{
  m_descriptorSet.reset();
}

void ZVulkanFontPipelineContext::record(Z3DRendererBase& renderer,
                                        const RenderBatch& batch,
                                        const FontPayload& payload,
                                        const vk::Viewport& viewport,
                                        const vk::Rect2D& scissor,
                                        vk::raii::CommandBuffer& cmd)
{
  // Allow truly empty payloads to no-op; treat everything else as invariants.
  if (payload.positions.empty() || payload.texcoords.empty() || payload.indices.empty()) {
    return;
  }

  // GL parity: for picking, missing pickingColors means skip rendering; same for color pass.
  if (payload.pickingPass) {
    if (payload.pickingColors.empty() || payload.pickingColors.size() != payload.positions.size()) {
      return;
    }
  } else {
    if (payload.colors.empty() || payload.colors.size() != payload.positions.size()) {
      return;
    }
  }

  // payload is self-contained; no renderer pointer expected
  CHECK(payload.positions.size() == payload.texcoords.size())
    << "Font payload size mismatch: positions=" << payload.positions.size()
    << " texcoords=" << payload.texcoords.size();

  // Upload/prepare geometry
  uploadGeometry(payload);
  CHECK(m_vertexUploadBuffer && m_indexUploadBuffer) << "Font buffers not created after uploadGeometry";
  CHECK(m_vertexCount > 0 && m_indexCount > 0) << "Uploaded empty font geometry";

  // Ensure texture descriptor via per-draw override set
  ensureDescriptorLayout();
  CHECK(m_setTexture.has_value()) << "Failed to create font descriptor set layout";
  ZVulkanTexture* atlas = ensureAtlasFromPayload(payload);
  CHECK(atlas != nullptr) << "Font atlas texture unavailable";
  ZVulkanDescriptorSet* ds = m_backend.allocateOverrideDescriptorSet(**m_setTexture);
  CHECK(ds != nullptr) << "Failed to allocate override descriptor set for font atlas";
  ds->updateTexture(0, *atlas, m_backend.defaultSampler());

  // Debug: verify viewport/scissor and atlas metadata for MoltenVK issues
  VLOG(1) << fmt::format(
    "VK font state: viewport=({:.1f},{:.1f} {:.1f}x{:.1f}) scissor=({},{} {}x{}) atlas={}x{} fmt={} picking={}",
    viewport.x,
    viewport.y,
    viewport.width,
    viewport.height,
    scissor.offset.x,
    scissor.offset.y,
    scissor.extent.width,
    scissor.extent.height,
    atlas->width(),
    atlas->height(),
    enumOrUnderlying(atlas->format(), 16),
    payload.pickingPass);

  const auto formats = vulkan::extractAttachmentFormats(batch);

  PipelineKey key;
  key.picking = payload.pickingPass;
  key.showOutline = payload.showOutline;
  key.showShadow = payload.showShadow;
  key.outlineMode = payload.outlineMode;
  key.colorFormats = formats.colorFormats;
  key.depthFormat = formats.depthFormat;

  PipelineInstance& pipeline = ensurePipeline(key, formats);

  // Draw-only; backend controls attachments and render area

  // Compose push constants (match shader layout)
  const auto& eyeState = renderer.viewState().eyes[static_cast<size_t>(batch.eye)];
  CHECK(payload.paramsCaptured) << "Font payload missing params";
  // Match OpenGL behaviour: renderers that opt out of coord transforms (e.g., the
  // axis overlay font) expect billboard quads in camera space.
  const glm::mat4 modelTransform = payload.followCoordTransform ? payload.params.coordTransform : glm::mat4(1.f);
  glm::mat4 projView = eyeState.projectionMatrix * eyeState.viewMatrix * modelTransform;

  FontPushConstants constants;
  constants.projectionView = projView;
  constants.alpha = 1.0f; // fully opaque text; fragment premultiplies
  constants.softedgeScale = payload.softedgeScale;
  constants.outlineColor = payload.outlineColor;
  constants.shadowColor = payload.shadowColor;

  ZVulkanPipelineCommandRecorder::GraphicsDrawSpec drawSpec{};
  drawSpec.viewports = {viewport};
  drawSpec.scissors = {scissor};
  drawSpec.pipelineHandle = pipeline.pipeline->pipelineHandle();
  drawSpec.pipelineLayoutHandle = pipeline.pipeline->pipelineLayoutHandle();
  drawSpec.descriptorSetFirst = 0;
  drawSpec.descriptorSets = {ds->descriptorSet()};
  drawSpec.expectedDescriptorSetCount = 1;
  drawSpec.vertexBuffers = {m_vertexUploadBuffer};
  drawSpec.vertexOffsets = {m_vertexUploadOffset};
  drawSpec.indexBuffer = m_indexUploadBuffer;
  drawSpec.indexOffset = m_indexUploadOffset;
  drawSpec.indexType = vk::IndexType::eUint32;
  drawSpec.indexCount = static_cast<uint32_t>(m_indexCount);
  drawSpec.instanceCount = 1;
  drawSpec.pushConstantsData = &constants;
  drawSpec.pushConstantsSize = static_cast<uint32_t>(sizeof(FontPushConstants));
  drawSpec.pushConstantsStages = vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment;
  drawSpec.requirePushConstants = true;

  VLOG(1) << fmt::format("VK font draw(recorder): verts={} idx={} picking={} dpr={:.3f} usesCoordXf=1",
                         m_vertexCount,
                         m_indexCount,
                         payload.pickingPass,
                         renderer.sceneState().devicePixelRatio);

  ZVulkanPipelineCommandRecorder recorder(cmd);
  recorder.recordGraphicsDraw(drawSpec);
}

void ZVulkanFontPipelineContext::ensureDescriptorLayout()
{
  if (m_setTexture) {
    return;
  }
  auto& device = m_backend.device();
  auto& vkDevice = device.context().device();

  std::array<vk::DescriptorSetLayoutBinding, 1> bindings{
    vk::DescriptorSetLayoutBinding{.binding = 0,
                                   .descriptorType = vk::DescriptorType::eCombinedImageSampler,
                                   .descriptorCount = 1,
                                   .stageFlags = vk::ShaderStageFlagBits::eFragment}
  };

  vk::DescriptorSetLayoutCreateInfo createInfo{.bindingCount = static_cast<uint32_t>(bindings.size()),
                                               .pBindings = bindings.data()};
  m_setTexture.emplace(vkDevice, createInfo);
}

void ZVulkanFontPipelineContext::ensureDescriptorSet() {}

vk::PipelineVertexInputStateCreateInfo ZVulkanFontPipelineContext::makeVertexInputState() const
{
  static vk::VertexInputBindingDescription binding{.binding = 0,
                                                   .stride = static_cast<uint32_t>(sizeof(FontVertex)),
                                                   .inputRate = vk::VertexInputRate::eVertex};
  static std::array<vk::VertexInputAttributeDescription, 3> attrs{
    vk::VertexInputAttributeDescription{.location = 0,
                                        .binding = 0,
                                        .format = vk::Format::eR32G32B32Sfloat,
                                        .offset = static_cast<uint32_t>(offsetof(FontVertex, position))},
    vk::VertexInputAttributeDescription{.location = 1,
                                        .binding = 0,
                                        .format = vk::Format::eR32G32Sfloat,
                                        .offset = static_cast<uint32_t>(offsetof(FontVertex, texcoord))},
    vk::VertexInputAttributeDescription{.location = 2,
                                        .binding = 0,
                                        .format = vk::Format::eR32G32B32A32Sfloat,
                                        .offset = static_cast<uint32_t>(offsetof(FontVertex, color))   }
  };

  static vk::PipelineVertexInputStateCreateInfo info{};
  info.vertexBindingDescriptionCount = 1;
  info.pVertexBindingDescriptions = &binding;
  info.vertexAttributeDescriptionCount = static_cast<uint32_t>(attrs.size());
  info.pVertexAttributeDescriptions = attrs.data();
  return info;
}

void ZVulkanFontPipelineContext::uploadGeometry(const FontPayload& payload)
{
  m_vertexCount = 0;
  m_indexCount = 0;
  const size_t vtxCount = payload.positions.size();
  const size_t idxCount = payload.indices.size();
  if (vtxCount == 0 || idxCount == 0) {
    return;
  }

  CHECK(payload.texcoords.size() == vtxCount)
    << "Font uploadGeometry size mismatch: positions=" << vtxCount << " texcoords=" << payload.texcoords.size();
  if (payload.pickingPass) {
    CHECK(payload.pickingColors.size() == vtxCount)
      << "Font uploadGeometry pickingColors mismatch: pickingColors=" << payload.pickingColors.size()
      << " positions=" << vtxCount;
  } else {
    CHECK(payload.colors.size() == vtxCount)
      << "Font uploadGeometry colors mismatch: colors=" << payload.colors.size() << " positions=" << vtxCount;
  }

  // Fast path: if this stream was promoted to device-local static buffers and
  // nothing changed, bind the static buffers and skip per-frame staging/memcpy.
  {
    CHECK(payload.streamKey != 0) << "Font payload missing streamKey";
    CacheKey key{payload.streamKey, payload.pickingPass};
    if (!m_staticCopyPendingKeys.contains(key)) {
      auto it = m_staticCache.find(key);
      if (it != m_staticCache.end()) {
        const CacheEntry& entry = it->second;
        const bool sizeSame = (entry.vertexCount == vtxCount) && (entry.indexCount == idxCount);
        const uint32_t colorGen = payload.pickingPass ? payload.pickingColorsGen : payload.colorsGen;
        const bool gensSame = (entry.posGen == payload.positionsGen) && (entry.texGen == payload.texcoordsGen) &&
                              (entry.colorGen == colorGen) && (entry.indexGen == payload.indicesGen);
        if (entry.promoted && sizeSame && gensSame && entry.vb && entry.ib) {
          m_vertexCount = vtxCount;
          m_indexCount = idxCount;
          m_vertexUploadBuffer = entry.vb.buffer;
          m_vertexUploadOffset = entry.vb.offset;
          m_indexUploadBuffer = entry.ib.buffer;
          m_indexUploadOffset = entry.ib.offset;
          m_backend.pinStaticSliceForActiveSubmission(entry.vb);
          m_backend.pinStaticSliceForActiveSubmission(entry.ib);
          return;
        }
      }
    }
  }

  auto vSlice = m_backend.suballocateUpload(vtxCount * sizeof(FontVertex), alignof(FontVertex));
  auto iSlice = m_backend.suballocateUpload(idxCount * sizeof(uint32_t), alignof(uint32_t));
  CHECK(vSlice.buffer && vSlice.mapped && iSlice.buffer && iSlice.mapped) << "Font arena slices allocation failed";

  auto* vertices = static_cast<FontVertex*>(vSlice.mapped);
  for (size_t i = 0; i < vtxCount; ++i) {
    vertices[i].position = payload.positions[i];
    vertices[i].texcoord = payload.texcoords[i];
    vertices[i].color = payload.pickingPass ? payload.pickingColors[i] : payload.colors[i];
  }

  // Debug-only guard on index range to catch bad CPU geometry.
  for (const auto idx : payload.indices) {
    DCHECK(idx < vtxCount) << "Font index out of range: " << idx << " >= " << vtxCount;
  }

  std::memcpy(iSlice.mapped, payload.indices.data(), payload.indices.size() * sizeof(uint32_t));
  m_vertexCount = vtxCount;
  m_indexCount = idxCount;
  m_vertexUploadBuffer = vSlice.buffer;
  m_vertexUploadOffset = vSlice.offset;
  m_indexUploadBuffer = iSlice.buffer;
  m_indexUploadOffset = iSlice.offset;

  // Static promotion (AoS)
  {
    CHECK(payload.streamKey != 0) << "Font payload missing streamKey";
    CacheKey key{payload.streamKey, payload.pickingPass};
    auto it = m_staticCache.find(key);
    const int kPromotionThreshold = 2;
    if (it == m_staticCache.end()) {
      CacheEntry entry{};
      entry.vertexCount = static_cast<uint32_t>(vtxCount);
      entry.indexCount = static_cast<uint32_t>(idxCount);
      entry.posGen = payload.positionsGen;
      entry.texGen = payload.texcoordsGen;
      entry.colorGen = payload.pickingPass ? payload.pickingColorsGen : payload.colorsGen;
      entry.indexGen = payload.indicesGen;
      m_staticCache.emplace(key, entry);
    } else {
      CacheEntry& entry = it->second;
      const bool sizeSame = (entry.vertexCount == vtxCount) && (entry.indexCount == idxCount);
      const bool gensSame = entry.posGen == payload.positionsGen && entry.texGen == payload.texcoordsGen &&
                            entry.indexGen == payload.indicesGen &&
                            entry.colorGen == (payload.pickingPass ? payload.pickingColorsGen : payload.colorsGen);
      if (sizeSame && gensSame) {
        entry.unchangedFrames++;
      } else {
        entry.unchangedFrames = 0;
      }
      // A previous draw in this submission scheduled upload->static copies for
      // this stream; do not bind statics until the next submission because the
      // copies are flushed after rendering ends.
      if (m_staticCopyPendingKeys.contains(key)) {
        entry.vertexCount = static_cast<uint32_t>(vtxCount);
        entry.indexCount = static_cast<uint32_t>(idxCount);
        entry.posGen = payload.positionsGen;
        entry.texGen = payload.texcoordsGen;
        entry.colorGen = payload.pickingPass ? payload.pickingColorsGen : payload.colorsGen;
        entry.indexGen = payload.indicesGen;
        return;
      }
      if (entry.promoted && !sizeSame) {
        m_backend.releaseStaticSlice(entry.vb);
        m_backend.releaseStaticSlice(entry.ib);
        entry.promoted = false;
        entry.unchangedFrames = 0;
        entry.vertexCount = static_cast<uint32_t>(vtxCount);
        entry.indexCount = static_cast<uint32_t>(idxCount);
        entry.posGen = payload.positionsGen;
        entry.texGen = payload.texcoordsGen;
        entry.colorGen = payload.pickingPass ? payload.pickingColorsGen : payload.colorsGen;
        entry.indexGen = payload.indicesGen;
        return;
      }
      if (entry.promoted && sizeSame) {
        // Restage VB/IB if any gen changed
        size_t restaged = 0;
        bool anyChanged = false;
        if (entry.posGen != payload.positionsGen || entry.texGen != payload.texcoordsGen ||
            entry.colorGen != (payload.pickingPass ? payload.pickingColorsGen : payload.colorsGen)) {
          m_backend.scheduleStaticCopy(entry.vb.buffer, entry.vb.offset, vSlice, /*isIndexBuffer=*/false);
          restaged += vtxCount * sizeof(FontVertex);
          anyChanged = true;
        }
        if (entry.indexGen != payload.indicesGen) {
          m_backend.scheduleStaticCopy(entry.ib.buffer, entry.ib.offset, iSlice, /*isIndexBuffer=*/true);
          restaged += idxCount * sizeof(uint32_t);
          anyChanged = true;
        }
        entry.vertexCount = static_cast<uint32_t>(vtxCount);
        entry.indexCount = static_cast<uint32_t>(idxCount);
        entry.posGen = payload.positionsGen;
        entry.texGen = payload.texcoordsGen;
        entry.colorGen = payload.pickingPass ? payload.pickingColorsGen : payload.colorsGen;
        entry.indexGen = payload.indicesGen;
        if (restaged > 0) {
          m_backend.addFontBytesStaged(restaged);
        }
        if (!anyChanged) {
          // Bind statics on steady frames
          m_vertexUploadBuffer = entry.vb.buffer;
          m_vertexUploadOffset = entry.vb.offset;
          m_indexUploadBuffer = entry.ib.buffer;
          m_indexUploadOffset = entry.ib.offset;
          m_backend.pinStaticSliceForActiveSubmission(entry.vb);
          m_backend.pinStaticSliceForActiveSubmission(entry.ib);
        } else {
          m_staticCopyPendingKeys.insert(key);
        }
        return;
      }
      entry.vertexCount = static_cast<uint32_t>(vtxCount);
      entry.indexCount = static_cast<uint32_t>(idxCount);
      entry.posGen = payload.positionsGen;
      entry.texGen = payload.texcoordsGen;
      entry.colorGen = payload.pickingPass ? payload.pickingColorsGen : payload.colorsGen;
      entry.indexGen = payload.indicesGen;
      if (!entry.promoted && sizeSame && entry.unchangedFrames >= kPromotionThreshold) {
        auto vbDst = m_backend.allocateStaticVB(vtxCount * sizeof(FontVertex), alignof(FontVertex));
        auto ibDst = m_backend.allocateStaticIB(idxCount * sizeof(uint32_t), alignof(uint32_t));
        if (vbDst && ibDst) {
          m_backend.scheduleStaticCopy(vbDst.buffer, vbDst.offset, vSlice, /*isIndexBuffer=*/false);
          m_backend.scheduleStaticCopy(ibDst.buffer, ibDst.offset, iSlice, /*isIndexBuffer=*/true);
          entry.vb = vbDst;
          entry.ib = ibDst;
          entry.posGen = payload.positionsGen;
          entry.texGen = payload.texcoordsGen;
          entry.colorGen = payload.pickingPass ? payload.pickingColorsGen : payload.colorsGen;
          entry.indexGen = payload.indicesGen;
          entry.promoted = true;
          m_backend.addFontBytesStaged(vtxCount * sizeof(FontVertex) + idxCount * sizeof(uint32_t));
          // Do not bind statics this frame; keep upload slices. Statics bind next frame.
          m_staticCopyPendingKeys.insert(key);
          return;
        }
      }
    }
  }
}

ZVulkanTexture* ZVulkanFontPipelineContext::ensureAtlasFromPayload(const FontPayload& payload)
{
  // Priority: native Vulkan handle → CPU pixels → fallback
  if (payload.atlasHandle.valid() && payload.atlasHandle.backend == RenderBackend::Vulkan) {
    auto& texture = vulkan::textureFromHandle(payload.atlasHandle, m_backend.device(), "font atlas sampled image");
    return &texture;
  }

  if (payload.atlasPixels && payload.atlasWidth > 0 && payload.atlasHeight > 0) {
    auto it = m_atlasCache.find(payload.atlasPixels);
    if (it != m_atlasCache.end()) {
      auto* tex = it->second.get();
      const auto& ext = tex->extent();
      if (ext.width == payload.atlasWidth && ext.height == payload.atlasHeight) {
        return tex;
      }
      // size changed, recreate
      m_atlasCache.erase(it);
    }

    auto& device = m_backend.device();
    auto info =
      ZVulkanTexture::CreateInfo::make2D(payload.atlasWidth,
                                         payload.atlasHeight,
                                         vk::Format::eB8G8R8A8Unorm,
                                         vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst,
                                         vk::MemoryPropertyFlagBits::eDeviceLocal,
                                         1u,
                                         true,
                                         vk::ImageLayout::eShaderReadOnlyOptimal);
    auto tex = device.createTexture(info);
    CHECK(tex != nullptr) << "Failed to create font atlas texture from CPU pixels (" << payload.atlasWidth << "x"
                          << payload.atlasHeight << ")";
    const size_t byteSize = static_cast<size_t>(payload.atlasWidth) * payload.atlasHeight * 4u;
    tex->uploadData(payload.atlasPixels, byteSize, vk::ImageLayout::eShaderReadOnlyOptimal);
    VLOG(1) << fmt::format("VK font atlas upload: {}x{} {}B", payload.atlasWidth, payload.atlasHeight, byteSize);
    auto [inserted, _] = m_atlasCache.emplace(payload.atlasPixels, std::move(tex));
    return inserted->second.get();
  }

  // Fallback: tiny white
  auto it = m_atlasCache.find(nullptr);
  if (it != m_atlasCache.end()) {
    return it->second.get();
  }
  auto& device = m_backend.device();
  auto info =
    ZVulkanTexture::CreateInfo::make2D(1,
                                       1,
                                       vk::Format::eR8G8B8A8Unorm,
                                       vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst,
                                       vk::MemoryPropertyFlagBits::eDeviceLocal,
                                       1u,
                                       true,
                                       vk::ImageLayout::eShaderReadOnlyOptimal);
  auto tex = device.createTexture(info);
  CHECK(tex != nullptr) << "Failed to create fallback 1x1 white font atlas texture";
  const uint32_t white = 0xffffffffu;
  tex->uploadData(&white, sizeof(white), vk::ImageLayout::eShaderReadOnlyOptimal);
  auto [inserted, _] = m_atlasCache.emplace(nullptr, std::move(tex));
  return inserted->second.get();
}

ZVulkanFontPipelineContext::PipelineInstance&
ZVulkanFontPipelineContext::ensurePipeline(const PipelineKey& key, const vulkan::AttachmentFormats& formats)
{
  auto it = m_pipelineCache.find(key);
  if (it != m_pipelineCache.end()) {
    return it->second;
  }

  ensureDescriptorLayout();

  auto& device = m_backend.device();
  static const std::string shaderBase = ZSystemInfo::resourcesDirPath().toStdString() + "/shader/vulkan/spv/";

  PipelineInstance instance;
  instance.shader =
    std::make_unique<ZVulkanShader>(device, shaderBase + "almag.vert.spv", shaderBase + "almag.frag.spv", std::nullopt);

  auto vertexInput = makeVertexInputState();
  instance.pipeline = device.createPipeline(*instance.shader, vertexInput, vk::PrimitiveTopology::eTriangleList);
  instance.pipeline->setDescriptorSetLayouts({**m_setTexture});
  instance.pipeline->setAttachmentFormats(formats.colorFormats, formats.depthFormat);
  instance.pipeline->setCullMode(vk::CullModeFlagBits::eNone);
  instance.pipeline->setFrontFace(vk::FrontFace::eCounterClockwise);
  if (key.picking) {
    instance.pipeline->setDepthTestEnable(true);
    instance.pipeline->setDepthWriteEnable(false);
    // No blending needed in picking, rely on color writes only
  } else {
    // Overlay text: disable depth test/write to avoid accidental occlusion
    instance.pipeline->setDepthTestEnable(false);
    instance.pipeline->setDepthWriteEnable(false);
    // Premultiplied alpha blending for font shader (rgb is premultiplied by a)
    vk::PipelineColorBlendAttachmentState blend{};
    blend.blendEnable = true;
    blend.srcColorBlendFactor = vk::BlendFactor::eOne;
    blend.dstColorBlendFactor = vk::BlendFactor::eOneMinusSrcAlpha;
    blend.colorBlendOp = vk::BlendOp::eAdd;
    blend.srcAlphaBlendFactor = vk::BlendFactor::eOne;
    blend.dstAlphaBlendFactor = vk::BlendFactor::eOneMinusSrcAlpha;
    blend.alphaBlendOp = vk::BlendOp::eAdd;
    blend.colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                           vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA;
    instance.pipeline->setColorBlendAttachment(blend);
  }

  // Blending configured above per picking/overlay case

  // Specialization constants are not strictly required here; flags are passed via push constants.
  vk::PushConstantRange range{.stageFlags = vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment,
                              .offset = 0,
                              .size = static_cast<uint32_t>(sizeof(FontPushConstants))};
  instance.pipeline->setPushConstantRanges({range});
  instance.pipeline->create();

  VLOG(1) << fmt::format(
    "VK font pipeline created: picking={} colorAttachments={} depthFormat={} blend=premul(One,OneMinusSrcAlpha) depthTest={} depthWrite={}",
    key.picking,
    formats.colorFormats.size(),
    key.depthFormat ? static_cast<int>(*key.depthFormat) : static_cast<int>(vk::Format::eUndefined),
    key.picking ? 1 : 0,
    0);

  auto [inserted, _2] = m_pipelineCache.insert({key, std::move(instance)});
  return inserted->second;
}

} // namespace nim
