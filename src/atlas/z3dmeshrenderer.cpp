#include "z3dmeshrenderer.h"

#include "z3dgl.h"
#include "z3dgpuinfo.h"
#include "z3dshaderprogram.h"
#include "z3dtexture.h"
#include "zlog.h"
#include "zmesh.h"
#include "zoptionparameter.h"
#include "znumericparameter.h"

#include <gflags/gflags.h>

#include <algorithm>
#include <utility>
#include <limits>

namespace nim {

namespace {

template<typename T>
[[nodiscard]] GLsizeiptr bufferByteSize(const std::vector<T>& values)
{
  return static_cast<GLsizeiptr>(values.size() * sizeof(T));
}

template<typename T>
[[nodiscard]] std::span<T* const> subspanOrEmpty(const std::vector<T*>* ptr, size_t start, size_t count)
{
  if (ptr == nullptr || count == 0) {
    return {};
  }
  CHECK(start + count <= ptr->size()) << "Mesh payload pointer subspan exceeds backing vector";
  return std::span<T* const>(*ptr).subspan(start, count);
}

static_assert(sizeof(glm::vec2) == sizeof(GLfloat) * 2, "Z3DMeshRenderer assumes glm::vec2 is tightly packed");
static_assert(sizeof(glm::vec3) == sizeof(GLfloat) * 3, "Z3DMeshRenderer assumes glm::vec3 is tightly packed");
static_assert(sizeof(glm::vec4) == sizeof(GLfloat) * 4, "Z3DMeshRenderer assumes glm::vec4 is tightly packed");

[[nodiscard]] constexpr size_t ceilDiv(size_t numerator, size_t denominator)
{
  return denominator == 0 ? 0 : (numerator + denominator - 1) / denominator;
}

} // namespace

DEFINE_uint64(
  atlas_mesh_preferred_triangle_budget_per_segment,
  1000000,
  "Preferred mesh triangle budget for one renderer-owned segment. This is the normal performance/UX target; "
  "backends still enforce maxMonolithicGeometryStreamBytes as a hard safety guard.");

Z3DMeshRenderer::Z3DMeshRenderer(Z3DRendererBase& rendererBase)
  : Z3DPrimitiveRenderer(rendererBase)
  , m_meshPt(nullptr)
  , m_meshColorsPt(nullptr)
  , m_meshPickingColorsPt(nullptr)
  , m_origMeshPt(nullptr)
  , m_origMeshColorsPt(nullptr)
  , m_origMeshPickingColorsPt(nullptr)
  , m_texture(nullptr)
  , m_meshColorReady(false)
  , m_meshPickingColorReady(false)
  , m_dataChanged(false)
  , m_pickingDataChanged(false)
{
  m_colorSource = MeshColorSource::MeshColor;

#if !defined(ATLAS_USE_CORE_PROFILE) && defined(ATLAS_SUPPORT_FIXED_PIPELINE)
  setUseDisplayList(true);
#endif
  createResources(m_rendererBase.activeBackend());
}

void Z3DMeshRenderer::setData(std::vector<ZMesh*>* meshInput)
{
  m_origMeshPt = meshInput;
  m_meshPt = meshInput;
  invalidateMeshSegmentationPlan();
  // Per-mesh transforms are tied to the current mesh pointer list; clear them
  // so callers must explicitly re-provide matching arrays after setData().
  m_origMeshPosTransformsPt = nullptr;
  m_origMeshPosTransformNormalMatricesPt = nullptr;
  m_meshPosTransformsPt = nullptr;
  m_meshPosTransformNormalMatricesPt = nullptr;

#if !defined(ATLAS_USE_CORE_PROFILE) && defined(ATLAS_SUPPORT_FIXED_PIPELINE)
  invalidateOpenglRenderer();
  invalidateOpenglPickingRenderer();
#endif
  m_dataChanged = true;
  m_pickingDataChanged = true;
  // Bump geometry-related generations (positions/normals/indices)
  m_posGen++;
  m_normGen++;
  m_indexGen++;
}

void Z3DMeshRenderer::setDataColors(std::vector<glm::vec4>* meshColorsInput)
{
  m_origMeshColorsPt = meshColorsInput;
  m_meshColorsPt = nullptr;
  m_meshColorReady = false;
#if !defined(ATLAS_USE_CORE_PROFILE) && defined(ATLAS_SUPPORT_FIXED_PIPELINE)
  invalidateOpenglRenderer();
#endif
  m_dataChanged = true;
  // Only colors stream changed
  m_colorGen++;
}

void Z3DMeshRenderer::setTexture(Z3DTexture* tex)
{
  m_texture = tex;
#if !defined(ATLAS_USE_CORE_PROFILE) && defined(ATLAS_SUPPORT_FIXED_PIPELINE)
  invalidateOpenglRenderer();
#endif
  m_dataChanged = true;
}

void Z3DMeshRenderer::setDataPickingColors(std::vector<glm::vec4>* meshPickingColorsInput)
{
  m_origMeshPickingColorsPt = meshPickingColorsInput;
  m_meshPickingColorsPt = nullptr;
  m_meshPickingColorReady = false;
#if !defined(ATLAS_USE_CORE_PROFILE) && defined(ATLAS_SUPPORT_FIXED_PIPELINE)
  invalidateOpenglPickingRenderer();
#endif
  m_pickingDataChanged = true;
}

void Z3DMeshRenderer::setPerMeshPosTransforms(std::vector<glm::mat4>* meshPosTransformsInput,
                                              std::vector<glm::mat3>* meshPosTransformNormalMatricesInput)
{
  if (meshPosTransformsInput == nullptr || meshPosTransformNormalMatricesInput == nullptr) {
    m_origMeshPosTransformsPt = nullptr;
    m_origMeshPosTransformNormalMatricesPt = nullptr;
    prepareMeshTransforms();
    return;
  }

  CHECK(meshPosTransformsInput->size() == meshPosTransformNormalMatricesInput->size())
    << "Per-mesh transform and normal-matrix arrays must have the same size";
  if (m_origMeshPt != nullptr) {
    CHECK(meshPosTransformsInput->size() == m_origMeshPt->size())
      << "Per-mesh transform arrays must match the mesh list size";
  }

  m_origMeshPosTransformsPt = meshPosTransformsInput;
  m_origMeshPosTransformNormalMatricesPt = meshPosTransformNormalMatricesInput;
  prepareMeshTransforms();
}

void Z3DMeshRenderer::setColorSource(MeshColorSource source)
{
  if (m_colorSource == source) {
    return;
  }
  m_colorSource = source;
  invalidateMeshSegmentationPlan();
  m_texGen++;
  compile();
}

void Z3DMeshRenderer::compile()
{
  if (m_rendererBase.activeBackend() != RenderBackend::OpenGL) {
    return;
  }
  DCHECK(m_meshShaderGrp != nullptr);
  m_dataChanged = true;
  m_meshShaderGrp->rebuild(m_rendererBase.generateHeader() + generateHeader());
}

std::string Z3DMeshRenderer::generateHeader()
{
  std::string header;
  switch (m_colorSource) {
    case MeshColorSource::MeshColor:
      header = "#define USE_MESH_COLOR\n";
      break;
    case MeshColorSource::Mesh1DTexture:
      header = "#define USE_MESH_1DTEXTURE\n";
      break;
    case MeshColorSource::Mesh2DTexture:
      header = "#define USE_MESH_2DTEXTURE\n";
      break;
    case MeshColorSource::Mesh3DTexture:
      header = "#define USE_MESH_3DTEXTURE\n";
      break;
    case MeshColorSource::CustomColor:
      break;
  }

  // mesh_func.frag always sets fragDepth = gl_FragCoord.z; avoid redundant
  // gl_FragDepth writes so the driver can keep early depth/stencil optimizations.
  header += "#define ATLAS_DISABLE_FRAG_DEPTH_WRITE\n";
  return header;
}

void Z3DMeshRenderer::invalidateMeshSegmentationPlan()
{
  m_segmentationPlan = {};
  m_meshPt = m_origMeshPt;
  m_meshColorsPt = nullptr;
  m_meshPickingColorsPt = nullptr;
  m_meshPosTransformsPt = m_origMeshPosTransformsPt;
  m_meshPosTransformNormalMatricesPt = m_origMeshPosTransformNormalMatricesPt;
  m_splitMeshesColors.clear();
  m_splitMeshesPickingColors.clear();
  m_splitMeshesPosTransforms.clear();
  m_splitMeshesPosTransformNormalMatrices.clear();
  m_meshColorReady = false;
  m_meshPickingColorReady = false;
}

size_t Z3DMeshRenderer::maxMonolithicGeometryStreamBytesForBackend(RenderBackend backend) const
{
  CHECK(backend == m_rendererBase.activeBackend())
    << "Mesh segmentation plan requires the active backend's geometry-stream limit";
  const auto* renderBackend = m_rendererBase.backend();
  CHECK(renderBackend != nullptr) << "Mesh segmentation plan requires an active backend";
  return renderBackend->maxMonolithicGeometryStreamBytes();
}

size_t Z3DMeshRenderer::textureStreamBytesForVertexCount(size_t vertexCount) const
{
  switch (m_colorSource) {
    case MeshColorSource::Mesh1DTexture:
      return vertexCount * sizeof(float);
    case MeshColorSource::Mesh2DTexture:
      return vertexCount * sizeof(glm::vec2);
    case MeshColorSource::Mesh3DTexture:
      return vertexCount * sizeof(glm::vec3);
    case MeshColorSource::MeshColor:
    case MeshColorSource::CustomColor:
    default:
      return size_t{0};
  }
}

bool Z3DMeshRenderer::rangeFitsMonolithicStreamLimit(size_t vertexCount, size_t indexCount, size_t maxStreamBytes) const
{
  if (maxStreamBytes == std::numeric_limits<size_t>::max()) {
    return true;
  }

  return vertexCount * sizeof(glm::vec3) <= maxStreamBytes && vertexCount * sizeof(glm::vec3) <= maxStreamBytes &&
         vertexCount * sizeof(glm::vec4) <= maxStreamBytes &&
         textureStreamBytesForVertexCount(vertexCount) <= maxStreamBytes &&
         indexCount * sizeof(uint32_t) <= maxStreamBytes;
}

void Z3DMeshRenderer::prepareMeshTransforms()
{
  m_splitMeshesPosTransforms.clear();
  m_splitMeshesPosTransformNormalMatrices.clear();
  m_meshPosTransformsPt = m_origMeshPosTransformsPt;
  m_meshPosTransformNormalMatricesPt = m_origMeshPosTransformNormalMatricesPt;

  if (m_origMeshPosTransformsPt == nullptr || m_origMeshPosTransformNormalMatricesPt == nullptr) {
    return;
  }

  CHECK(m_origMeshPt != nullptr) << "Per-mesh transforms require a mesh list";
  CHECK(m_origMeshPosTransformsPt->size() == m_origMeshPt->size())
    << "Per-mesh transform arrays must match the original mesh list size";
  CHECK(m_origMeshPosTransformNormalMatricesPt->size() == m_origMeshPt->size())
    << "Per-mesh transform normal matrices must match the original mesh list size";

  if (!m_segmentationPlan.valid || !m_segmentationPlan.anyMeshSplit) {
    return;
  }

  CHECK(m_meshPt != nullptr) << "Split transform preparation requires an active mesh list";
  CHECK(m_segmentationPlan.activeMeshSourceIndices.size() == m_meshPt->size())
    << "Mesh segmentation source-index metadata must match active mesh count";
  m_splitMeshesPosTransforms.reserve(m_meshPt->size());
  m_splitMeshesPosTransformNormalMatrices.reserve(m_meshPt->size());
  for (const size_t sourceIndex : m_segmentationPlan.activeMeshSourceIndices) {
    CHECK(sourceIndex < m_origMeshPosTransformsPt->size());
    CHECK(sourceIndex < m_origMeshPosTransformNormalMatricesPt->size());
    m_splitMeshesPosTransforms.push_back((*m_origMeshPosTransformsPt)[sourceIndex]);
    m_splitMeshesPosTransformNormalMatrices.push_back((*m_origMeshPosTransformNormalMatricesPt)[sourceIndex]);
  }
  m_meshPosTransformsPt = &m_splitMeshesPosTransforms;
  m_meshPosTransformNormalMatricesPt = &m_splitMeshesPosTransformNormalMatrices;
}

void Z3DMeshRenderer::ensureMeshSegmentationPlan(RenderBackend backend)
{
  const size_t maxStreamBytes = maxMonolithicGeometryStreamBytesForBackend(backend);
  const size_t preferredTriangleBudget =
    std::max<size_t>(1, static_cast<size_t>(FLAGS_atlas_mesh_preferred_triangle_budget_per_segment));
  if (m_segmentationPlan.valid && m_segmentationPlan.backend == backend &&
      m_segmentationPlan.maxMonolithicGeometryStreamBytes == maxStreamBytes &&
      m_segmentationPlan.colorSource == m_colorSource) {
    return;
  }

  m_segmentationPlan = {};
  m_segmentationPlan.valid = true;
  m_segmentationPlan.backend = backend;
  m_segmentationPlan.maxMonolithicGeometryStreamBytes = maxStreamBytes;
  m_segmentationPlan.colorSource = m_colorSource;

  m_meshPt = m_origMeshPt;
  m_meshColorsPt = nullptr;
  m_meshPickingColorsPt = nullptr;
  m_meshColorReady = false;
  m_meshPickingColorReady = false;
  m_splitMeshesColors.clear();
  m_splitMeshesPickingColors.clear();

  if (m_origMeshPt == nullptr || m_origMeshPt->empty()) {
    prepareMeshTransforms();
    return;
  }

  size_t physicalSplitTriangleLimit = preferredTriangleBudget;
  if (maxStreamBytes != std::numeric_limits<size_t>::max()) {
    const size_t bytesPerSplitTriangle =
      3 * sizeof(glm::vec3) + 3 * sizeof(glm::vec3) + 3 * sizeof(glm::vec4) + textureStreamBytesForVertexCount(3);
    CHECK(bytesPerSplitTriangle > 0) << "Mesh split byte accounting must stay positive";
    const size_t hardTriangleBudget = std::max(size_t{1}, maxStreamBytes / bytesPerSplitTriangle);
    physicalSplitTriangleLimit = std::min(physicalSplitTriangleLimit, hardTriangleBudget);
  }
  physicalSplitTriangleLimit = std::max(size_t{1}, physicalSplitTriangleLimit);

  auto requiresPhysicalSplit = [&](const ZMesh& mesh) {
    return mesh.numTriangles() > physicalSplitTriangleLimit;
  };

  size_t activeMeshCount = 0;
  size_t ownedSplitMeshCount = 0;
  bool anyMeshSplit = false;
  for (size_t meshIndex = 0; meshIndex < m_origMeshPt->size(); ++meshIndex) {
    const ZMesh* mesh = (*m_origMeshPt)[meshIndex];
    CHECK(mesh != nullptr) << "Mesh renderer expects non-null mesh pointers";
    if (!requiresPhysicalSplit(*mesh)) {
      ++activeMeshCount;
      continue;
    }

    const size_t splitCount = std::max(size_t{1}, ceilDiv(mesh->numTriangles(), physicalSplitTriangleLimit));
    activeMeshCount += splitCount;
    ownedSplitMeshCount += splitCount;
    anyMeshSplit = true;
  }

  m_segmentationPlan.anyMeshSplit = anyMeshSplit;
  if (anyMeshSplit) {
    m_segmentationPlan.ownedSplitMeshes.reserve(ownedSplitMeshCount);
    m_segmentationPlan.activeMeshPtrs.reserve(activeMeshCount);
    m_segmentationPlan.activeMeshSourceIndices.reserve(activeMeshCount);

    for (size_t meshIndex = 0; meshIndex < m_origMeshPt->size(); ++meshIndex) {
      ZMesh* mesh = (*m_origMeshPt)[meshIndex];
      CHECK(mesh != nullptr) << "Mesh renderer expects non-null mesh pointers";
      if (!requiresPhysicalSplit(*mesh)) {
        m_segmentationPlan.activeMeshPtrs.push_back(mesh);
        m_segmentationPlan.activeMeshSourceIndices.push_back(meshIndex);
        continue;
      }

      std::vector<ZMesh> splitMeshes = mesh->split(physicalSplitTriangleLimit);
      CHECK(!splitMeshes.empty()) << "Physical mesh split must produce at least one piece";
      const size_t start = m_segmentationPlan.ownedSplitMeshes.size();
      for (auto& splitMesh : splitMeshes) {
        m_segmentationPlan.ownedSplitMeshes.push_back(std::move(splitMesh));
      }
      for (size_t splitIndex = 0; splitIndex < splitMeshes.size(); ++splitIndex) {
        m_segmentationPlan.activeMeshPtrs.push_back(&m_segmentationPlan.ownedSplitMeshes[start + splitIndex]);
        m_segmentationPlan.activeMeshSourceIndices.push_back(meshIndex);
      }
    }
    m_meshPt = &m_segmentationPlan.activeMeshPtrs;
    LOG(INFO) << fmt::format("Mesh renderer built a split segmentation plan: backend={} meshes_before={} "
                             "meshes_after={} preferred_triangles={} hard_guard_bytes={}B",
                             enumOrUnderlying(backend, 16),
                             m_origMeshPt->size(),
                             m_meshPt->size(),
                             preferredTriangleBudget,
                             maxStreamBytes);
  }

  const std::vector<ZMesh*>* activeMeshes = anyMeshSplit ? &m_segmentationPlan.activeMeshPtrs : m_origMeshPt;
  CHECK(activeMeshes != nullptr) << "Mesh segmentation plan requires an active mesh list";
  if (!activeMeshes->empty()) {
    size_t rangeStart = 0;
    size_t rangeTriangleCount = 0;
    size_t rangeVertexCount = 0;
    size_t rangeIndexCount = 0;
    uint32_t segmentOrdinal = 0;

    auto flushRange = [&](size_t rangeEnd) {
      if (rangeEnd <= rangeStart) {
        return;
      }
      m_segmentationPlan.batchSegments.push_back(MeshBatchSegment{rangeStart, rangeEnd - rangeStart, segmentOrdinal++});
      rangeStart = rangeEnd;
      rangeTriangleCount = 0;
      rangeVertexCount = 0;
      rangeIndexCount = 0;
    };

    for (size_t meshIndex = 0; meshIndex < activeMeshes->size(); ++meshIndex) {
      const ZMesh* mesh = (*activeMeshes)[meshIndex];
      CHECK(mesh != nullptr) << "Mesh segmentation plan expects non-null mesh pointers";
      const size_t meshTriangleCount = mesh->numTriangles();
      const size_t meshVertexCount = mesh->numVertices();
      const size_t meshIndexCount = mesh->indices().size();
      const size_t candidateTriangleCount = rangeTriangleCount + meshTriangleCount;
      const size_t candidateVertexCount = rangeVertexCount + meshVertexCount;
      const size_t candidateIndexCount = rangeIndexCount + meshIndexCount;
      if (meshIndex > rangeStart &&
          ((rangeTriangleCount > 0 && candidateTriangleCount > preferredTriangleBudget) ||
           !rangeFitsMonolithicStreamLimit(candidateVertexCount, candidateIndexCount, maxStreamBytes))) {
        flushRange(meshIndex);
      }

      rangeTriangleCount += meshTriangleCount;
      rangeVertexCount += meshVertexCount;
      rangeIndexCount += meshIndexCount;
      CHECK(rangeFitsMonolithicStreamLimit(rangeVertexCount, rangeIndexCount, maxStreamBytes))
        << "Renderer-owned mesh segmentation produced a batch that still exceeds the backend hard stream limit";
    }
    flushRange(activeMeshes->size());
  }

  prepareMeshTransforms();
}

void Z3DMeshRenderer::prepareMeshColor()
{
  m_meshColorReady = true;
  m_splitMeshesColors.clear();
  m_meshColorsPt = m_origMeshColorsPt;
  if (m_origMeshPt == nullptr || !m_origMeshColorsPt || m_origMeshColorsPt->size() < m_origMeshPt->size()) {
    return;
  }
  if (m_segmentationPlan.valid && m_segmentationPlan.anyMeshSplit) {
    CHECK(m_meshPt != nullptr) << "Split mesh colors require an active mesh list";
    CHECK(m_segmentationPlan.activeMeshSourceIndices.size() == m_meshPt->size())
      << "Mesh segmentation source-index metadata must match active mesh count";
    m_splitMeshesColors.reserve(m_meshPt->size());
    for (const size_t sourceIndex : m_segmentationPlan.activeMeshSourceIndices) {
      CHECK(sourceIndex < m_origMeshColorsPt->size());
      m_splitMeshesColors.push_back((*m_origMeshColorsPt)[sourceIndex]);
    }
    m_meshColorsPt = &m_splitMeshesColors;
  }
}

void Z3DMeshRenderer::prepareMeshPickingColor()
{
  m_meshPickingColorReady = true;
  m_splitMeshesPickingColors.clear();
  m_meshPickingColorsPt = m_origMeshPickingColorsPt;
  if (m_origMeshPt == nullptr || !m_origMeshPickingColorsPt ||
      m_origMeshPickingColorsPt->size() < m_origMeshPt->size()) {
    return;
  }
  if (m_segmentationPlan.valid && m_segmentationPlan.anyMeshSplit) {
    CHECK(m_meshPt != nullptr) << "Split mesh picking colors require an active mesh list";
    CHECK(m_segmentationPlan.activeMeshSourceIndices.size() == m_meshPt->size())
      << "Mesh segmentation source-index metadata must match active mesh count";
    m_splitMeshesPickingColors.reserve(m_meshPt->size());
    for (const size_t sourceIndex : m_segmentationPlan.activeMeshSourceIndices) {
      CHECK(sourceIndex < m_origMeshPickingColorsPt->size());
      m_splitMeshesPickingColors.push_back((*m_origMeshPickingColorsPt)[sourceIndex]);
    }
    m_meshPickingColorsPt = &m_splitMeshesPickingColors;
  }
}

MeshPayload Z3DMeshRenderer::buildMeshPayload() const
{
  return buildMeshPayload(0, m_meshPt ? m_meshPt->size() : 0u, 0u);
}

MeshPayload Z3DMeshRenderer::buildMeshPayload(size_t meshStart, size_t meshCount, uint32_t streamSegmentOrdinal) const
{
  MeshPayload payload;
  payload.streamKey = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(this));
  payload.streamSegmentOrdinal = streamSegmentOrdinal;

