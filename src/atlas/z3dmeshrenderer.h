#pragma once

#include "z3dprimitiverenderer.h"
#include "z3drendercommands.h"
#include "zmesh.h"
#include <QString>
#include <memory>
#include <string>

namespace nim {

// NOTE: Color of each vertex can comes from many sources, call setColorSource
// "MeshColor" (default) : colors field of ZMesh is used.
// "Mesh1DTexture" : 1DTextureCoord field of ZMesh is used, setTexture must be called
// "Mesh2DTexture" : 2DTextureCoord field of ZMesh is used, setTexture must be called
// "Mesh3DTexture" : 3DTextureCoord field of ZMesh is used, setTexture must be called
// "CustomColor" : setDataColors must be called to set color of each mesh
enum class MeshColorSource
{
  MeshColor,
  Mesh1DTexture,
  Mesh2DTexture,
  Mesh3DTexture,
  CustomColor
};

class Z3DMeshRenderer : public Z3DPrimitiveRenderer
{
public:
  explicit Z3DMeshRenderer(Z3DRendererBase& rendererBase);

  void setData(std::vector<ZMesh*>* meshInput);

  // if set, this color will used instead colors from ZMesh (if any)
  // the number should match the number of meshes
  void setDataColors(std::vector<glm::vec4>* meshColorsInput);

  // set texture, texture coordinates of each vertex should exist in
  // ZMesh
  void setTexture(Z3DTexture* tex);

  // Optional: provide a backend-native sampled image handle for Vulkan.
  // When set, Vulkan path binds this handle instead of bridging from GL.
  void setTextureHandle(const SampledImageHandle& handle)
  {
    // Caller manages the lifecycle of the underlying backend resource.
    m_textureHandle = handle;
  }

  void setDataPickingColors(std::vector<glm::vec4>* meshPickingColorsInput = nullptr);

  // Optional: provide an additional per-mesh transform that should be applied to vertices
  // before the renderer's global coordTransform.
  //
  // This is primarily used by runtime Neuroglancer mesh LOD to avoid CPU vertex transforms:
  // chunk meshes keep vertices in stored quantized coordinates, and the renderer applies a
  // per-chunk affine matrix at draw time.
  //
  // If set, both vectors must either be null, or have the same size as the mesh list passed to setData().
  // normalMatrices should contain the inverse-transpose of the linear part of the corresponding transform.
  void setPerMeshPosTransforms(std::vector<glm::mat4>* meshPosTransformsInput,
                               std::vector<glm::mat3>* meshPosTransformNormalMatricesInput);

  // One of "MeshColor", "Mesh1DTexture", "Mesh2DTexture", "Mesh3DTexture", "CustomColor"
  void setColorSource(MeshColorSource source);

  enum class WireframeMode
  {
    NoWireframe,
    WithWireframe,
    OnlyWireframe
  };

  void setWireframeMode(WireframeMode mode)
  {
    m_wireframeModeValue = mode;
  }

  void setWireframeColor(const glm::vec4& color)
  {
    m_wireframeColorValue = color;
  }

  // Command list integration helper used by backend implementations.

protected:
  void compile() override;

  [[nodiscard]] std::string generateHeader();

#if !defined(ATLAS_USE_CORE_PROFILE) && defined(ATLAS_SUPPORT_FIXED_PIPELINE)
  void renderUsingOpengl() override;
  void renderPickingUsingOpengl() override;
#endif

  void render(Z3DEye eye) override;

  void renderPicking(Z3DEye eye) override;

  void enqueueRenderBatches(Z3DEye eye, RenderBackend backend, bool picking) override;

private:
  struct MeshBatchSegment
  {
    size_t meshStart = 0;
    size_t meshCount = 0;
    uint32_t streamSegmentOrdinal = 0;
  };

  struct MeshSegmentationPlan
  {
    bool valid = false;
    RenderBackend backend = RenderBackend::OpenGL;
    size_t maxMonolithicGeometryStreamBytes = std::numeric_limits<size_t>::max();
    MeshColorSource colorSource = MeshColorSource::MeshColor;
    bool anyMeshSplit = false;
    std::vector<ZMesh> ownedSplitMeshes;
    std::vector<ZMesh*> activeMeshPtrs;
    std::vector<size_t> activeMeshSourceIndices;
    std::vector<MeshBatchSegment> batchSegments;
  };

