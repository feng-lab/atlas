#ifndef Z3DVIEW_H
#define Z3DVIEW_H

#include <QObject>
#include <QAction>
#include "zviewsettinginterface.h"
#include <QDir>
#include <QJsonObject>
#include "z3dglobalparameters.h"

class QMainWindow;

namespace nim {

class Z3DCanvas;
class ZDoc;
class Z3DObjView;
class Z3DCompositor;
class Z3DCanvasPainter;
class Z3DNetworkEvaluator;
class Z3DMainWindow;

class Z3DView : public QObject, public ZViewSettingInterface
{
  Q_OBJECT
public:
  Z3DView(ZDoc *doc, bool stereo, Z3DMainWindow *parent = nullptr);
  ~Z3DView();

  inline QAction* zoomInAction() { return m_zoomInAction; }
  inline QAction* zoomOutAction() { return m_zoomOutAction; }
  inline QAction* resetCameraAction() { return m_resetCameraAction; }

  ZWidgetsGroup* viewSettingWidgetsGroupOf(size_t id);

  Z3DCameraParameter& camera() { return m_globalParas.camera; }
  const Z3DCameraParameter& camera() const { return m_globalParas.camera; }
  Z3DTrackballInteractionHandler& interactionHandler() { return m_globalParas.interactionHandler; }
  inline Z3DCanvas& canvas() { return *m_canvas; }
  inline Z3DCanvasPainter& canvasPainter() { return *m_canvasPainter; }
  inline Z3DCompositor& compositor() { return *m_compositor; }
  inline Z3DNetworkEvaluator& networkEvaluator() { return *m_networkEvaluator; }
  inline Z3DGlobalParameters& globalParas() { return m_globalParas; }

  QWidget* globalParasWidget();
  QWidget* captureWidget();
  QWidget* backgroundWidget();
  QWidget* axisWidget();

  void updateBoundBox();

  void read(size_t id, const QJsonObject &json);
  void write(size_t id, QJsonObject &json) const;
  void read(const QJsonObject &json);
  void write(QJsonObject &json) const;

signals:
  void objViewReady(size_t id);

private slots:
  void zoomIn();
  void zoomOut();
  void resetCamera();  // set up camera based on visible objects in scene, original position
  void resetCameraClippingRange(); // Reset the camera clipping range to include this entire bounding box

  void takeScreenShot(QString filename, int width, int height, Z3DScreenShotType sst);
  void takeScreenShot(QString filename, Z3DScreenShotType sst);
  void takeSeriesScreenShot(const QDir& dir, const QString &namePrefix, glm::vec3 axis,
                            bool clockWise, int numFrame, int width, int height,
                            Z3DScreenShotType sst);
  void takeSeriesScreenShot(const QDir& dir, const QString &namePrefix, glm::vec3 axis,
                            bool clockWise, int numFrame, Z3DScreenShotType sst);

private:
  void init();
  void createActions();

private:
  ZDoc *m_doc;
  bool m_isStereoView;
  QMainWindow *m_mainWin;

  //
  QAction *m_zoomInAction;
  QAction *m_zoomOutAction;
  QAction *m_resetCameraAction;

  QList<Z3DObjView*> m_3dObjViews;

  Z3DNetworkEvaluator *m_networkEvaluator;
  Z3DGlobalParameters m_globalParas;
  Z3DCanvas *m_canvas;
  Z3DCanvasPainter *m_canvasPainter;
  Z3DCompositor *m_compositor;

  std::vector<double> m_boundBox;
  size_t m_numObjsBefore;

  bool m_lock;
};

} // namespace nim

#endif // Z3DVIEW_H