  payload.meshes = subspanOrEmpty(m_meshPt, meshStart, meshCount);
  if (m_meshColorReady) {
    payload.meshColors = subspanOrEmpty(m_meshColorsPt, meshStart, meshCount);
  }
  if (m_meshPickingColorReady) {
    payload.meshPickingColors = subspanOrEmpty(m_meshPickingColorsPt, meshStart, meshCount);
  }
  payload.perMeshPosTransforms = subspanOrEmpty(m_meshPosTransformsPt, meshStart, meshCount);
  payload.perMeshPosTransformNormalMatrices = subspanOrEmpty(m_meshPosTransformNormalMatricesPt, meshStart, meshCount);
  payload.textureHandle = m_textureHandle;
  payload.meshNeedsSplit = m_segmentationPlan.anyMeshSplit;
  payload.meshColorReady = m_meshColorReady;
  payload.meshPickingColorReady = m_meshPickingColorReady;

  switch (m_colorSource) {
    case MeshColorSource::MeshColor:
      payload.colorSource = MeshPayload::ColorSource::MeshColor;
      break;
    case MeshColorSource::Mesh1DTexture:
      payload.colorSource = MeshPayload::ColorSource::Mesh1DTexture;
      break;
    case MeshColorSource::Mesh2DTexture:
      payload.colorSource = MeshPayload::ColorSource::Mesh2DTexture;
      break;
    case MeshColorSource::Mesh3DTexture:
      payload.colorSource = MeshPayload::ColorSource::Mesh3DTexture;
      break;
    case MeshColorSource::CustomColor:
      payload.colorSource = MeshPayload::ColorSource::CustomColor;
      break;
  }