  void invalidateMeshSegmentationPlan();
  void ensureMeshSegmentationPlan(RenderBackend backend);
  void prepareMeshTransforms();
  [[nodiscard]] size_t maxMonolithicGeometryStreamBytesForBackend(RenderBackend backend) const;
  [[nodiscard]] size_t textureStreamBytesForVertexCount(size_t vertexCount) const;
  [[nodiscard]] bool rangeFitsMonolithicStreamLimit(size_t vertexCount, size_t indexCount, size_t maxStreamBytes) const;

  // Material/picking color helpers populate the lazily-evaluated span pointers
  // used by MeshPayload. They never allocate unless splitting requires it.
  void prepareMeshColor();

  void prepareMeshPickingColor();

  // MeshPayload is a zero-copy view of the renderer's CPU-side buffers. Callers
  // must ensure the corresponding prepare* helpers ran before dereferencing the
  // spans they expose.
  [[nodiscard]] MeshPayload buildMeshPayload() const;
  [[nodiscard]] RenderBatch buildRenderBatch(Z3DEye eye, bool picking) const;
  [[nodiscard]] MeshPayload buildMeshPayload(size_t meshStart, size_t meshCount, uint32_t streamSegmentOrdinal) const;
  [[nodiscard]] RenderBatch
  buildRenderBatch(Z3DEye eye, bool picking, size_t meshStart, size_t meshCount, uint32_t streamSegmentOrdinal) const;

protected:
  void createResources(RenderBackend backend) override;

  void destroyResources() override;

  std::unique_ptr<Z3DShaderGroup> m_meshShaderGrp;

  std::vector<ZMesh*>* m_meshPt;
  std::vector<glm::vec4>* m_meshColorsPt;
  std::vector<glm::vec4>* m_meshPickingColorsPt;
  std::vector<ZMesh*>* m_origMeshPt;
  std::vector<glm::vec4>* m_origMeshColorsPt;
  std::vector<glm::vec4>* m_origMeshPickingColorsPt;
  std::vector<glm::mat4>* m_origMeshPosTransformsPt = nullptr;
  std::vector<glm::mat3>* m_origMeshPosTransformNormalMatricesPt = nullptr;

  Z3DTexture* m_texture;
  SampledImageHandle m_textureHandle{};

private:
  std::vector<glm::vec4> m_splitMeshesColors;
  std::vector<glm::vec4> m_splitMeshesPickingColors;
  std::vector<glm::mat4> m_splitMeshesPosTransforms;
  std::vector<glm::mat3> m_splitMeshesPosTransformNormalMatrices;
  MeshSegmentationPlan m_segmentationPlan;
  bool m_meshColorReady;
  bool m_meshPickingColorReady;

  MeshColorSource m_colorSource;

  bool m_dataChanged;
  bool m_pickingDataChanged;
  // one VAO for each mesh
  std::unique_ptr<Z3DVertexArrayObject> m_VAOs;
  std::unique_ptr<Z3DVertexArrayObject> m_pickingVAOs;
  std::vector<std::unique_ptr<Z3DVertexBufferObject>> m_VBOs;
  std::vector<std::unique_ptr<Z3DVertexBufferObject>> m_pickingVBOs;

  // Optional per-mesh transforms (post-split). When non-null, sizes must match m_meshPt.
  std::vector<glm::mat4>* m_meshPosTransformsPt = nullptr;
  std::vector<glm::mat3>* m_meshPosTransformNormalMatricesPt = nullptr;

  WireframeMode m_wireframeModeValue = WireframeMode::NoWireframe;
  glm::vec4 m_wireframeColorValue{1.f};

  // Per-stream generation counters for Vulkan selective restaging
  uint32_t m_posGen = 0;
  uint32_t m_normGen = 0;
  uint32_t m_colorGen = 0;
  uint32_t m_texGen = 0;
  uint32_t m_indexGen = 0;
};

} // namespace nim
