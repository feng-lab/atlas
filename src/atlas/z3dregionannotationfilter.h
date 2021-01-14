#pragma once

#include "z3dmeshfilter.h"
#include "zregionannotationpack.h"
#include "zregionannotationviewsettingtreemodel.h"

namespace nim {

class Z3DRegionAnnotationFilter : public Z3DGeometryFilter
{
Q_OBJECT
public:
  explicit Z3DRegionAnnotationFilter(Z3DGlobalParameters& globalParas, QObject* parent = nullptr);

  void process(Z3DEye eye) override;

  void setData(ZRegionAnnotationPack& rap);

  bool isReady(Z3DEye eye) const override;

  std::shared_ptr<ZWidgetsGroup> widgetsGroup();

  void renderOpaque(Z3DEye eye) override;

  void renderTransparent(Z3DEye eye) override;

  bool hasOpaque(Z3DEye /*unused*/) const override
  { return true; }

  bool hasTransparent(Z3DEye /*unused*/) const override
  { return true; }

  void setViewport(glm::uvec2 viewport) override;

  void setViewport(glm::uvec4 viewport) override;

  void setShaderHookType(Z3DRendererBase::ShaderHookType t) override;

  void setShaderHookParaDDPDepthBlenderTexture(const Z3DTexture* t) override;

  void setShaderHookParaDDPFrontBlenderTexture(const Z3DTexture* t) override;

protected:
  void visibleChanged(bool v);

  void allMeshChanged();

  void renderPicking(Z3DEye eye) override;

  void registerPickingObjects() override;

  void deregisterPickingObjects() override;

  //void updateAxisAlignedBoundBoxImpl() override;
  void updateNotTransformedBoundBoxImpl() override;

  void showAllRegions();

  void hideAllRegions();

private:
  std::shared_ptr<ZWidgetsGroup> m_widgetsGroup;
  bool m_dataIsInvalid = true;

  ZRegionAnnotationPack* m_regionAnnotationPack = nullptr;

  std::map<int64_t, std::unique_ptr<Z3DMeshFilter>> m_idToMeshFilters;
  std::map<int64_t, QString> m_idToRegionNames;
  std::map<QString, int64_t> m_nameToID;

  size_t m_numParas;

  std::unique_ptr<ZRegionAnnotationViewSettingTreeModel> m_viewSettingTreeModel;
  QWidget* m_viewSettingTreeWidget = nullptr;
};

} // namespace nim