  switch (m_wireframeModeValue) {
    case WireframeMode::NoWireframe:
      payload.wireframeMode = MeshPayload::WireframeMode::NoWireframe;
      break;
    case WireframeMode::WithWireframe:
      payload.wireframeMode = MeshPayload::WireframeMode::WithWireframe;
      break;
    case WireframeMode::OnlyWireframe:
      payload.wireframeMode = MeshPayload::WireframeMode::OnlyWireframe;
      break;
  }

  payload.wireframeColor = m_wireframeColorValue;

  // Per-stream generation counters (coarse-grained)
  payload.posGen = m_posGen;
  payload.normGen = m_normGen;
  payload.colorGen = m_colorGen;
  payload.texGen = m_texGen;   // Note: texcoords live on meshes; renderer cannot always detect
  payload.indexGen = m_indexGen;

  payload.params = m_rendererBase.parameterState();
  payload.paramsCaptured = true;
  payload.wantsLighting = needLighting();
  // GL parity: carry follow flags so Vulkan respects per-renderer toggles
  payload.followCoordTransform = m_followCoordTransform;
  payload.followSizeScale = m_followSizeScale;
  payload.followOpacity = m_followOpacity;
  return payload;
}

RenderBatch Z3DMeshRenderer::buildRenderBatch(Z3DEye eye, bool picking) const
{
  return buildRenderBatch(eye, picking, 0, m_meshPt ? m_meshPt->size() : 0u, 0u);
}

