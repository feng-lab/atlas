#pragma once

#include <QGraphicsScene>
#include <memory>

namespace nim {

class Z3DCanvas;

class Z3DCanvasPainter;

class Z3DRenderingEngine;

class Z3DScene : public QGraphicsScene
{
  Q_OBJECT

public:
  explicit Z3DScene(int width, int height, bool stereo, Z3DCanvas& canvas);

  void drawBackground(QPainter* painter, const QRectF& rect) override;

  void initPainter();

  void setRenderingEngine(Z3DRenderingEngine* engine);

private:
  bool m_isStereoScene;
  Z3DCanvas& m_canvas;

  std::shared_ptr<Z3DCanvasPainter> m_painter;
};

} // namespace nim
