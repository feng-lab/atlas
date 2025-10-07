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
#include "zvulkandescriptorpool.h"
#include "zvulkandescriptorset.h"
#include "zvulkanbuffer.h"
#include "zvulkanrenderconversions.h"
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
  m_vertexUploadBuffer = VK_NULL_HANDLE;
  m_vertexUploadOffset = 0;
  m_indexUploadBuffer = VK_NULL_HANDLE;
  m_indexUploadOffset = 0;
  resetDescriptors();
}

void ZVulkanFontPipelineContext::resetDescriptors()
{
  m_descriptorSet.reset();
  if (m_descriptorPool) {
    m_descriptorPool->reset();
  }
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

  CHECK(payload.renderer != nullptr) << "Font record called with null renderer while payload not empty";
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

  const auto formats = vulkan::extractAttachmentFormats(batch);

  PipelineKey key;
  key.picking = payload.pickingPass;
  key.showOutline = payload.showOutline;
  key.showShadow = payload.showShadow;
  key.outlineMode = payload.outlineMode;
  key.colorFormats = formats.colorFormats;
  key.depthFormat = formats.depthFormat;

  PipelineInstance& pipeline = ensurePipeline(key, formats);

  // Bind pipeline and resources
  cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline.pipeline->pipeline());

  cmd.bindVertexBuffers(0, {m_vertexUploadBuffer}, {m_vertexUploadOffset});
  cmd.bindIndexBuffer(m_indexUploadBuffer, m_indexUploadOffset, vk::IndexType::eUint32);

  std::array<vk::DescriptorSet, 1> sets{ds->descriptorSet()};
  cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, pipeline.pipeline->pipelineLayout(), 0, sets, {});

  cmd.setViewport(0, viewport);
  cmd.setScissor(0, scissor);

  // Compose push constants
  const auto& eyeState = renderer.viewState().eyes[static_cast<size_t>(batch.eye)];
  const auto& params = renderer.parameterState();
  glm::mat4 mvp = eyeState.projectionMatrix * eyeState.viewMatrix * params.coordTransform;

  FontPushConstants constants;
  constants.mvp = mvp;
  constants.outlineColor = payload.outlineColor;
  constants.shadowColor = payload.shadowColor;
  constants.softedgeScale = payload.softedgeScale;
  uint32_t flags = 0u;
  flags |= (payload.pickingPass ? 1u : 0u);
  flags |= (payload.showOutline ? (1u << 1) : 0u);
  flags |= (payload.showShadow ? (1u << 2) : 0u);
  flags |= (static_cast<uint32_t>(payload.outlineMode & 0xFF) << 8);
  constants.flags = flags;

  cmd.pushConstants<FontPushConstants>(pipeline.pipeline->pipelineLayout(),
                                       vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment,
                                       0,
                                       constants);

  cmd.drawIndexed(static_cast<uint32_t>(m_indexCount), 1, 0, 0, 0);
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