RenderBatch Z3DMeshRenderer::buildRenderBatch(Z3DEye eye,
                                              bool picking,
                                              size_t meshStart,
                                              size_t meshCount,
                                              uint32_t streamSegmentOrdinal) const
{
  RenderBatch batch;

  batch.eye = eye;

  batch.draw.topology = PrimitiveTopology::TriangleList;

  uint32_t vertexCount = 0u;
  uint32_t indexCount = 0u;
  if (m_meshPt) {
    CHECK(meshStart + meshCount <= m_meshPt->size()) << "Mesh batch segment exceeds renderer mesh list";
    for (size_t i = meshStart; i < meshStart + meshCount; ++i) {
      const auto* mesh = (*m_meshPt)[i];
      if (!mesh) {
        continue;
      }
      vertexCount += static_cast<uint32_t>(mesh->numVertices());
      indexCount += static_cast<uint32_t>(mesh->indices().size());
    }
  }
  batch.draw.vertexCount = vertexCount;
  batch.draw.indexCount = indexCount;

  auto payload = buildMeshPayload(meshStart, meshCount, streamSegmentOrdinal);
  payload.pickingPass = picking;
  batch.geometry = std::move(payload);

  return batch;
}

#if !defined(ATLAS_USE_CORE_PROFILE) && defined(ATLAS_SUPPORT_FIXED_PIPELINE)
void Z3DMeshRenderer::renderUsingOpengl()
{
  ensureMeshSegmentationPlan(RenderBackend::OpenGL);
  if (!m_meshPt || m_meshPt->empty()) {
    return;
  }

  if (m_colorSource == MeshColorSource::CustomColor && (!m_meshColorReady || m_meshColorsPt == nullptr)) {
    prepareMeshColor();
  }

  for (size_t i = 0; i < m_meshPt->size(); ++i) {
    if (m_colorSource == MeshColorSource::MeshColor && (*m_meshPt)[i]->numColors() < (*m_meshPt)[i]->numVertices()) {
      return;
    }
    if (m_colorSource == MeshColorSource::Mesh1DTexture &&
        ((*m_meshPt)[i]->num1DTextureCoordinates() < (*m_meshPt)[i]->numVertices() || !m_texture ||
         m_texture->textureTarget() != GL_TEXTURE_1D)) {
      return;
    }
    if (m_colorSource == MeshColorSource::Mesh2DTexture &&
        ((*m_meshPt)[i]->num2DTextureCoordinates() < (*m_meshPt)[i]->numVertices() || !m_texture ||
         m_texture->textureTarget() != GL_TEXTURE_2D)) {
      return;
    }
    if (m_colorSource == MeshColorSource::Mesh3DTexture &&
        ((*m_meshPt)[i]->num3DTextureCoordinates() < (*m_meshPt)[i]->numVertices() || !m_texture ||
         m_texture->textureTarget() != GL_TEXTURE_3D)) {
      return;
    }
    if (m_colorSource == MeshColorSource::CustomColor &&
        (!m_meshColorsPt || m_meshColorsPt->size() < m_meshPt->size())) {
      return;
    }
    if ((*m_meshPt)[i]->numNormals() != (*m_meshPt)[i]->numVertices()) {
      (*m_meshPt)[i]->generateNormals();
    }
  }

  glMatrixMode(GL_MODELVIEW);
  glPushMatrix();
  // glScalef(getCoordTransform().x, getCoordTransform().y, getCoordTransform().z);
  glMultMatrixf(&coordTransform()[0][0]); // not sure, todo check

  if (m_colorSource == MeshColorSource::Mesh2DTexture || m_colorSource == MeshColorSource::Mesh3DTexture) {
    glActiveTexture(GL_TEXTURE0);
    m_texture->bind();
  }

  for (size_t i = 0; i < m_meshPt->size(); ++i) {
    const std::vector<glm::vec3>& vertices = (*m_meshPt)[i]->vertices();
    const std::vector<glm::vec3>& normals = (*m_meshPt)[i]->normals();
    const std::vector<glm::vec4>& colors = (*m_meshPt)[i]->colors();
    const std::vector<float>& texture1DCoords = (*m_meshPt)[i]->textureCoordinates1D();
    const std::vector<glm::vec2>& texture2DCoords = (*m_meshPt)[i]->textureCoordinates2D();
    const std::vector<glm::vec3>& texture3DCoords = (*m_meshPt)[i]->textureCoordinates3D();
    const std::vector<GLuint>& triangleIndexes = (*m_meshPt)[i]->indices();
    GLenum type = (*m_meshPt)[i]->type();

    GLuint bufObjects[4];
    glGenBuffers(4, bufObjects);

    glEnableClientState(GL_VERTEX_ARRAY);
    glBindBuffer(GL_ARRAY_BUFFER, bufObjects[0]);
    glBufferData(GL_ARRAY_BUFFER, bufferByteSize(vertices), vertices.data(), GL_STATIC_DRAW);
    glVertexPointer(3, GL_FLOAT, 0, 0);

    if (m_colorSource == MeshColorSource::MeshColor) {
      glEnableClientState(GL_COLOR_ARRAY);
      glBindBuffer(GL_ARRAY_BUFFER, bufObjects[1]);
      glBufferData(GL_ARRAY_BUFFER, bufferByteSize(colors), colors.data(), GL_STATIC_DRAW);
      glColorPointer(4, GL_FLOAT, 0, 0);
    } else if (m_colorSource == MeshColorSource::CustomColor) {
      glColor4f((*m_meshColorsPt)[i].r,
                (*m_meshColorsPt)[i].g,
                (*m_meshColorsPt)[i].b,
                (*m_meshColorsPt)[i].a * opacity());
    } else if (m_colorSource == MeshColorSource::Mesh1DTexture) {
      glClientActiveTexture(GL_TEXTURE0);
      glEnableClientState(GL_TEXTURE_COORD_ARRAY);
      glBindBuffer(GL_ARRAY_BUFFER, bufObjects[1]);
      glBufferData(GL_ARRAY_BUFFER, bufferByteSize(texture1DCoords), texture1DCoords.data(), GL_STATIC_DRAW);
      glTexCoordPointer(1, GL_FLOAT, 0, 0);
    } else if (m_colorSource == MeshColorSource::Mesh2DTexture) {
      glClientActiveTexture(GL_TEXTURE0);
      glEnableClientState(GL_TEXTURE_COORD_ARRAY);
      glBindBuffer(GL_ARRAY_BUFFER, bufObjects[1]);
      glBufferData(GL_ARRAY_BUFFER, bufferByteSize(texture2DCoords), texture2DCoords.data(), GL_STATIC_DRAW);
      glTexCoordPointer(2, GL_FLOAT, 0, 0);
    } else if (m_colorSource == MeshColorSource::Mesh3DTexture) {
      glClientActiveTexture(GL_TEXTURE0);
      glEnableClientState(GL_TEXTURE_COORD_ARRAY);
      glBindBuffer(GL_ARRAY_BUFFER, bufObjects[1]);
      glBufferData(GL_ARRAY_BUFFER, bufferByteSize(texture3DCoords), texture3DCoords.data(), GL_STATIC_DRAW);
      glTexCoordPointer(3, GL_FLOAT, 0, 0);
    }

    glEnableClientState(GL_NORMAL_ARRAY);
    glBindBuffer(GL_ARRAY_BUFFER, bufObjects[2]);
    glBufferData(GL_ARRAY_BUFFER, bufferByteSize(normals), normals.data(), GL_STATIC_DRAW);
    glNormalPointer(GL_FLOAT, 0, 0);

    if (triangleIndexes.empty()) {
      glDrawArrays(type, 0, vertices.size());
    } else {
      glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, bufObjects[3]);
      glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                   triangleIndexes.size() * sizeof(GLuint),
                   triangleIndexes.data(),
                   GL_STATIC_DRAW);
      glDrawElements(type, triangleIndexes.size(), GL_UNSIGNED_INT, 0);
      glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    }

    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glDeleteBuffers(4, bufObjects);
    glDisableClientState(GL_VERTEX_ARRAY);
    if (m_colorSource == MeshColorSource::MeshColor) {
      glDisableClientState(GL_COLOR_ARRAY);
    } else if (m_colorSource == MeshColorSource::Mesh1DTexture) {
      glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    } else if (m_colorSource == MeshColorSource::Mesh2DTexture) {
      glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    } else if (m_colorSource == MeshColorSource::Mesh3DTexture) {
      glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    }
    glDisableClientState(GL_NORMAL_ARRAY);
  }

  glMatrixMode(GL_MODELVIEW);
  glPopMatrix();
}

