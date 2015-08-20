#ifndef Z3DMESHFILTER_H
#define Z3DMESHFILTER_H

#include <QObject>
#include "z3dgeometryfilter.h"
#include "zoptionparameter.h"
#include <map>
#include <QString>
#include <QPoint>
#include <vector>
#include "zwidgetsgroup.h"
#include "znumericparameter.h"
#include "z3dmeshrenderer.h"
#include "zeventlistenerparameter.h"

namespace nim {

class Z3DMeshFilter : public Z3DGeometryFilter
{
  Q_OBJECT
public:
  explicit Z3DMeshFilter(Z3DGlobalParameters& globalParas, QObject *parent = nullptr);
  virtual ~Z3DMeshFilter();

  void setVisible(bool v) { m_visible.set(v); }
  bool isVisible() const { return m_visible.get(); }
  void setMeshColor(glm::vec4 col) { m_singleColorForAllMesh.set(col); }

  virtual void process(Z3DEye) override;

  void setData(std::vector<ZMesh*> *meshList);
  void setData(QList<ZMesh*> *meshList);
  void setSelectedMeshes(std::set<ZMesh*> *list) { m_selectedMeshes = list; }

  virtual bool isReady(Z3DEye eye) const override;

  ZWidgetsGroup *widgetsGroup();
  ZWidgetsGroup *widgetsGroupForAnnotationFilter();

  virtual void renderOpaque(Z3DEye eye) override;
  virtual void renderTransparent(Z3DEye eye) override;

signals:
  void meshSelected(ZMesh*, bool append);

protected slots:
  void prepareColor();
  void adjustWidgets();
  void selectMesh(QMouseEvent *e, int w, int h);

protected:
  virtual void renderPicking(Z3DEye eye) override;

  void prepareData();

  virtual void registerPickingObjects() override;
  virtual void deregisterPickingObjects() override;

  std::vector<double> meshBound(ZMesh* p);
  //virtual void updateAxisAlignedBoundBoxImpl() override;
  virtual void updateNotTransformedBoundBoxImpl() override;

private:

public slots:
  void updateMeshVisibleState();

private:
  // get visible data from m_origMeshList put into m_meshList
  void getVisibleData();

  Z3DMeshRenderer m_triangleListRenderer;

  ZBoolParameter m_visible;
  ZStringIntOptionParameter m_colorMode;
  ZVec4Parameter m_singleColorForAllMesh;

  //std::map<QString, size_t> m_sourceColorMapper;   // should use unordered_map
  // mesh list used for rendering, it is a subset of m_origMeshList. Some mesh are
  // hidden because they are unchecked from the object model. This allows us to control
  // the visibility of each single punctum.
  std::vector<ZMesh*> m_meshList;
  std::vector<ZMesh*> m_registeredMeshList;    // used for picking

  std::vector<glm::vec4> m_meshColors;
  std::vector<glm::vec4> m_meshPickingColors;

  ZEventListenerParameter m_selectMeshEvent;
  glm::ivec2 m_startCoord;
  ZMesh *m_pressedMesh;
  std::set<ZMesh*> *m_selectedMeshes;   //point to all selected meshes, managed by other class

  // generate and save to speed up bound box rendering for big mesh
  std::map<ZMesh*, std::vector<double>> m_meshBoundboxMapper;

  ZWidgetsGroup *m_widgetsGroup;
  bool m_dataIsInvalid;

  std::vector<ZMesh*> m_origMeshList;
};

} // namespace nim

#endif // Z3DMESHFILTER_H
