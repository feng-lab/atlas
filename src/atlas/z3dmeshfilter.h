#pragma once

#include "z3dgeometryfilter.h"
#include "zoptionparameter.h"
#include "zwidgetsgroup.h"
#include "znumericparameter.h"
#include "z3dmeshrenderer.h"
#include "zeventlistenerparameter.h"
#include "zneuroglancerexternalsource.h"
#include "zneuroglancerprecomputedmesh.h"
#include "zjson.h"
#include <QObject>
#include <QPoint>
#include <QTimer>
#include <folly/CancellationToken.h>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <unordered_map>
#include <vector>

namespace nim {

struct RegionNode;

class Z3DMeshFilter : public Z3DGeometryFilter
{
  Q_OBJECT

public:
  explicit Z3DMeshFilter(Z3DGlobalParameters& globalParas,
                         const RegionNode* regionNode = nullptr,
                         QObject* parent = nullptr);
  ~Z3DMeshFilter() override;

  void setMeshColor(const glm::vec4& col)
  {
    m_singleColorForAllMesh.set(col);
  }

  [[nodiscard]] glm::vec4 meshColor() const
  {
    return m_singleColorForAllMesh.get();
  }

  [[nodiscard]] QString regionName() const;

  [[nodiscard]] bool isFixed() const
  {
    return m_meshList[0]->numVertices() == 96957;
  }

  void setGlow(bool v)
  {
    m_glow.set(v);
  }

  double process(Z3DEye eye) override;
  void setProgressiveRenderingMode(bool v) override;

  void setData(std::vector<ZMesh*>* meshList);
  void setExternalSourceJson(json::value sourceJson);
  void setExternalSourceState(json::value sourceJson, std::shared_ptr<const ZNeuroglancerRemoteContext> remoteContext);
  void setViewport(glm::uvec2 viewport) override;
  void setViewport(glm::uvec4 viewport) override;
  void beginExportMeshLod(const glm::uvec2& fullViewport);
  void endExportMeshLod();

  void setSelectedMeshes(std::set<ZMesh*>* list)
  {
    m_selectedMeshes = list;
  }

  [[nodiscard]] bool isReady(Z3DEye eye) const override;

  std::shared_ptr<ZWidgetsGroup> widgetsGroup();

  std::shared_ptr<ZWidgetsGroup> widgetsGroupForAnnotationFilter();

  [[nodiscard]] bool hasOpaque(Z3DEye eye) const override
  {
    return Z3DGeometryFilter::hasOpaque(eye) && !m_glow.get();
  }

  void renderOpaque(Z3DEye eye) override;

  [[nodiscard]] bool hasTransparent(Z3DEye eye) const override
  {
    return Z3DGeometryFilter::hasTransparent(eye) || m_glow.get();
  }

  void renderTransparent(Z3DEye eye) override;

Q_SIGNALS:
  void meshSelected(ZMesh*, bool append);

protected:
  void prepareColor();

  void adjustWidgets();

  void updateWireframeMode();
  void updateWireframeColor();

  void selectMesh(QMouseEvent* e, int w, int h);

  void onApplyTransform();

  void renderPicking(Z3DEye eye) override;

  void updateSize(const glm::uvec2& targetSize) override;

  void prepareData();

  void registerPickingObjects() override;

  void deregisterPickingObjects() override;

  ZBBox<glm::dvec3> meshBound(ZMesh* p);

  // void updateAxisAlignedBoundBoxImpl() override;
  void updateNotTransformedBoundBoxImpl() override;

  void updateMeshVisibleState();

private:
  void resetRuntimeNeuroglancerLodState();

  void markRuntimeNeuroglancerLodDirty();

  void onRuntimeNeuroglancerCameraChanged();

  void onRuntimeNeuroglancerIdleTimeout();

  void startRuntimeNeuroglancerOpen();

  void requestRuntimeNeuroglancerRows(const std::vector<uint32_t>& rows);

  void applyRuntimeNeuroglancerSelection();

  void logRuntimeNeuroglancerRefinementStarted(size_t remainingDesiredRows);

