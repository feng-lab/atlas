#pragma once

#include "z3dglobalparameters.h"
#include "zviewsettinginterface.h"
#include "zbbox.h"
#include <QDir>
#include <QObject>
#include <QAction>
#include <QMutexLocker>

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
  Z3DView(ZDoc& doc, bool stereo, Z3DMainWindow* parent = nullptr);

  ~Z3DView() override;

  [[nodiscard]] const ZDoc& doc() const;

  inline QAction* zoomInAction()
  { return m_zoomInAction; }

  inline QAction* zoomOutAction()
  { return m_zoomOutAction; }

  inline QAction* resetCameraAction()
  { return m_resetCameraAction; }

  std::shared_ptr<ZWidgetsGroup> viewSettingWidgetsGroupOf(size_t id) override;

  Z3DCameraParameter& camera()
  { return m_globalParas->camera; }

  [[nodiscard]] const Z3DCameraParameter& camera() const
  { return m_globalParas->camera; }

  Z3DTrackballInteractionHandler& interactionHandler()
  { return m_globalParas->interactionHandler; }

  inline Z3DCanvas& canvas()
  { return *m_canvas; }

  [[nodiscard]] inline const Z3DCanvas& canvas() const
  { return *m_canvas; }

  inline Z3DCanvasPainter& canvasPainter()
  { return *m_canvasPainter; }

  inline Z3DCompositor& compositor()
  { return *m_compositor; }

  inline Z3DNetworkEvaluator& networkEvaluator()
  { return *m_networkEvaluator; }

  inline Z3DGlobalParameters& globalParas()
  { return *m_globalParas; }

  QWidget* globalParasWidget();

  [[nodiscard]] QWidget* captureWidget() const;

  QWidget* backgroundWidget();

  QWidget* axisWidget();

  static QWidget* helpWidget();

  void updateBoundBox();

  void read(size_t id, const json::object& json);

  void write(size_t id, json::object& json) const;

  void read(const json::object& json);

  void write(json::object& json) const;

  bool takeFixedSizeScreenShot(const QString& filename, int width, int height, Z3DScreenShotType sst);

  bool takeScreenShot(const QString& filename, Z3DScreenShotType sst);

  std::vector<Z3DObjView*> objViews()
  { return m_3dObjViews; }

  [[nodiscard]] ZBBox<glm::dvec3> boundBoxOfObjs(const std::vector<size_t>& ids) const;

  [[nodiscard]] ZBBox<glm::dvec3> boundBoxOfObjsAfterClipping(const std::vector<size_t>& ids) const;

  void cameraFocusesOn(double x, double y, double z, double radius = 64)
  { m_globalParas->cameraFocusesOn(x, y, z, radius); }

  void cameraFocusesOn(const ZBBox<glm::dvec3>& bound, double minRadius = 64)
  { m_globalParas->cameraFocusesOn(bound, minRadius); }

  void cameraPointsTo(double x, double y, double z)
  { m_globalParas->cameraPointsTo(x, y, z); }

  void cameraPointsTo(const ZBBox<glm::dvec3>& bound)
  { m_globalParas->cameraPointsTo(bound); }

  void flipView(); //Look from the oppsite side

  void setXZView();

  void setYZView();

signals:

  void objViewReady(size_t id);

  void networkConstructed();

private:
  void zoomIn();

  void zoomOut();

  void resetCamera();  // set up camera based on visible objects in scene, original position
  void resetCameraCenter();

  void resetCameraClippingRange(); // Reset the camera clipping range to include this entire bounding box

  bool takeFixedSizeSeriesScreenShot(const QDir& dir, const QString& namePrefix, const glm::vec3& axis,
                                     bool clockWise, int numFrame, int width, int height,
                                     Z3DScreenShotType sst);

  bool takeSeriesScreenShot(const QDir& dir, const QString& namePrefix, const glm::vec3& axis,
                            bool clockWise, int numFrame, Z3DScreenShotType sst);

  void init();

  void createActions();

private:
  ZDoc& m_doc;
  bool m_isStereoView;
  QMainWindow* m_mainWin = nullptr;

  //
  QAction* m_zoomInAction = nullptr;
  QAction* m_zoomOutAction = nullptr;
  QAction* m_resetCameraAction = nullptr;

  std::vector<Z3DObjView*> m_3dObjViews;

  Z3DCanvas* m_canvas = nullptr;
  std::unique_ptr<Z3DGlobalParameters> m_globalParas;
  std::unique_ptr<Z3DNetworkEvaluator> m_networkEvaluator;
  std::unique_ptr<Z3DCanvasPainter> m_canvasPainter;
  std::unique_ptr<Z3DCompositor> m_compositor;

  ZBBox<glm::dvec3> m_boundBox;
  size_t m_numObjsBefore;

  QMutex m_mutex;
};

} // namespace nim

