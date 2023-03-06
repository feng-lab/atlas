#include "z3dscene.h"

#include "z3dgl.h"

namespace nim {

Z3DScene::Z3DScene(int width, int height, bool stereo, QObject* parent)
  : QGraphicsScene(0, 0, width, height, parent)
  , m_isStereoScene(stereo)
{}

void Z3DScene::drawBackground(QPainter* /*painter*/, const QRectF& /*rect*/)
{
  // QPainter set glclearcolor to white, we set it back
  glClearColor(0.f, 0.f, 0.f, 0.f);
  glFinish();
}

} // namespace nim