void Z3DMeshRenderer::renderPickingUsingOpengl()
{
  if (!m_meshPt || m_meshPt->empty()) {
    return;
  }

  if (!m_meshPickingColorReady) {
    prepareMeshPickingColor();
  }

  if (!m_meshPickingColorsPt || m_meshPickingColorsPt->empty() || m_meshPickingColorsPt->size() != m_meshPt->size()) {
    return;
  }

  glMatrixMode(GL_MODELVIEW);
  glPushMatrix();
  // glScalef(getCoordTransform().x, getCoordTransform().y, getCoordTransform().z);
  glMultMatrixf(&coordTransform()[0][0]); // not sure, todo check

  for (size_t i = 0; i < m_meshPt->size(); ++i) {
    const std::vector<glm::vec3>& vertices = (*m_meshPt)[i]->vertices();
    // const std::vector<glm::vec3>& normals = (*m_meshPt)[i]->normals();
    const std::vector<GLuint>& triangleIndexes = (*m_meshPt)[i]->indices();
    GLenum type = (*m_meshPt)[i]->type();

    GLuint bufObjects[3];
    glGenBuffers(3, bufObjects);

    glEnableClientState(GL_VERTEX_ARRAY);
    glBindBuffer(GL_ARRAY_BUFFER, bufObjects[0]);
    glBufferData(GL_ARRAY_BUFFER, bufferByteSize(vertices), vertices.data(), GL_STATIC_DRAW);
    glVertexPointer(3, GL_FLOAT, 0, 0);

    glColor4f((*m_meshPickingColorsPt)[i].r,
              (*m_meshPickingColorsPt)[i].g,
              (*m_meshPickingColorsPt)[i].b,
              (*m_meshPickingColorsPt)[i].a);

    // glEnableClientState(GL_NORMAL_ARRAY);
    // glBindBuffer(GL_ARRAY_BUFFER, bufObjects[1]);
    // glBufferData(GL_ARRAY_BUFFER, normals.size()*3*sizeof(GLfloat), normals.data(), GL_STATIC_DRAW);
    // glNormalPointer(GL_FLOAT, 0, 0);

    if (triangleIndexes.empty()) {
      glDrawArrays(type, 0, vertices.size());
    } else {
      glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, bufObjects[2]);
      glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                   triangleIndexes.size() * sizeof(GLuint),
                   triangleIndexes.data(),
                   GL_STATIC_DRAW);
      glDrawElements(type, triangleIndexes.size(), GL_UNSIGNED_INT, 0);
      glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    }

    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glDeleteBuffers(3, bufObjects);
    glDisableClientState(GL_VERTEX_ARRAY);
    glDisableClientState(GL_NORMAL_ARRAY);
  }

  glMatrixMode(GL_MODELVIEW);
  glPopMatrix();
}
#endif

