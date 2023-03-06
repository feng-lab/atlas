#pragma once

#include "z3dglobalparameters.h"
#include "z3dcontext.h"
#include "zviewsettinginterface.h"
#include "zbbox.h"
#include <QDir>
#include <QObject>
#include <QEvent>
#include <QMutexLocker>
#include <set>

class QMainWindow;

namespace nim {

class Z3DCanvas;

class ZDoc;

class Z3DObjView;

class Z3DCompositor;

class Z3DNetworkEvaluator;

class Z3DCanvasEventListener;

class Z3DRenderingEngine
  : public QObject
  , public ZViewSettingInterface
{
  Q_OBJECT

public:
  explicit Z3DRenderingEngine(ZDoc& doc, QObject* parent = nullptr);

  ~Z3DRenderingEngine() override;

  [[nodiscard]] const ZDoc& doc() const;

  std::shared_ptr<ZWidgetsGroup> viewSettingWidgetsGroupOf(size_t id) override;

  Z3DCameraParameter& camera()
  {
    return m_globalParas->camera;
  }

  [[nodiscard]] const Z3DCameraParameter& camera() const
  {
    return m_globalParas->camera;
  }

  Z3DTrackballInteractionHandler& interactionHandler()
  {
    return m_globalParas->interactionHandler;
  }

  inline Z3DCanvas* canvas()
  {
    return m_canvas;
  }

  inline Z3DCompositor& compositor()
  {
    return *m_compositor;
  }

  inline Z3DNetworkEvaluator& networkEvaluator()
  {
    return *m_networkEvaluator;
  }

  inline Z3DGlobalParameters& globalParas()
  {
    return *m_globalParas;
  }

  QWidget* globalParasWidget();

  QWidget* backgroundWidget();

  QWidget* axisWidget();

  void updateBoundBox();

  void read(size_t id, const json::object& json);

  void write(size_t id, json::object& json) const;

  void read(const json::object& json);

  void write(json::object& json) const;

  void takeFixedSizeScreenShot(const QString& filename, int width, int height, Z3DScreenShotType sst);

  void
  takeFixedSizeScreenShotWithoutResetCanvasSize(const QString& filename, int width, int height, Z3DScreenShotType sst);

  void resetCanvasSize();

  void takeScreenShot(const QString& filename, Z3DScreenShotType sst);

  std::vector<Z3DObjView*> objViews()
  {
    return m_3dObjViews;
  }

  [[nodiscard]] ZBBox<glm::dvec3> boundBoxOfObjs(const std::vector<size_t>& ids) const;

  [[nodiscard]] ZBBox<glm::dvec3> boundBoxOfObjsAfterClipping(const std::vector<size_t>& ids) const;

  void cameraFocusesOn(double x, double y, double z, double radius = 64)
  {
    m_globalParas->cameraFocusesOn(x, y, z, radius);
  }

  void cameraFocusesOn(const ZBBox<glm::dvec3>& bound, double minRadius = 64)
  {
    m_globalParas->cameraFocusesOn(bound, minRadius);
  }

  void cameraPointsTo(double x, double y, double z)
  {
    m_globalParas->cameraPointsTo(x, y, z);
  }

  void cameraPointsTo(const ZBBox<glm::dvec3>& bound)
  {
    m_globalParas->cameraPointsTo(bound);
  }

  void flipView(); // Look from the oppsite side

  void setXZView();

  void setYZView();

  void init();

  void attachToCanvas(Z3DCanvas* canvas);

  void setOutputSize(const glm::uvec2& size);

  void makeOutputSizeEvenNumbers();

  void zoomIn();

  void zoomOut();

  void resetCamera(); // set up camera based on visible objects in scene, original position

  void addEventListenerToBack(Z3DCanvasEventListener& e)
  {
    m_listeners.push_back(&e);
  }

  void addEventListenerToFront(Z3DCanvasEventListener& e)
  {
    m_listeners.push_front(&e);
  }

  void removeEventListener(Z3DCanvasEventListener& e)
  {
    erase(m_listeners, &e);
  }

  void clearEventListeners()
  {
    m_listeners.clear();
  }

Q_SIGNALS:

  void objViewReady(size_t id);

  void networkConstructed();

protected:
  bool event(QEvent* e) override;

private:
  void resetCameraCenter();

  void resetCameraClippingRange(); // Reset the camera clipping range to include this entire bounding box

  //  bool takeFixedSizeSeriesScreenShot(const QDir& dir,
  //                                     const QString& namePrefix,
  //                                     const glm::vec3& axis,
  //                                     bool clockWise,
  //                                     int numFrame,
  //                                     int width,
  //                                     int height,
  //                                     Z3DScreenShotType sst);
  //
  //  bool takeSeriesScreenShot(const QDir& dir,
  //                            const QString& namePrefix,
  //                            const glm::vec3& axis,
  //                            bool clockWise,
  //                            int numFrame,
  //                            Z3DScreenShotType sst);

  static ZImg textureToRGBAImg(const Z3DTexture& tex);

  void onCanvasResized(size_t w, size_t h);

  void initGL();

  void rotateX();

  void rotateY();

  void rotateZ();

  void rotateXM();

  void rotateYM();

  void rotateZM();

private:
  std::unique_ptr<Z3DContext> m_context;
  ZDoc& m_doc;

  std::vector<Z3DObjView*> m_3dObjViews;

  Z3DCanvas* m_canvas = nullptr;
  std::unique_ptr<Z3DGlobalParameters> m_globalParas;
  std::unique_ptr<Z3DNetworkEvaluator> m_networkEvaluator;
  std::unique_ptr<Z3DCompositor> m_compositor;

  ZBBox<glm::dvec3> m_boundBox;
  size_t m_numObjsBefore;

  QMutex m_mutex;

  std::deque<Z3DCanvasEventListener*> m_listeners;
  bool m_inited = false;

  std::set<QEvent::Type> m_eventTypes;
};

} // namespace nim
