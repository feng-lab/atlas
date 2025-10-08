#pragma once

#include "z3dtypes.h"
#include "zglmutils.h"
#include "z3dgl.h"
#include "z3dscratchresourcepool.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <variant>
#include <vector>
#include <limits>

namespace nim {
  
class ZMesh;
class Z3DImg;
class ZColorMap;
class Z3DTransferFunction;
struct RendererParameterState;

enum class BackgroundMode
{
  Uniform,
  Gradient
};

enum class BackgroundGradientOrientation
{
  LeftToRight,
  RightToLeft,
  TopToBottom,
  BottomToTop
};

enum class TextureBlendMode
{
  DepthTest,
  FirstOnTop,
  SecondOnTop,
  DepthTestBlending,
  FirstOnTopBlending,
  SecondOnTopBlending,
  MIPImageDepthTestBlending
};

enum class GlowMode
{
  Additive,
  Screen,
  Softlight,
  Glowmap
};

enum class ImgCompositingMode
{
  DirectVolumeRendering,
  MaximumIntensityProjection,
  MIPOpaque,
  LocalMIP,
  LocalMIPOpaque,
  IsoSurface,
  XRay
};

//------------------------------------------------------------------------------
// Backend handles
//------------------------------------------------------------------------------
enum class AttachmentBackend
{
  Unknown,
  OpenGL,
  Vulkan
};

struct AttachmentHandle
{
  uint64_t id = 0;
  uint32_t index = 0;
  AttachmentBackend backend = AttachmentBackend::Unknown;

  [[nodiscard]] bool valid() const
  {
    return id != 0;
  }
};

// Neutral handle for sampled images (textures) passed in payloads.
// For Vulkan, `id` is a reinterpret_cast<uint64_t>(ZVulkanTexture*).
struct SampledImageHandle
{
  uint64_t id = 0;
  AttachmentBackend backend = AttachmentBackend::Unknown;

  [[nodiscard]] bool valid() const
  {
    return id != 0;
  }
};

struct BufferHandle
{
  uint64_t id = 0;
};

struct ShaderHandle
{
  uint64_t id = 0;
};

struct SamplerHandle
{
  uint64_t id = 0;
};

struct DescriptorSetHandle
{
  uint64_t id = 0;
};

//------------------------------------------------------------------------------
// Attachment clear policy
//------------------------------------------------------------------------------
enum class LoadOp
{
  DontCare,
  Load,
  Clear
};

enum class StoreOp
{
  DontCare,
  Store
};

struct ClearValue
{
  glm::vec4 color = glm::vec4(0.0f);
  float depth = 1.0f;
  uint32_t stencil = 0u;
};

struct AttachmentDesc
{
  AttachmentHandle handle;
  LoadOp loadOp = LoadOp::Load;
  StoreOp storeOp = StoreOp::Store;
  ClearValue clearValue{};
};

struct ViewportDesc
{
  glm::vec2 origin = glm::vec2(0.0f);
  glm::vec2 extent = glm::vec2(0.0f);
  float minDepth = 0.0f;
  float maxDepth = 1.0f;
};

//------------------------------------------------------------------------------
// Pass description
//------------------------------------------------------------------------------
struct BackendPassDesc
{
  enum class Kind
  {
    Raster,
    Compute
  };

  Kind kind = Kind::Raster;

