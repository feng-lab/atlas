#pragma once

#include <QGraphicsScene>

namespace nim {

class Z3DScene : public QGraphicsScene
{
  Q_OBJECT

public:
  explicit Z3DScene(int width, int height, bool stereo, QObject* parent = nullptr);

  void drawBackground(QPainter* painter, const QRectF& rect) override;

private:
  bool m_isStereoScene;
};

} // namespace nim