void Z3DMeshRenderer::render(Z3DEye eye)
{
  ensureMeshSegmentationPlan(m_rendererBase.activeBackend());
  if (!m_meshPt || m_meshPt->empty()) {
    return;
  }

  if (m_colorSource == MeshColorSource::CustomColor && (!m_meshColorReady || m_meshColorsPt == nullptr)) {
    prepareMeshColor();
  }

  for (auto mesh : *m_meshPt) {
    // if (m_colorSource == MeshColorSource::MeshColor && mesh->numColors() < mesh->numVertices())
    // return;
    if (m_colorSource == MeshColorSource::Mesh1DTexture &&
        (mesh->num1DTextureCoordinates() < mesh->numVertices() || !m_texture ||
         m_texture->textureTarget() != GL_TEXTURE_1D)) {
      return;
    }
    if (m_colorSource == MeshColorSource::Mesh2DTexture &&
        (mesh->num2DTextureCoordinates() < mesh->numVertices() || !m_texture ||
         m_texture->textureTarget() != GL_TEXTURE_2D)) {
      return;
    }
    if (m_colorSource == MeshColorSource::Mesh3DTexture &&
        (mesh->num3DTextureCoordinates() < mesh->numVertices() || !m_texture ||
         m_texture->textureTarget() != GL_TEXTURE_3D)) {
      return;
    }
    if (m_colorSource == MeshColorSource::CustomColor &&
        (!m_meshColorsPt || m_meshColorsPt->size() < m_meshPt->size())) {
      return;
    }
    if (mesh->numNormals() != mesh->numVertices()) {
      mesh->generateNormals();
    }
  }

  const bool drawSurface = m_wireframeModeValue != WireframeMode::OnlyWireframe;
  const bool drawWireframe = m_wireframeModeValue != WireframeMode::NoWireframe;

  m_meshShaderGrp->bind();
  Z3DShaderProgram& shader = m_meshShaderGrp->get();
  m_rendererBase.setGlobalShaderParameters(shader, eye);
  setShaderParameters(shader);

  if (m_colorSource == MeshColorSource::Mesh2DTexture || m_colorSource == MeshColorSource::Mesh3DTexture ||
      m_colorSource == MeshColorSource::Mesh1DTexture) {
    shader.bindTexture("texture", m_texture);
  }

  auto attr_vertex = shader.vertexAttributeLocation();
  auto attr_1dTexCoord0 = shader.tex1dCoord0AttributeLocation();
  auto attr_2dTexCoord0 = shader.tex2dCoord0AttributeLocation();
  auto attr_3dTexCoord0 = shader.tex3dCoord0AttributeLocation();
  auto attr_normal = shader.normalAttributeLocation();
  auto attr_color = shader.colorAttributeLocation();

  const bool hasPerMeshTransforms =
    (m_meshPosTransformsPt != nullptr && m_meshPosTransformNormalMatricesPt != nullptr && m_meshPt != nullptr &&
     m_meshPosTransformsPt->size() == m_meshPt->size() &&
     m_meshPosTransformNormalMatricesPt->size() == m_meshPt->size());

  glm::mat4 coordMat(1.0f);
  glm::mat3 viewCoordNormalMat(1.0f);
  if (hasPerMeshTransforms) {
    coordMat = m_followCoordTransform ? coordTransform() : glm::mat4(1.0f);
    if (shader.hasPosTransformNormalMatrixUniform()) {
      const glm::mat4 viewMat = m_rendererBase.viewState().eyes[eye].viewMatrix;
      // Match GL backend: pos_transform_normal_matrix is in eye space (inverse-transpose of view*model).
      viewCoordNormalMat = glm::transpose(glm::inverse(glm::mat3(viewMat * coordMat)));
    }
  }

  auto applyPerMeshTransform = [&](size_t meshIndex) {
    if (!hasPerMeshTransforms) {
      return;
    }
    const glm::mat4 modelMat = coordMat * (*m_meshPosTransformsPt)[meshIndex];
    shader.setPosTransformUniform(modelMat);
    if (shader.hasPosTransformNormalMatrixUniform()) {
      shader.setPosTransformNormalMatrixUniform(viewCoordNormalMat * (*m_meshPosTransformNormalMatricesPt)[meshIndex]);
    }
  };

  if (m_useVAO) {
    if (m_dataChanged) {
      m_VAOs->resize(m_meshPt->size());

      m_VBOs.resize(m_meshPt->size());
      for (auto& vbo : m_VBOs) {
        if (!vbo) {
          vbo = std::make_unique<Z3DVertexBufferObject>(4);
        } else {
          vbo->resize(4);
        }
      }

      for (size_t i = 0; i < m_meshPt->size(); ++i) {
        m_VAOs->bind(i);

        const std::vector<glm::vec3>& vertices = (*m_meshPt)[i]->vertices();
        const std::vector<float>& textureCoordinates1D = (*m_meshPt)[i]->textureCoordinates1D();
        const std::vector<glm::vec2>& textureCoordinates2D = (*m_meshPt)[i]->textureCoordinates2D();
        const std::vector<glm::vec3>& textureCoordinates3D = (*m_meshPt)[i]->textureCoordinates3D();
        const std::vector<glm::vec3>& normals = (*m_meshPt)[i]->normals();
        const std::vector<glm::vec4>& colors = (*m_meshPt)[i]->colors();
        const std::vector<GLuint>& triangleIndexes = (*m_meshPt)[i]->indices();

        size_t bufIdx = 0;
        glEnableVertexAttribArray(attr_vertex);
        m_VBOs[i]->bind(GL_ARRAY_BUFFER, bufIdx++);
        glBufferData(GL_ARRAY_BUFFER, bufferByteSize(vertices), vertices.data(), GL_STATIC_DRAW);
        glVertexAttribPointer(attr_vertex, 3, GL_FLOAT, GL_FALSE, 0, 0);

        if (attr_normal != -1) {
          glEnableVertexAttribArray(attr_normal);
          m_VBOs[i]->bind(GL_ARRAY_BUFFER, bufIdx++);
          glBufferData(GL_ARRAY_BUFFER, bufferByteSize(normals), normals.data(), GL_STATIC_DRAW);
          glVertexAttribPointer(attr_normal, 3, GL_FLOAT, GL_FALSE, 0, 0);
        }

        if (!triangleIndexes.empty()) {
          m_VBOs[i]->bind(GL_ELEMENT_ARRAY_BUFFER, bufIdx++);
          glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                       triangleIndexes.size() * sizeof(GLuint),
                       triangleIndexes.data(),
                       GL_STATIC_DRAW);
        }

        if (m_colorSource == MeshColorSource::Mesh1DTexture && attr_1dTexCoord0 != -1 &&
            !textureCoordinates1D.empty()) {
          glEnableVertexAttribArray(attr_1dTexCoord0);
          m_VBOs[i]->bind(GL_ARRAY_BUFFER, bufIdx++);
          glBufferData(GL_ARRAY_BUFFER,
                       bufferByteSize(textureCoordinates1D),
                       textureCoordinates1D.data(),
                       GL_STATIC_DRAW);
          glVertexAttribPointer(attr_1dTexCoord0, 1, GL_FLOAT, GL_FALSE, 0, 0);
        }

        if (m_colorSource == MeshColorSource::Mesh2DTexture && attr_2dTexCoord0 != -1 &&
            !textureCoordinates2D.empty()) {
          glEnableVertexAttribArray(attr_2dTexCoord0);
          m_VBOs[i]->bind(GL_ARRAY_BUFFER, bufIdx++);
          glBufferData(GL_ARRAY_BUFFER,
                       bufferByteSize(textureCoordinates2D),
                       textureCoordinates2D.data(),
                       GL_STATIC_DRAW);
          glVertexAttribPointer(attr_2dTexCoord0, 2, GL_FLOAT, GL_FALSE, 0, 0);
        }

        if (m_colorSource == MeshColorSource::Mesh3DTexture && attr_3dTexCoord0 != -1 &&
            !textureCoordinates3D.empty()) {
          glEnableVertexAttribArray(attr_3dTexCoord0);
          m_VBOs[i]->bind(GL_ARRAY_BUFFER, bufIdx++);
          glBufferData(GL_ARRAY_BUFFER,
                       bufferByteSize(textureCoordinates3D),
                       textureCoordinates3D.data(),
                       GL_STATIC_DRAW);
          glVertexAttribPointer(attr_3dTexCoord0, 3, GL_FLOAT, GL_FALSE, 0, 0);
        }

        if (m_colorSource == MeshColorSource::MeshColor && attr_color != -1 && colors.size() >= vertices.size()) {
          glEnableVertexAttribArray(attr_color);
          m_VBOs[i]->bind(GL_ARRAY_BUFFER, bufIdx++);
          glBufferData(GL_ARRAY_BUFFER, bufferByteSize(colors), colors.data(), GL_STATIC_DRAW);
          glVertexAttribPointer(attr_color, 4, GL_FLOAT, GL_FALSE, 0, 0);
        }

        glBindBuffer(GL_ARRAY_BUFFER, 0);

        m_VAOs->release();
      }

      m_dataChanged = false;
    }

    if (drawSurface) {
      for (size_t i = 0; i < m_meshPt->size(); ++i) {
        applyPerMeshTransform(i);
        if (m_colorSource == MeshColorSource::CustomColor) {
          shader.setUseCustomColorUniform(true);
          shader.setCustomColorUniform((*m_meshColorsPt)[i]);
        } else if (m_colorSource == MeshColorSource::MeshColor &&
                   (*m_meshPt)[i]->numColors() < (*m_meshPt)[i]->numVertices()) {
          shader.setUseCustomColorUniform(true);
          shader.setCustomColorUniform(glm::vec4(0.f, 0.f, 0.f, 1.f));
        } else {
          shader.setUseCustomColorUniform(false);
        }

        auto type = toGLType((*m_meshPt)[i]->type());
        const std::vector<glm::vec3>& vertices = (*m_meshPt)[i]->vertices();
        const std::vector<GLuint>& triangleIndexes = (*m_meshPt)[i]->indices();
        m_VAOs->bind(i);
        if (triangleIndexes.empty()) {
          glDrawArrays(type, 0, vertices.size());
        } else {
          glDrawElements(type, triangleIndexes.size(), GL_UNSIGNED_INT, 0);
        }
        m_VAOs->release();
      }
    }

    if (drawWireframe) {
      // offset the wireframe
      glEnable(GL_POLYGON_OFFSET_LINE);
      glPolygonOffset(-1, -1);

      // draw the wireframe
      glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
      for (size_t i = 0; i < m_meshPt->size(); ++i) {
        applyPerMeshTransform(i);
        shader.setUseCustomColorUniform(true);
        shader.setCustomColorUniform(m_wireframeColorValue);

        auto type = toGLType((*m_meshPt)[i]->type());
        const std::vector<glm::vec3>& vertices = (*m_meshPt)[i]->vertices();
        const std::vector<GLuint>& triangleIndexes = (*m_meshPt)[i]->indices();
        m_VAOs->bind(i);
        if (triangleIndexes.empty()) {
          glDrawArrays(type, 0, vertices.size());
        } else {
          glDrawElements(type, triangleIndexes.size(), GL_UNSIGNED_INT, 0);
        }
        m_VAOs->release();
      }
      // restore default polygon mode
      glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
      glDisable(GL_POLYGON_OFFSET_LINE);
    }

  } else {
    //    for (size_t i=0; i<m_meshPt->size(); ++i) {
    //      shader.setUniformValue("lighting_enabled", m_needLighting);
    //      if (m_colorSource == MeshColorSource::CustomColor)
    //        renderTriangleList(shader, *((*m_meshPt)[i]), (*m_meshColorsPt)[i]);
    //      else
    //        renderTriangleList(shader, *((*m_meshPt)[i]));
    //    }
    if (m_dataChanged) {
      m_VBOs.resize(m_meshPt->size());
      for (auto& vbo : m_VBOs) {
        if (!vbo) {
          vbo = std::make_unique<Z3DVertexBufferObject>(4);
        } else {
          vbo->resize(4);
        }
      }
    }

    for (size_t i = 0; i < m_meshPt->size(); ++i) {
      applyPerMeshTransform(i);
      if (m_colorSource == MeshColorSource::CustomColor) {
        shader.setUseCustomColorUniform(true);
        shader.setCustomColorUniform((*m_meshColorsPt)[i]);
      } else if (m_colorSource == MeshColorSource::MeshColor &&
                 (*m_meshPt)[i]->numColors() < (*m_meshPt)[i]->numVertices()) {
        shader.setUseCustomColorUniform(true);
        shader.setCustomColorUniform(glm::vec4(0.f, 0.f, 0.f, 1.f));
      } else {
        shader.setUseCustomColorUniform(false);
      }

      const std::vector<glm::vec3>& vertices = (*m_meshPt)[i]->vertices();
      const std::vector<float>& textureCoordinates1D = (*m_meshPt)[i]->textureCoordinates1D();
      const std::vector<glm::vec2>& textureCoordinates2D = (*m_meshPt)[i]->textureCoordinates2D();
      const std::vector<glm::vec3>& textureCoordinates3D = (*m_meshPt)[i]->textureCoordinates3D();
      const std::vector<glm::vec3>& normals = (*m_meshPt)[i]->normals();
      const std::vector<glm::vec4>& colors = (*m_meshPt)[i]->colors();
      const std::vector<GLuint>& triangleIndexes = (*m_meshPt)[i]->indices();
      auto type = toGLType((*m_meshPt)[i]->type());

      size_t bufIdx = 0;
      glEnableVertexAttribArray(attr_vertex);
      m_VBOs[i]->bind(GL_ARRAY_BUFFER, bufIdx++);
      if (m_dataChanged) {
        glBufferData(GL_ARRAY_BUFFER, bufferByteSize(vertices), vertices.data(), GL_STATIC_DRAW);
      }
      glVertexAttribPointer(attr_vertex, 3, GL_FLOAT, GL_FALSE, 0, 0);

      if (attr_normal != -1) {
        glEnableVertexAttribArray(attr_normal);
        m_VBOs[i]->bind(GL_ARRAY_BUFFER, bufIdx++);
        if (m_dataChanged) {
          glBufferData(GL_ARRAY_BUFFER, bufferByteSize(normals), normals.data(), GL_STATIC_DRAW);
        }
        glVertexAttribPointer(attr_normal, 3, GL_FLOAT, GL_FALSE, 0, 0);
      }

      if (!triangleIndexes.empty()) {
        m_VBOs[i]->bind(GL_ELEMENT_ARRAY_BUFFER, bufIdx++);
        if (m_dataChanged) {
          glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                       triangleIndexes.size() * sizeof(GLuint),
                       triangleIndexes.data(),
                       GL_STATIC_DRAW);
        }
      }

      if (m_colorSource == MeshColorSource::Mesh1DTexture && attr_1dTexCoord0 != -1 && !textureCoordinates1D.empty()) {
        glEnableVertexAttribArray(attr_1dTexCoord0);
        m_VBOs[i]->bind(GL_ARRAY_BUFFER, bufIdx++);
        if (m_dataChanged) {
          glBufferData(GL_ARRAY_BUFFER,
                       bufferByteSize(textureCoordinates1D),
                       textureCoordinates1D.data(),
                       GL_STATIC_DRAW);
        }
        glVertexAttribPointer(attr_1dTexCoord0, 1, GL_FLOAT, GL_FALSE, 0, 0);
      }

      if (m_colorSource == MeshColorSource::Mesh2DTexture && attr_2dTexCoord0 != -1 && !textureCoordinates2D.empty()) {
        glEnableVertexAttribArray(attr_2dTexCoord0);
        m_VBOs[i]->bind(GL_ARRAY_BUFFER, bufIdx++);
        if (m_dataChanged) {
          glBufferData(GL_ARRAY_BUFFER,
                       bufferByteSize(textureCoordinates2D),
                       textureCoordinates2D.data(),
                       GL_STATIC_DRAW);
        }
        glVertexAttribPointer(attr_2dTexCoord0, 2, GL_FLOAT, GL_FALSE, 0, 0);
      }

      if (m_colorSource == MeshColorSource::Mesh3DTexture && attr_3dTexCoord0 != -1 && !textureCoordinates3D.empty()) {
        glEnableVertexAttribArray(attr_3dTexCoord0);
        m_VBOs[i]->bind(GL_ARRAY_BUFFER, bufIdx++);
        if (m_dataChanged) {
          glBufferData(GL_ARRAY_BUFFER,
                       bufferByteSize(textureCoordinates3D),
                       textureCoordinates3D.data(),
                       GL_STATIC_DRAW);
        }
        glVertexAttribPointer(attr_3dTexCoord0, 3, GL_FLOAT, GL_FALSE, 0, 0);
      }

      if (m_colorSource == MeshColorSource::MeshColor && attr_color != -1 && colors.size() >= vertices.size()) {
        glEnableVertexAttribArray(attr_color);
        m_VBOs[i]->bind(GL_ARRAY_BUFFER, bufIdx++);
        if (m_dataChanged) {
          glBufferData(GL_ARRAY_BUFFER, bufferByteSize(colors), colors.data(), GL_STATIC_DRAW);
        }
        glVertexAttribPointer(attr_color, 4, GL_FLOAT, GL_FALSE, 0, 0);
      }

      if (drawSurface) {
        if (triangleIndexes.empty()) {
          glDrawArrays(type, 0, vertices.size());
        } else {
          glDrawElements(type, triangleIndexes.size(), GL_UNSIGNED_INT, 0);
          glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
        }
      }
      if (drawWireframe) {
        shader.setUseCustomColorUniform(true);
        shader.setCustomColorUniform(m_wireframeColorValue);

        // offset the wireframe
        glEnable(GL_POLYGON_OFFSET_LINE);
        glPolygonOffset(-1, -1);

        // draw the wireframe
        glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
        if (triangleIndexes.empty()) {
          glDrawArrays(type, 0, vertices.size());
        } else {
          glDrawElements(type, triangleIndexes.size(), GL_UNSIGNED_INT, 0);
          glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
        }
        // restore default polygon mode
        glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
        glDisable(GL_POLYGON_OFFSET_LINE);
      }

      glBindBuffer(GL_ARRAY_BUFFER, 0);

      glDisableVertexAttribArray(attr_vertex);
      if (attr_normal != -1) {
        glDisableVertexAttribArray(attr_normal);
      }
      if (m_colorSource == MeshColorSource::Mesh1DTexture && attr_1dTexCoord0 != -1 && !textureCoordinates1D.empty()) {
        glDisableVertexAttribArray(attr_1dTexCoord0);
      }
      if (m_colorSource == MeshColorSource::Mesh2DTexture && attr_2dTexCoord0 != -1 && !textureCoordinates2D.empty()) {
        glDisableVertexAttribArray(attr_2dTexCoord0);
      }
      if (m_colorSource == MeshColorSource::Mesh3DTexture && attr_3dTexCoord0 != -1 && !textureCoordinates3D.empty()) {
        glDisableVertexAttribArray(attr_3dTexCoord0);
      }
      if (m_colorSource == MeshColorSource::MeshColor && attr_color != -1 && colors.size() >= vertices.size()) {
        glDisableVertexAttribArray(attr_color);
      }
    }

    m_dataChanged = false;
  }

  m_meshShaderGrp->release();
}