  std::vector<AttachmentDesc> colorAttachments;
  std::optional<AttachmentDesc> depthAttachment;
  std::optional<AttachmentDesc> resolveAttachment;
  // Viewport is the single source of truth for the render area.
  // Backends convert this to native viewport/scissor. If zero, the
  // renderer provides a default from the current frame viewport.
  ViewportDesc viewport;
  bool enableScissor = false;
  glm::vec4 scissorRect{0.0f, 0.0f, 0.0f, 0.0f};
};

//------------------------------------------------------------------------------
// Fixed-function state
//------------------------------------------------------------------------------
enum class PrimitiveTopology
{
  PointList,
  LineList,
  LineStrip,
  TriangleList,
  TriangleStrip
};

enum class CullMode
{
  None,
  Front,
  Back
};

enum class FrontFace
{
  CounterClockwise,
  Clockwise
};

enum class FillMode
{
  Solid,
  Wireframe
};

enum class BlendFactor
{
  One,
  Zero,
  SrcAlpha,
  OneMinusSrcAlpha,
  DstAlpha,
  OneMinusDstAlpha
};

enum class BlendOp
{
  Add,
  Subtract,
  ReverseSubtract
};

struct BlendState
{
  bool enabled = false;
  BlendFactor srcColor = BlendFactor::One;
  BlendFactor dstColor = BlendFactor::Zero;
  BlendOp colorOp = BlendOp::Add;
  BlendFactor srcAlpha = BlendFactor::One;
  BlendFactor dstAlpha = BlendFactor::Zero;
  BlendOp alphaOp = BlendOp::Add;
};

struct DepthStencilState
{
  bool depthTestEnable = true;
  bool depthWriteEnable = true;
  bool stencilEnable = false;
};

struct RasterState
{
  CullMode cullMode = CullMode::Back;
  FrontFace frontFace = FrontFace::CounterClockwise;
  FillMode fillMode = FillMode::Solid;
  bool depthBiasEnable = false;
  float depthBiasConstant = 0.0f;
  float depthBiasSlope = 0.0f;
  BlendState blend;
  DepthStencilState depthStencil;
};

struct VertexAttributeDesc
{
  uint32_t location = 0u;
  uint32_t binding = 0u;
  uint32_t offset = 0u;
  uint32_t stride = 0u;
  uint32_t size = 0u;
  bool normalized = false;
};

struct VertexFormat
{
  std::vector<VertexAttributeDesc> attributes;
  bool usesIndexBuffer = false;
};

struct PipelineStateDesc
{
  ShaderHandle shader;
  VertexFormat vertexFormat;
  RasterState raster;
};

//------------------------------------------------------------------------------
// Resource bindings
//------------------------------------------------------------------------------
enum class ResourceKind
{
  UniformBuffer,
  StorageBuffer,
  SampledImage,
  StorageImage,
  Sampler,
  CombinedSamplerImage
};

struct ResourceBinding
{
  ResourceKind kind = ResourceKind::UniformBuffer;
  uint32_t set = 0u;
  uint32_t slot = 0u;
  BufferHandle buffer;
  size_t offset = 0u;
  size_t range = 0u;
  AttachmentHandle image;
  SamplerHandle sampler;
  DescriptorSetHandle descriptorSet;
};

//------------------------------------------------------------------------------
// Draw command
//------------------------------------------------------------------------------
struct DrawCommand
{
  PrimitiveTopology topology = PrimitiveTopology::TriangleList;
  uint32_t vertexCount = 0u;
  uint32_t firstVertex = 0u;
  uint32_t instanceCount = 1u;
  uint32_t firstInstance = 0u;

  std::optional<BufferHandle> indexBuffer;
  uint32_t indexCount = 0u;
  uint32_t firstIndex = 0u;
  int32_t vertexOffset = 0;
};

//------------------------------------------------------------------------------
// Geometry payloads mirroring existing renderer data
//------------------------------------------------------------------------------
struct LinePayload
{
  uint64_t streamKey = 0;
  // Raw polyline data
  std::span<const glm::vec3> positions;
  std::span<const glm::vec4> colors;
  std::span<const glm::vec4> pickingColors;
  std::span<const float> perSegmentWidths;

  // Smooth-line tessellation buffers (mirrors m_smooth* containers)
  std::span<const glm::vec3> smoothP0Positions;
  std::span<const glm::vec3> smoothP1Positions;
  std::span<const glm::vec4> smoothP0Colors;
  std::span<const glm::vec4> smoothP1Colors;
  std::span<const glm::vec4> smoothPickingColors;
  std::span<const float> smoothFlags;
  std::span<const uint32_t> smoothIndices;

  bool useSmoothLine = true;
  bool useTextureColor = false;
  bool screenAligned = false;
  bool roundCap = true;
  bool isLineStrip = false;
  bool enableMultisample = false;

