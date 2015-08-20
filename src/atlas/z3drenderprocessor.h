#ifndef Z3DRENDERPROCESSOR_H
#define Z3DRENDERPROCESSOR_H

#include <vector>

#include "z3dboundedfilter.h"
#include "z3drenderport.h"
#include "z3drendererbase.h"
#include "zvertexarrayobject.h"

namespace nim {

// The base class for all processor classes that render to Z3DRenderOutputPort or screen
class Z3DRenderProcessor : public Z3DBoundedFilter
{
  Q_OBJECT
public:
  Z3DRenderProcessor(Z3DGlobalParameters &globalParas, QObject *parent = nullptr);

  const std::vector<Z3DRenderOutputPort*>& privateRenderPorts() const;

  static void saveTextureAsImage(const Z3DTexture &tex, const QString &filename);
  static void saveDepthTextureAsImage(const Z3DTexture &tex, const QString &filename);

signals:
  // emit this only if resize starts from current processor.
  void requestUpstreamSizeChange(Z3DRenderProcessor*);

public slots:
  // 1. for each outport, get all expected size from all connected inports, and use the maximum one
  //    as the new size of the outport
  // 2. update private port size
  // 3. Once we get the newsize of all outports, we calculate a expected size for each inport and set it.
  //    default choice for inport expected size is the maximum new outport size
  // reimplement this if you want different behavior
  virtual void updateSize();

protected:
  void addPrivateRenderPort(Z3DRenderOutputPort& port);
  void renderScreenQuad(const Z3DShaderProgram &shader);

protected:
  bool m_hardwareSupportVAO;

private:
  std::vector<Z3DRenderOutputPort*> m_privateRenderPorts;

  ZVertexArrayObject m_privateVAO;
};

} // namespace nim

#endif // Z3DRENDERPROCESSOR_H