void Z3DMeshRenderer::renderPicking(Z3DEye eye)
{
  ensureMeshSegmentationPlan(m_rendererBase.activeBackend());
  if (!m_meshPt || m_meshPt->empty()) {
    return;
  }

  if (!m_meshPickingColorReady || m_meshPickingColorsPt == nullptr) {
    prepareMeshPickingColor();
  }

  if (!m_meshPickingColorsPt || m_meshPickingColorsPt->empty() || m_meshPickingColorsPt->size() != m_meshPt->size()) {
    return;
  }

  m_meshShaderGrp->bind();
  Z3DShaderProgram& shader = m_meshShaderGrp->get();
  m_rendererBase.setGlobalShaderParameters(shader, eye);
  setPickingShaderParameters(shader);
  shader.setUseCustomColorUniform(true);

  auto attr_vertex = shader.vertexAttributeLocation();
  auto attr_normal = shader.normalAttributeLocation();

  const bool hasPerMeshTransforms =
    (m_meshPosTransformsPt != nullptr && m_meshPosTransformNormalMatricesPt != nullptr && m_meshPt != nullptr &&
     m_meshPosTransformsPt->size() == m_meshPt->size() &&
     m_meshPosTransformNormalMatricesPt->size() == m_meshPt->size());

  glm::mat4 coordMat(1.0f);
  glm::mat3 viewCoordNormalMat(1.0f);
  if (hasPerMeshTransforms) {
    coordMat = m_followCoordTransform ? coordTransform() : glm::mat4(1.0f);
    if (shader.hasPosTransformNormalMatrixUniform()) {
      const glm::mat4 viewMat = m_rendererBase.viewState().eyes[eye].viewMatrix;
      viewCoordNormalMat = glm::transpose(glm::inverse(glm::mat3(viewMat * coordMat)));
    }
  }

  auto applyPerMeshTransform = [&](size_t meshIndex) {
    if (!hasPerMeshTransforms) {
      return;
    }
    const glm::mat4 modelMat = coordMat * (*m_meshPosTransformsPt)[meshIndex];
    shader.setPosTransformUniform(modelMat);
    if (shader.hasPosTransformNormalMatrixUniform()) {
      shader.setPosTransformNormalMatrixUniform(viewCoordNormalMat * (*m_meshPosTransformNormalMatricesPt)[meshIndex]);
    }
  };

  if (m_useVAO) {
    if (m_pickingDataChanged) {
      m_pickingVAOs->resize(m_meshPt->size());

      m_pickingVBOs.resize(m_meshPt->size());
      for (auto& pickingVBO : m_pickingVBOs) {
        if (!pickingVBO) {
          pickingVBO = std::make_unique<Z3DVertexBufferObject>(3);
        } else {
          pickingVBO->resize(3);
        }
      }

      for (size_t i = 0; i < m_meshPt->size(); ++i) {
        m_pickingVAOs->bind(i);

        const auto& vertices = (*m_meshPt)[i]->vertices();
        const auto& normals = (*m_meshPt)[i]->normals();
        const auto& triangleIndexes = (*m_meshPt)[i]->indices();

        size_t bufIdx = 0;
        glEnableVertexAttribArray(attr_vertex);
        if (m_dataChanged) {
          m_pickingVBOs[i]->bind(GL_ARRAY_BUFFER, bufIdx++);
          glBufferData(GL_ARRAY_BUFFER, bufferByteSize(vertices), vertices.data(), GL_STATIC_DRAW);
        } else {
          m_VBOs[i]->bind(GL_ARRAY_BUFFER, bufIdx++);
        }
        glVertexAttribPointer(attr_vertex, 3, GL_FLOAT, GL_FALSE, 0, 0);

        if (attr_normal != -1) {
          glEnableVertexAttribArray(attr_normal);
          if (m_dataChanged) {
            m_pickingVBOs[i]->bind(GL_ARRAY_BUFFER, bufIdx++);
            glBufferData(GL_ARRAY_BUFFER, bufferByteSize(normals), normals.data(), GL_STATIC_DRAW);
          } else {
            m_VBOs[i]->bind(GL_ARRAY_BUFFER, bufIdx++);
          }
          glVertexAttribPointer(attr_normal, 3, GL_FLOAT, GL_FALSE, 0, 0);
        }

        if (!triangleIndexes.empty()) {
          if (m_dataChanged) {
            m_pickingVBOs[i]->bind(GL_ELEMENT_ARRAY_BUFFER, bufIdx++);
            glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                         triangleIndexes.size() * sizeof(GLuint),
                         triangleIndexes.data(),
                         GL_STATIC_DRAW);
          } else {
            m_VBOs[i]->bind(GL_ELEMENT_ARRAY_BUFFER, bufIdx++);
          }
        }

        glBindBuffer(GL_ARRAY_BUFFER, 0);

        m_pickingVAOs->release();
      }

      m_pickingDataChanged = false;
    }

    if (m_wireframeModeValue == WireframeMode::OnlyWireframe) {
      // offset the wireframe
      glEnable(GL_POLYGON_OFFSET_LINE);
      glPolygonOffset(-1, -1);

      // draw the wireframe
      glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
    }

    for (size_t i = 0; i < m_meshPt->size(); ++i) {
      applyPerMeshTransform(i);
      shader.setCustomColorUniform((*m_meshPickingColorsPt)[i]);

      auto type = toGLType((*m_meshPt)[i]->type());
      const auto& vertices = (*m_meshPt)[i]->vertices();
      const auto& triangleIndexes = (*m_meshPt)[i]->indices();

      m_pickingVAOs->bind(i);
      if (triangleIndexes.empty()) {
        glDrawArrays(type, 0, vertices.size());
      } else {
        glDrawElements(type, triangleIndexes.size(), GL_UNSIGNED_INT, 0);
      }
      m_pickingVAOs->release();
    }

    if (m_wireframeModeValue == WireframeMode::OnlyWireframe) {
      // restore default polygon mode
      glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
      glDisable(GL_POLYGON_OFFSET_LINE);
    }

  } else {
    // for (size_t i=0; i<m_meshPt->size(); ++i) {
    // renderTriangleList(shader, *((*m_meshPt)[i]), (*m_meshPickingColorsPt)[i]);
    // }
    if (m_pickingDataChanged) {
      m_pickingVBOs.resize(m_meshPt->size());
      for (auto& pickingVBO : m_pickingVBOs) {
        if (!pickingVBO) {
          pickingVBO = std::make_unique<Z3DVertexBufferObject>(3);
        } else {
          pickingVBO->resize(3);
        }
      }
    }

    if (m_wireframeModeValue == WireframeMode::OnlyWireframe) {
      // offset the wireframe
      glEnable(GL_POLYGON_OFFSET_LINE);
      glPolygonOffset(-1, -1);

      // draw the wireframe
      glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
    }

    for (size_t i = 0; i < m_meshPt->size(); ++i) {
      applyPerMeshTransform(i);
      shader.setCustomColorUniform((*m_meshPickingColorsPt)[i]);

      const auto& vertices = (*m_meshPt)[i]->vertices();
      const auto& normals = (*m_meshPt)[i]->normals();
      const auto& triangleIndexes = (*m_meshPt)[i]->indices();
      auto type = toGLType((*m_meshPt)[i]->type());

      size_t bufIdx = 0;
      glEnableVertexAttribArray(attr_vertex);
      if (m_dataChanged) {
        m_pickingVBOs[i]->bind(GL_ARRAY_BUFFER, bufIdx++);
        if (m_pickingDataChanged) {
          glBufferData(GL_ARRAY_BUFFER, bufferByteSize(vertices), vertices.data(), GL_STATIC_DRAW);
        }
      } else {
        m_VBOs[i]->bind(GL_ARRAY_BUFFER, bufIdx++);
      }
      glVertexAttribPointer(attr_vertex, 3, GL_FLOAT, GL_FALSE, 0, 0);

      if (attr_normal != -1) {
        glEnableVertexAttribArray(attr_normal);
        if (m_dataChanged) {
          m_pickingVBOs[i]->bind(GL_ARRAY_BUFFER, bufIdx++);
          if (m_pickingDataChanged) {
            glBufferData(GL_ARRAY_BUFFER, bufferByteSize(normals), normals.data(), GL_STATIC_DRAW);
          }
        } else {
          m_VBOs[i]->bind(GL_ARRAY_BUFFER, bufIdx++);
        }
        glVertexAttribPointer(attr_normal, 3, GL_FLOAT, GL_FALSE, 0, 0);
      }

      if (!triangleIndexes.empty()) {
        if (m_dataChanged) {
          m_pickingVBOs[i]->bind(GL_ELEMENT_ARRAY_BUFFER, bufIdx++);
          if (m_pickingDataChanged) {
            glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                         triangleIndexes.size() * sizeof(GLuint),
                         triangleIndexes.data(),
                         GL_STATIC_DRAW);
          }
        } else {
          m_VBOs[i]->bind(GL_ELEMENT_ARRAY_BUFFER, bufIdx++);
        }
        glDrawElements(type, triangleIndexes.size(), GL_UNSIGNED_INT, 0);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
      } else {
        glDrawArrays(type, 0, vertices.size());
      }

      glBindBuffer(GL_ARRAY_BUFFER, 0);

      glDisableVertexAttribArray(attr_vertex);
      if (attr_normal != -1) {
        glDisableVertexAttribArray(attr_normal);
      }
    }

    if (m_wireframeModeValue == WireframeMode::OnlyWireframe) {
      // restore default polygon mode
      glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
      glDisable(GL_POLYGON_OFFSET_LINE);
    }

    m_pickingDataChanged = false;
  }

  m_meshShaderGrp->release();
}