  void logRuntimeNeuroglancerRefinementFinished(const char* reason);

  [[nodiscard]] bool hasRuntimeNeuroglancerLod() const;

  [[nodiscard]] bool isSameRuntimeNeuroglancerSource(const ZNeuroglancerMeshExternalSourceKey& key) const;

  // get visible data from m_origMeshList put into m_meshList
  void getVisibleData();

private:
  Z3DMeshRenderer m_triangleListRenderer;
  ZStringIntOptionParameter m_wireframeMode;
  ZVec4Parameter m_wireframeColor;

  ZStringIntOptionParameter m_colorMode;
  ZVec4Parameter m_singleColorForAllMesh;
  ZBoolParameter m_glow;
  // Glow parameters now live on the filter for compositor to read
  ZStringIntOptionParameter m_glowMode;
  ZIntParameter m_glowBlurRadius;
  ZFloatParameter m_glowBlurScale;
  ZFloatParameter m_glowBlurStrength;

  // std::map<QString, size_t> m_sourceColorMapper;   // should use unordered_map
  //  mesh list used for rendering, it is a subset of m_origMeshList. Some mesh are
  //  hidden because they are unchecked from the object model. This allows us to control
  //  the visibility of each single punctum.
  std::vector<ZMesh*> m_meshList;
  std::vector<ZMesh*> m_registeredMeshList; // used for picking

  std::vector<glm::vec4> m_meshColors;
  std::vector<glm::vec4> m_meshPickingColors;

  ZEventListenerParameter m_selectMeshEvent;
  glm::ivec2 m_startCoord{};
  ZMesh* m_pressedMesh;
  std::set<ZMesh*>* m_selectedMeshes = nullptr; // point to all selected meshes, managed by other class

  // generate and save to speed up bound box rendering for big mesh
  std::map<ZMesh*, ZBBox<glm::dvec3>> m_meshBoundboxMapper;

  std::shared_ptr<ZWidgetsGroup> m_widgetsGroup;
  bool m_dataIsInvalid;

  std::vector<ZMesh*> m_origMeshList;

  json::value m_externalSourceJson;
  std::shared_ptr<const ZNeuroglancerRemoteContext> m_runtimeNgRemoteContext;
  std::optional<ZNeuroglancerMeshExternalSourceKey> m_runtimeNgSourceKey;
  std::shared_ptr<const ZNeuroglancerPrecomputedMeshSource> m_runtimeNgSource;
  std::shared_ptr<const ZNeuroglancerPrecomputedMeshSource::MultiLodManifest> m_runtimeNgManifest;
  std::vector<uint32_t> m_runtimeNgBaseRows;
  std::unordered_map<uint32_t, std::shared_ptr<const ZNeuroglancerPrecomputedMeshSource::MultiLodChunkMesh>>
    m_runtimeNgLoadedRows;
  std::set<uint32_t> m_runtimeNgRowsInFlight;
  std::unordered_map<uint32_t, uint64_t> m_runtimeNgRowBytesInFlight;
  uint64_t m_runtimeNgBytesInFlight = 0;
  std::shared_ptr<folly::CancellationSource> m_runtimeNgCancellationSource;
  std::unordered_map<uint32_t, std::shared_ptr<folly::CancellationSource>> m_runtimeNgRowCancellationSources;
  bool m_runtimeNgCancelObsoleteInFlight = false;
  std::set<uint32_t> m_runtimeNgFailedRows;
  std::vector<ZMesh*> m_runtimeNgVisibleMeshes;
  std::vector<ZMesh*> m_runtimeNgFrozenVisibleMeshes;
  QTimer m_runtimeNgIdleTimer;
  uint64_t m_runtimeNgEpoch = 0;
  bool m_runtimeNgBaseReady = false;
  bool m_runtimeNgInteractionActive = false;
  bool m_runtimeNgSelectionDirty = false;
  bool m_runtimeNgRefinementActive = false;
  bool m_runtimeNgProgressiveRendering = true;
  bool m_runtimeNgExportActive = false;

  const RegionNode* m_regionNode = nullptr;
};

} // namespace nim
