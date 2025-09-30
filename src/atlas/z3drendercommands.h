#pragma once

#include "z3dtypes.h"
#include "zglmutils.h"
#include "z3dgl.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <variant>
#include <vector>

namespace nim {

class Z3DTexture;
class ZMesh;
class Z3DLineRenderer;
class Z3DMeshRenderer;
class Z3DEllipsoidRenderer;
class Z3DConeRenderer;

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

  glm::uvec2 extent{0u, 0u};

  std::vector<AttachmentDesc> colorAttachments;
  std::optional<AttachmentDesc> depthAttachment;
  std::optional<AttachmentDesc> resolveAttachment;

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
  Z3DLineRenderer* renderer = nullptr;
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

  Z3DTexture* texture = nullptr;

  bool useSmoothLine = true;
  bool useTextureColor = false;
  bool screenAligned = false;
  bool roundCap = true;
  bool isLineStrip = false;
  bool enableMultisample = false;

  float srcLineWidth = 1.0f;
  float resolvedLineWidth = 1.0f;
  bool pickingPass = false;
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
  Z3DMeshRenderer* renderer = nullptr;
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

  Z3DTexture* texture = nullptr;

  bool meshNeedsSplit = false;
  bool meshColorReady = false;
  bool meshPickingColorReady = false;
};

struct EllipsoidPayload
{
  Z3DEllipsoidRenderer* renderer = nullptr;
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
};

struct ConePayload
{
  Z3DConeRenderer* renderer = nullptr;
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
};

using GeometryPayload = std::variant<std::monostate, LinePayload, MeshPayload, EllipsoidPayload, ConePayload>;

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