void Z3DMeshRenderer::enqueueRenderBatches(Z3DEye eye, RenderBackend backend, bool picking)
{
  if (backend != RenderBackend::Vulkan) {
    return;
  }

  ensureMeshSegmentationPlan(backend);
  if (!m_meshPt || m_meshPt->empty()) {
    return;
  }

  if (picking) {
    if (!m_meshPickingColorReady || m_meshPickingColorsPt == nullptr) {
      prepareMeshPickingColor();
    }
    if (!m_meshPickingColorsPt || m_meshPickingColorsPt->empty() || m_meshPickingColorsPt->size() != m_meshPt->size()) {
      return;
    }
  }

  if (m_colorSource == MeshColorSource::CustomColor && (!m_meshColorReady || m_meshColorsPt == nullptr)) {
    prepareMeshColor();
  }

  for (auto mesh : *m_meshPt) {
    if (m_colorSource == MeshColorSource::Mesh1DTexture &&
        (mesh->num1DTextureCoordinates() < mesh->numVertices() || !m_texture ||
         m_texture->textureTarget() != GL_TEXTURE_1D)) {
      return;
    }
    if (m_colorSource == MeshColorSource::Mesh2DTexture &&
        (mesh->num2DTextureCoordinates() < mesh->numVertices() || !m_texture ||
         m_texture->textureTarget() != GL_TEXTURE_2D)) {
      return;
    }
    if (m_colorSource == MeshColorSource::Mesh3DTexture &&
        (mesh->num3DTextureCoordinates() < mesh->numVertices() || !m_texture ||
         m_texture->textureTarget() != GL_TEXTURE_3D)) {
      return;
    }
    if (m_colorSource == MeshColorSource::CustomColor &&
        (!m_meshColorsPt || m_meshColorsPt->size() < m_meshPt->size())) {
      return;
    }
    if (mesh->numNormals() != mesh->numVertices()) {
      mesh->generateNormals();
    }
  }

  for (const MeshBatchSegment& segment : m_segmentationPlan.batchSegments) {
    auto batch = buildRenderBatch(eye, picking, segment.meshStart, segment.meshCount, segment.streamSegmentOrdinal);
    m_rendererBase.appendBatch(std::move(batch));
  }
}

void Z3DMeshRenderer::createResources(RenderBackend backend)
{
  if (backend != RenderBackend::OpenGL) {
    return;
  }
  m_meshShaderGrp = std::make_unique<Z3DShaderGroup>(m_rendererBase);

  QStringList allshaders;
  allshaders << "mesh.vert"
             << "mesh_func.frag"
             << "lighting2.frag";
  QStringList normalShaders;
  normalShaders << "mesh.vert"
                << "mesh.frag"
                << "lighting2.frag";
  const std::string header = m_rendererBase.generateHeader() + generateHeader();
  m_meshShaderGrp->init(allshaders, header, "", normalShaders);
  m_meshShaderGrp->addAllSupportedPostShaders();

  m_VAOs = std::make_unique<Z3DVertexArrayObject>(1);
  m_pickingVAOs = std::make_unique<Z3DVertexArrayObject>(1);
  m_VBOs.clear();
  m_pickingVBOs.clear();

  m_dataChanged = true;
  m_pickingDataChanged = true;
}

void Z3DMeshRenderer::destroyResources()
{
  m_meshShaderGrp.reset();
  m_VAOs.reset();
  m_pickingVAOs.reset();
  m_VBOs.clear();
  m_pickingVBOs.clear();
}

} // namespace nim