  float srcLineWidth = 1.0f;
  float resolvedLineWidth = 1.0f;
  const RendererParameterState* params = nullptr; // originating renderer params
  bool pickingPass = false;
  // Generation counters for selective restaging
  uint32_t positionsGen = 0;    // raw polyline positions
  uint32_t smoothGen = 0;       // smooth-line expanded streams/indices
  uint32_t indicesGen = 0;      // index buffer for strips
  uint32_t colorsGen = 0;       // thin-line colors
  uint32_t pickingColorsGen = 0;// thin-line picking colors
  // Wide-line per-stream gens
  uint32_t smoothP0PositionsGen = 0;
  uint32_t smoothP1PositionsGen = 0;
  uint32_t smoothP0ColorsGen = 0;
  uint32_t smoothP1ColorsGen = 0;
  uint32_t smoothPickingColorsGen = 0;
  uint32_t smoothFlagsGen = 0;
  uint32_t smoothIndicesGen = 0;
};

struct LineWideVertex
{
  glm::vec3 p0;
  glm::vec3 p1;
  glm::vec4 c0;
  glm::vec4 c1;
  float flags;
  float _pad = 0.0f;
};

struct MeshPayload
{
  uint64_t streamKey = 0;
  std::span<ZMesh* const> meshes;
  std::span<const glm::vec4> meshColors;
  std::span<const glm::vec4> meshPickingColors;

  enum class ColorSource
  {
    MeshColor,
    Mesh1DTexture,
    Mesh2DTexture,
    Mesh3DTexture,
    CustomColor
  } colorSource = ColorSource::MeshColor;

  enum class WireframeMode
  {
    NoWireframe,
    WithWireframe,
    OnlyWireframe
  } wireframeMode = WireframeMode::NoWireframe;

  glm::vec4 wireframeColor{1.0f};

  SampledImageHandle textureHandle; // Vulkan-native sampled image (optional)

  bool meshNeedsSplit = false;
  bool meshColorReady = false;
  bool meshPickingColorReady = false;
  bool pickingPass = false;
  bool wantsLighting = false;
  // Per-stream generation counters
  uint32_t posGen = 0;
  uint32_t normGen = 0;
  uint32_t colorGen = 0;
  uint32_t texGen = 0;
  uint32_t indexGen = 0;
  const RendererParameterState* params = nullptr; // originating renderer params
};

struct SpherePayload
{
  uint64_t streamKey = 0;
  std::span<const glm::vec4> pointsAndRadius;
  std::span<const glm::vec4> colors;
  std::span<const glm::vec4> pickingColors;
  std::span<const glm::vec4> specularAndShininess;
  std::span<const float> flags;
  std::span<const uint32_t> indices;

  bool useDynamicMaterial = true;
  // Whether the renderer requests lighting for this draw (precomputed)
  bool wantsLighting = false;
  bool pickingPass = false;
  // Per-stream generation counters
  uint32_t centersGen = 0;
  uint32_t colorsGen = 0;
  uint32_t pickingColorsGen = 0;
  uint32_t specularGen = 0;
  uint32_t flagsGen = 0;
  uint32_t indexGen = 0;
  const RendererParameterState* params = nullptr;
};

struct BackgroundPayload
{
  glm::vec4 color1{1.0f};
  glm::vec4 color2{0.0f};
  glm::vec4 region{0.0f, 1.0f, 0.0f, 1.0f};
  BackgroundMode mode = BackgroundMode::Gradient;
  BackgroundGradientOrientation orientation = BackgroundGradientOrientation::BottomToTop;
  const RendererParameterState* params = nullptr; // originating renderer params
};

struct TextureCopyPayload
{
  enum class OutputMode
  {
    NoChange,
    DivideByAlpha,
    MultiplyAlpha
  };

  bool discardTransparent = true;
  OutputMode mode = OutputMode::NoChange;
  bool flipY = false;
  AttachmentHandle colorAttachmentHandle;
  AttachmentHandle depthAttachmentHandle;
};

struct TextureBlendPayload
{
  TextureBlendMode mode = TextureBlendMode::DepthTestBlending;
  AttachmentHandle colorAttachmentHandle0;
  AttachmentHandle depthAttachmentHandle0;
  AttachmentHandle colorAttachmentHandle1;
  AttachmentHandle depthAttachmentHandle1;
};

struct TextureGlowPayload
{
  GlowMode mode = GlowMode::Screen;
  int blurRadius = 0;
  float blurScale = 1.0f;
  float blurStrength = 0.5f;
  AttachmentHandle colorAttachmentHandle;
  AttachmentHandle depthAttachmentHandle;
};

struct TextureDualPeelPayload
{
  enum class Stage
  {
    Blend,
    Final
  } stage = Stage::Blend;