void ZVulkanFontPipelineContext::ensureDescriptorSet()
{
  ensureDescriptorLayout();

  auto& device = m_backend.device();
  if (!m_descriptorPool) {
    m_descriptorPool = device.createDescriptorPool();
  }

  if (!m_descriptorSet) {
    auto descriptorSet = m_descriptorPool->allocateDescriptorSet(**m_setTexture);
    m_descriptorSet = std::make_unique<ZVulkanDescriptorSet>(device, descriptorSet, false);
  }
}

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
  if (payload.renderer) {
    CacheKey key{payload.renderer, payload.pickingPass};
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
      if (entry.promoted && sizeSame) {
        // Restage VB/IB if any gen changed
        size_t restaged = 0;
        if (entry.posGen != payload.positionsGen || entry.texGen != payload.texcoordsGen ||
            entry.colorGen != (payload.pickingPass ? payload.pickingColorsGen : payload.colorsGen)) {
          m_backend.stageCopy(entry.vb, entry.vbOffset, vSlice, /*isIndexBuffer=*/false);
          entry.posGen = payload.positionsGen;
          entry.texGen = payload.texcoordsGen;
          entry.colorGen = payload.pickingPass ? payload.pickingColorsGen : payload.colorsGen;
          restaged += vtxCount * sizeof(FontVertex);
        }
        if (entry.indexGen != payload.indicesGen) {
          m_backend.stageCopy(entry.ib, entry.ibOffset, iSlice, /*isIndexBuffer=*/true);
          entry.indexGen = payload.indicesGen;
          restaged += idxCount * sizeof(uint32_t);
        }
        if (restaged > 0) {
          m_backend.addFontBytesStaged(restaged);
        }
        m_vertexUploadBuffer = entry.vb;
        m_vertexUploadOffset = entry.vbOffset;
        m_indexUploadBuffer = entry.ib;
        m_indexUploadOffset = entry.ibOffset;
        return;
      }
      if (!entry.promoted && sizeSame && entry.unchangedFrames >= kPromotionThreshold) {
        auto vbDst = m_backend.allocateStaticVB(vtxCount * sizeof(FontVertex), alignof(FontVertex));
        auto ibDst = m_backend.allocateStaticIB(idxCount * sizeof(uint32_t), alignof(uint32_t));
        if (vbDst.buffer && ibDst.buffer) {
          m_backend.stageCopy(vbDst.buffer, vbDst.offset, vSlice, /*isIndexBuffer=*/false);
          m_backend.stageCopy(ibDst.buffer, ibDst.offset, iSlice, /*isIndexBuffer=*/true);
          entry.vb = vbDst.buffer;
          entry.vbOffset = vbDst.offset;
          entry.ib = ibDst.buffer;
          entry.ibOffset = ibDst.offset;
          entry.posGen = payload.positionsGen;
          entry.texGen = payload.texcoordsGen;
          entry.colorGen = payload.pickingPass ? payload.pickingColorsGen : payload.colorsGen;
          entry.indexGen = payload.indicesGen;
          entry.promoted = true;
          m_backend.addFontBytesStaged(vtxCount * sizeof(FontVertex) + idxCount * sizeof(uint32_t));
          m_vertexUploadBuffer = entry.vb;
          m_vertexUploadOffset = entry.vbOffset;
          m_indexUploadBuffer = entry.ib;
          m_indexUploadOffset = entry.ibOffset;
          return;
        }
      }
    }
  }
}

ZVulkanTexture* ZVulkanFontPipelineContext::ensureAtlasFromPayload(const FontPayload& payload)
{
  // Priority: native Vulkan handle → CPU pixels → fallback
  if (payload.atlasHandle.valid() && payload.atlasHandle.backend == AttachmentBackend::Vulkan) {
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
    tex->uploadData(payload.atlasPixels, byteSize);
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
  tex->uploadData(&white, sizeof(white));
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
  instance.pipeline->setDepthTestEnable(true);
  instance.pipeline->setDepthWriteEnable(false);

  // Blending
  vk::PipelineColorBlendAttachmentState blendAttachment{};
  if (key.picking) {
    blendAttachment.blendEnable = VK_FALSE;
  } else {
    blendAttachment.blendEnable = VK_TRUE;
    blendAttachment.srcColorBlendFactor = vk::BlendFactor::eOne;
    blendAttachment.dstColorBlendFactor = vk::BlendFactor::eOneMinusSrcAlpha;
    blendAttachment.colorBlendOp = vk::BlendOp::eAdd;
    blendAttachment.srcAlphaBlendFactor = vk::BlendFactor::eOne;
    blendAttachment.dstAlphaBlendFactor = vk::BlendFactor::eOneMinusSrcAlpha;
    blendAttachment.alphaBlendOp = vk::BlendOp::eAdd;
  }
  blendAttachment.colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                                   vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA;
  instance.pipeline->setColorBlendAttachment(blendAttachment);

  // Specialization constants are not strictly required here; flags are passed via push constants.
  vk::PushConstantRange range{.stageFlags = vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment,
                              .offset = 0,
                              .size = static_cast<uint32_t>(sizeof(FontPushConstants))};
  instance.pipeline->setPushConstantRanges({range});
  instance.pipeline->create();

  auto [inserted, _2] = m_pipelineCache.insert({key, std::move(instance)});
  return inserted->second;
}

} // namespace nim
