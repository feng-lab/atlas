#pragma once

#include "z3dglobalparameters.h"
#include "z3dcontext.h"
#include "zviewsettinginterface.h"
#include "zbbox.h"
#include <QDir>
#include <QObject>
#include <QEvent>
#include <QMutexLocker>
#include <QPointer>
#include <boost/unordered/unordered_flat_set.hpp>

class QOffscreenSurface;

namespace nim {

class Z3DCanvas;

class ZDoc;

class Z3DObjView;

class Z3DCompositor;

class Z3DNetworkEvaluator;

class Z3DCanvasEventListener;

class ZAnimation;

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

  Z3DCompositor& compositor()
  {
    return *m_compositor;
  }

  Z3DNetworkEvaluator& networkEvaluator()
  {
    return *m_networkEvaluator;
  }

  Z3DGlobalParameters& globalParas()
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

  void takeFixedSizeScreenShot(const QString& filename,
                               int width,
                               int height,
                               Z3DScreenShotType sst = Z3DScreenShotType::MonoView);

  void takeScreenShot(const QString& filename, Z3DScreenShotType sst);

  void exportFixedSize3DAnimation(const ZAnimation* animation,
                                  const QString& fn,
                                  int framePerSecond,
                                  int startFrame,
                                  int endFrame,
                                  int width,
                                  int height,
                                  bool overwriteFileIfExist = true,
                                  Z3DScreenShotType sst = Z3DScreenShotType::MonoView,
                                  std::atomic_bool* cancelFlag = nullptr,
                                  const QString* imageOuputFolder = nullptr,
                                  bool skipVideoCompression = false,
                                  int tileSize = 0,
                                  int tileBorder = 0);

  const std::vector<std::unique_ptr<Z3DObjView>>& objViews() const
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

  // for offscreen rendering
  void init();

  void initAndAttachToCanvas(Z3DCanvas* canvas);

  void detachCanvas();

  glm::uvec2 outputSize() const;

  void setOutputSize(const glm::uvec2& size);

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
    std::erase(m_listeners, &e);
  }

  void clearEventListeners()
  {
    m_listeners.clear();
  }

  void renderFast(bool stereo = false);

  void render(bool stereo = false);

  Z3DRenderTarget* monoReadyTarget() const;

  Z3DRenderTarget* leftReadyTarget() const;

  Z3DRenderTarget* rightReadyTarget() const;

  Z3DLocalColorBuffer* monoReadyLocalBuffer() const;

  Z3DLocalColorBuffer* leftReadyLocalBuffer() const;

  Z3DLocalColorBuffer* rightReadyLocalBuffer() const;

  std::mutex& targetSwitchMutex()
  {
    return m_globalParas->targetSwitchMutex;
  }

  bool hasNewRenderingFlag() const
  {
    return m_globalParas->hasNewRendering.load();
  }

  void clearNewRenderingFlag()
  {
    m_globalParas->hasNewRendering = false;
  }

  void cancelLongRendering()
  {
    if (m_globalParas->cancellationSource && !m_globalParas->cancellationSource->isCancellationRequested()) {
      // LOG(INFO) << "request cancel";
      m_globalParas->cancellationSource->requestCancellation();
    }
  }

  void reportRenderingError(const QString& error) const
  {
    Q_EMIT renderingError(error);
  }

  void reportRenderingError(const std::string& error) const
  {
    Q_EMIT renderingError(QString::fromStdString(error));
  }

  void reportRenderingError(const char* error) const
  {
    Q_EMIT renderingError(QString(error));
  }

  void reportCancelError() const
  {
    Q_EMIT renderingError("cancelled");
  }

Q_SIGNALS:
  void objViewReady(size_t id);

  void sceneParaUpdated();

  void renderingFinished();

  void renderingError(const QString& error) const;

  void progressChanged(int v);

  void videoEncoderFinished();

  void initialized();

protected:
  bool event(QEvent* e) override;

  void getGLFocus();

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

  // private version will throw exception on error
  void takeFixedSizeScreenShotWithoutResetCanvasSizePrivate(const QString& filename,
                                                            int width,
                                                            int height,
                                                            Z3DScreenShotType sst);

  void takeFixedSizeScreenShotWithoutResetCanvasSizeByTilePrivate(const QString& filename,
                                                                  const QString& rightFilename,
                                                                  int width,
                                                                  int height,
                                                                  Z3DScreenShotType sst,
                                                                  int tileSize = 0,
                                                                  int tileBorder = 0,
                                                                  int tileStartX = 0,
                                                                  int tileStartY = 0);

  // private version will throw exception on error
  void takeScreenShotPrivate(const QString& filename, Z3DScreenShotType sst);

  void resetOutputSizeToMatchCanvasSize();

private:
  std::unique_ptr<QOffscreenSurface> m_offscreenSurface;
  std::unique_ptr<Z3DContext> m_context;
  ZDoc& m_doc;

  std::vector<std::unique_ptr<Z3DObjView>> m_3dObjViews;

  QPointer<Z3DCanvas> m_canvas;
  std::unique_ptr<Z3DGlobalParameters> m_globalParas;
  std::unique_ptr<Z3DNetworkEvaluator> m_networkEvaluator;
  std::unique_ptr<Z3DCompositor> m_compositor;

  ZBBox<glm::dvec3> m_boundBox;
  size_t m_numObjsBefore;

  std::deque<Z3DCanvasEventListener*> m_listeners;

  boost::unordered_flat_set<QEvent::Type> m_eventTypes;

  bool m_isRendering = false;

  QMutex m_mutex;

  double m_progress = 0;
};

} // namespace nim