  AttachmentHandle tempAttachment;
  AttachmentHandle frontAttachment;
  AttachmentHandle backAttachment;
  AttachmentHandle depthAttachment;
  glm::vec2 screenDimRcp{1.0f};
};

struct TextureWeightedAveragePayload
{
  AttachmentHandle accumulationAttachment;
  AttachmentHandle momentsAttachment;
  glm::vec2 screenDimRcp{1.0f};
};

struct TextureWeightedBlendedPayload
{
  AttachmentHandle accumulationAttachment;
  AttachmentHandle transmittanceAttachment;
  glm::vec2 screenDimRcp{1.0f};
};

struct ImgSlicePayload
{
  Z3DImg* image = nullptr;
  const std::vector<const ZColorMap*>* colormaps = nullptr;
  std::span<const ZMesh> slices;
  glm::uvec2 outputSize{0u, 0u};
  bool fastPathOnly = true;
  bool maxProjectionMerge = true;
  std::shared_ptr<Z3DScratchResourcePool::RenderTargetLease> layerLease;
};

struct ImgRaycasterPayload
{
  // Stable identity of the source renderer/stream
  uint64_t streamKey = 0;
  Z3DImg* image = nullptr;
  std::shared_ptr<Z3DScratchResourcePool::RenderTargetLease> entryExitLease;
  std::shared_ptr<Z3DScratchResourcePool::RenderTargetLease> blockIdLease;
  std::shared_ptr<Z3DScratchResourcePool::RenderTargetLease> lastAccumLease;
  std::shared_ptr<Z3DScratchResourcePool::RenderTargetLease> currentAccumLease;
  std::shared_ptr<Z3DScratchResourcePool::RenderTargetLease> channelLayerLease;
  glm::uvec2 outputSize{0u, 0u};
  float samplingRate = 1.0f;
  float isoValue = 0.5f;
  float localMIPThreshold = 0.8f;
  ImgCompositingMode compositingMode = ImgCompositingMode::DirectVolumeRendering;
  bool fastPathOnly = true;
  glm::uvec2 entryExitSize{0u, 0u};
  std::vector<glm::vec3> entryPositions;
  std::vector<glm::vec3> entryTexCoords;
  std::vector<uint32_t> entryIndices;
  uint32_t entryPrimitive = 0u;
  bool entryHasIndices = false;
  bool entryFlipped = false;
  std::vector<size_t> visibleChannels;
  // Transfer function inputs (one per channel). The vector must be non-null
  // and have size >= max(visibleChannels)+1 when used by Vulkan.
  const std::vector<Z3DTransferFunction*>* transferFunctions = nullptr;
  std::vector<uint32_t> blockIdReadback;
  uint32_t blockIdAttachmentCount = 0u;
  uint32_t roundsCompleted = 0u;
  uint32_t roundsRemaining = 0u;
  size_t activeChannel = std::numeric_limits<size_t>::max();
  uint32_t activeChannelIndex = 0u;
  uint32_t progressiveGeneration = 0u;
};

struct EllipsoidPayload
{
  uint64_t streamKey = 0;
  std::span<const glm::vec4> centers;
  std::span<const glm::vec4> axis1;
  std::span<const glm::vec4> axis2;
  std::span<const glm::vec4> axis3;
  std::span<const glm::vec4> specularAndShininess;
  std::span<const glm::vec4> colors;
  std::span<const glm::vec4> pickingColors;
  std::span<const float> flags;
  std::span<const uint32_t> indices;

  bool useDynamicMaterial = true;
  bool wantsLighting = false;
  bool pickingPass = false;
  // Per-stream generation counters
  uint32_t centersGen = 0;
  uint32_t axesGen = 0; // any axis change
  uint32_t colorsGen = 0;
  uint32_t pickingColorsGen = 0;
  uint32_t specularGen = 0;
  uint32_t flagsGen = 0;
  uint32_t indexGen = 0;
  const RendererParameterState* params = nullptr;
};

struct ConePayload
{
  uint64_t streamKey = 0;
  std::span<const glm::vec4> baseAndRadius;
  std::span<const glm::vec4> axisAndTopRadius;
  std::span<const glm::vec4> baseColors;
  std::span<const glm::vec4> topColors;
  std::span<const glm::vec4> pickingColors;
  std::span<const float> flags;
  std::span<const uint32_t> indices;

