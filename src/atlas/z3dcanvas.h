#pragma once

#include "zglmutils.h"
#include <QGraphicsView>
#include <QGraphicsScene>
#include <QGraphicsPixmapItem>
#include <QInputEvent>
#include <QShortcut>
#include <QPoint>
#include <QPointer>

#include <cstdint>
#include <memory>
#include <optional>

#ifdef ATLAS_USE_OPENGLWIDGET
class QOpenGLContext;
#endif

namespace nim {

class Z3DRenderingEngine;
class ZDoc;
class ZSwcPack;
class ZSwcTypeDialog;

#ifdef ATLAS_USE_OPENGLWIDGET
class ZOpenGLWidget;
class Z3DScene;
#endif

class Z3DCanvas : public QGraphicsView
{
  Q_OBJECT

public:
  Z3DCanvas(const QString& title,
            int width,
            int height,
            QWidget* parent = nullptr,
            Qt::WindowFlags f = Qt::WindowFlags());

#ifdef ATLAS_USE_OPENGLWIDGET
  ~Z3DCanvas() override;

  QOpenGLContext* context() const;

  // Set the opengl context of this canvas as the current one.
  void getGLFocus();
#endif

  void setRenderingEngine(Z3DRenderingEngine* engine);

  void setDoc(ZDoc* doc)
  {
    m_doc = doc;
  }

  void showSeedTraceContextMenu(QPoint globalPos, size_t imgObjId, size_t sc, float x, float y, float z);

  void pointInVolumeLeftClicked(QPoint globalPos,
                                size_t imgObjId,
                                size_t sc,
                                float x,
                                float y,
                                float z,
                                Qt::KeyboardModifiers modifiers);

  void showSwcNodeContextMenu(QPoint globalPos, ZSwcPack* swcPack, int64_t clickedNodeId);

  void request3dSwcAddNeuronNode(ZSwcPack* swcPack, double x, double y, double z, double r);
  void request3dSwcPlainExtend(ZSwcPack* swcPack, double x, double y, double z, double r);
  void request3dSwcConnectToTarget(ZSwcPack* swcPack, int64_t targetNodeId);

  void on3dObjectsMoved(double x, double y, double z);

  void toggleFullScreen();

  void sceneParaUpdated();

  void renderingFinished();

  // for high dpi support like retina
  glm::uvec2 physicalSize()
  {
    return glm::uvec2(width() * devicePixelRatio(), height() * devicePixelRatio());
  }

  glm::uvec2 logicalSize()
  {
    return glm::uvec2(width(), height());
  }

Q_SIGNALS:
  // w and h is physical size not logical size, opengl works in physical pixel
  void canvasSizeChanged(size_t w, size_t h);

#if defined(ATLAS_USE_OPENGLWIDGET)
  void openGLContextInitialized();
#endif

  void rotateX();

  void rotateY();

  void rotateZ();

  void rotateXM();

  void rotateYM();

  void rotateZM();

protected:
  void contextMenuEvent(QContextMenuEvent* e) override;

  //  void enterEvent(QEnterEvent* e) override;
  //
  //  void leaveEvent(QEvent* e) override;

  void mousePressEvent(QMouseEvent* e) override;

  void mouseReleaseEvent(QMouseEvent* e) override;

  void mouseMoveEvent(QMouseEvent* e) override;

  void mouseDoubleClickEvent(QMouseEvent* e) override;

  void wheelEvent(QWheelEvent* e) override;

  void timerEvent(QTimerEvent* e) override;

  void keyPressEvent(QKeyEvent* e) override;

  void keyReleaseEvent(QKeyEvent* e) override;

  void resizeEvent(QResizeEvent* e) override;

  void dragEnterEvent(QDragEnterEvent* e) override;

  void dropEvent(QDropEvent* e) override;

  //  void setCursor(const QCursor& c)
  //  { viewport()->setCursor(c); }

private:
  // double devicePixelRatio();

  void ensure3dSwcNodeActions();

  void setActive3dSwcPackForEditing(ZSwcPack* swcPack, int64_t clickedNodeId);

  void update3dSwcNodeActionEnabledState();

  void toggle3dSwcExtendMode(bool on);
  void start3dSwcConnectToMode();
  void toggle3dSwcMoveSelectedMode(bool on);
  void locate3dSwcNodesIn2D();
  void change3dSwcNodeType();
  void toggle3dAddNeuronNodeMode(bool on);

private:
  bool m_fullscreen = false;

#ifdef ATLAS_USE_OPENGLWIDGET
  ZOpenGLWidget* m_glWidget = nullptr;
  std::unique_ptr<Z3DScene> m_3dScene;
#else
  std::unique_ptr<QGraphicsScene> m_scene;
  QGraphicsPixmapItem* m_pixmapItem = nullptr;
#endif

  QShortcut* m_rotateXShortCut = nullptr;
  QShortcut* m_rotateYShortCut = nullptr;
  QShortcut* m_rotateZShortCut = nullptr;
  QShortcut* m_rotateXMShortCut = nullptr;
  QShortcut* m_rotateYMShortCut = nullptr;
  QShortcut* m_rotateZMShortCut = nullptr;

  ZDoc* m_doc = nullptr;
  Z3DRenderingEngine* m_engine = nullptr;

  // 3D SWC-node context menu (UI thread ownership).
  QPointer<ZSwcPack> m_active3dSwcPack;
  int64_t m_active3dClickedNodeId = -1;

  QAction* m_toggle3dExtendAction = nullptr;
  QAction* m_connectTo3dSwcNodeAction = nullptr;
  QAction* m_toggle3dMoveSelectedAction = nullptr;
  QAction* m_locate3dNodesIn2DAction = nullptr;
  QAction* m_change3dSwcNodeTypeAction = nullptr;
  QAction* m_toggle3dAddNeuronNodeAction = nullptr;

  bool m_connectTo3dSwcModeActive = false;
  std::optional<size_t> m_connectTo3dSwcObjId;

  // For 3D move-selected: cache inverse linear transform (rotation * scale) so we can
  // convert world-space deltas from the interaction handler into SWC-local deltas.
  std::optional<glm::dmat3> m_active3dSwcLinearInv;
};

} // namespace nim
