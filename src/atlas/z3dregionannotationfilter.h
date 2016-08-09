#pragma once


#include "z3dmeshfilter.h"
#include "zregionannotation.h"

namespace nim {

class Z3DRegionAnnotationFilter : public Z3DGeometryFilter
{
Q_OBJECT
public:
  explicit Z3DRegionAnnotationFilter(Z3DGlobalParameters& globalParas, QObject* parent = nullptr);

  virtual ~Z3DRegionAnnotationFilter();

  void setVisible(bool v)
  { m_visible.set(v); }

  virtual void process(Z3DEye eye) override;

  void setData(ZRegionAnnotation& regAnno);

  virtual bool isReady(Z3DEye eye) const override;

  std::shared_ptr<ZWidgetsGroup> widgetsGroup();

  virtual void renderOpaque(Z3DEye eye) override;

  virtual void renderTransparent(Z3DEye eye) override;

  virtual bool hasOpaque(Z3DEye) const override
  { return true; }

  virtual bool hasTransparent(Z3DEye) const override
  { return true; }

  virtual void setViewport(glm::uvec2 viewport) override;

  virtual void setViewport(glm::uvec4 viewport) override;

  virtual void setShaderHookType(Z3DRendererBase::ShaderHookType t) override;

  virtual void setShaderHookParaDDPDepthBlenderTexture(const Z3DTexture* t) override;

  virtual void setShaderHookParaDDPFrontBlenderTexture(const Z3DTexture* t) override;

protected:
  void visibleChanged(bool v);

  void allMeshChanged();

  virtual void renderPicking(Z3DEye eye) override;

  virtual void registerPickingObjects() override;

  virtual void deregisterPickingObjects() override;

  //virtual void updateAxisAlignedBoundBoxImpl() override;
  virtual void updateNotTransformedBoundBoxImpl() override;

private:
  ZBoolParameter m_visible;

  std::shared_ptr<ZWidgetsGroup> m_widgetsGroup;
  bool m_dataIsInvalid;

  ZRegionAnnotation* m_regionAnnotation;

  std::map<int, std::unique_ptr<Z3DMeshFilter>> m_idToMeshFilters;
  std::map<int, QString> m_idToRegionNames;
  std::map<QString, int> m_nameToID;

  size_t m_numParas;
};

} // namespace nim