  enum class CapStyle
  {
    FlatCaps,
    NoCaps,
    RoundCaps,
    RoundBaseFlatTop,
    FlatBaseRoundTop
  } capStyle = CapStyle::FlatCaps;

  int subdivisionAround = 36;
  int subdivisionAlong = 1;
  bool sameColorForBaseAndTop = false;
  bool useConeShader2 = true;
  bool pickingPass = false;
  bool wantsLighting = false;
  // Per-stream generation counters
  uint32_t baseGen = 0;
  uint32_t axisGen = 0;
  uint32_t baseColorGen = 0;
  uint32_t topColorGen = 0;
  uint32_t pickingColorsGen = 0;
  uint32_t flagsGen = 0;
  uint32_t indexGen = 0;
  const RendererParameterState* params = nullptr;
};

struct FontPayload
{
  uint64_t streamKey = 0;

  // CPU-side geometry generated by Z3DFontRenderer::prepareFontShaderData
  std::span<const glm::vec3> positions;
  std::span<const glm::vec2> texcoords;
  std::span<const glm::vec4> colors;
  std::span<const glm::vec4> pickingColors;
  std::span<const uint32_t> indices;

  // Glyph atlas: provide a native Vulkan handle or CPU pixels (BGRA8) + dimensions.
  SampledImageHandle atlasHandle;
  const uint8_t* atlasPixels = nullptr;
  uint32_t atlasWidth = 0;
  uint32_t atlasHeight = 0;

  // SDF text parameters
  float softedgeScale = 80.0f;
  bool showOutline = false;
  bool showShadow = false;
  // 0 = Glow, 1 = Outline
  int outlineMode = 0;
  glm::vec4 outlineColor = glm::vec4(1.0f);
  glm::vec4 shadowColor = glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);

  bool pickingPass = false;
  // Per-stream generation counters
  uint32_t positionsGen = 0;
  uint32_t texcoordsGen = 0;
  uint32_t colorsGen = 0;
  uint32_t pickingColorsGen = 0;
  uint32_t indicesGen = 0;
  const RendererParameterState* params = nullptr;
};

using GeometryPayload = std::variant<std::monostate,
                                     LinePayload,
                                     MeshPayload,
                                     SpherePayload,
                                     BackgroundPayload,
                                     ImgSlicePayload,
                                     ImgRaycasterPayload,
                                     TextureCopyPayload,
                                     TextureBlendPayload,
                                     TextureGlowPayload,
                                     TextureDualPeelPayload,
                                     TextureWeightedAveragePayload,
                                     TextureWeightedBlendedPayload,
                                     EllipsoidPayload,
                                     ConePayload,
                                     FontPayload>;

struct RenderBatch
{
  Z3DEye eye = MonoEye;
  BackendPassDesc pass;
  PipelineStateDesc pipeline;
  std::vector<ResourceBinding> resources;
  DrawCommand draw;
  GeometryPayload geometry;
};

struct RendererCPUState
{
  std::vector<RenderBatch> batches;
};

// ---------------------------------------------------------------------------
// Span helpers for renderers to expose existing std::vector storage
// ---------------------------------------------------------------------------
template<typename T>
inline std::span<const T> spanOrEmpty(const std::vector<T>& vec)
{
  if (!vec.empty()) {
    return std::span<const T>(vec.data(), vec.size());
  }
  return std::span<const T>();
}

template<typename T>
inline std::span<const T> spanOrEmpty(const std::vector<T>* ptr)
{
  if (ptr && !ptr->empty()) {
    return std::span<const T>(ptr->data(), ptr->size());
  }
  return std::span<const T>();
}

inline std::span<const float> spanFromGLfloats(const std::vector<GLfloat>& vec)
{
  if (vec.empty()) {
    return std::span<const float>();
  }
  static_assert(sizeof(GLfloat) == sizeof(float), "GLfloat expected to match float");
  return std::span<const float>(reinterpret_cast<const float*>(vec.data()), vec.size());
}

inline std::span<const uint32_t> spanFromGLuints(const std::vector<GLuint>& vec)
{
  if (vec.empty()) {
    return std::span<const uint32_t>();
  }
  static_assert(sizeof(GLuint) == sizeof(uint32_t), "GLuint expected to be 32-bit");
  return std::span<const uint32_t>(reinterpret_cast<const uint32_t*>(vec.data()), vec.size());
}

} // namespace nim
