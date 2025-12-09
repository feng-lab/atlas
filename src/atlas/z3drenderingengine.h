#pragma once

#include "z3dglobalparameters.h"
#include "z3dcontext.h"
#include "z3drenderglobalstate.h"
#include "zviewsettinginterface.h"
#include "zbbox.h"
#include "zbenchtimer.h"
#include <QDir>
#include <QObject>
#include <QEvent>
#include <QPointer>
#include <boost/unordered/unordered_flat_set.hpp>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class QOffscreenSurface;

namespace folly {
class CancellationToken;
}

namespace nim {

class Z3DFilter;

class Z3DCanvas;

class ZDoc;

class Z3DObjView;

class Z3DCompositor;

class Z3DCanvasEventListener;

class ZAnimation;

class Z3DScratchResourcePool;
class ZVulkanContext;
class ZVulkanDevice;

// Vulkan compositor forward decl removed (classification phase)

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
    m_listeners.insert(m_listeners.begin(), &e);
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

  [[nodiscard]] Z3DLocalColorBuffer* monoReadyLocalBuffer() const;

  [[nodiscard]] Z3DLocalColorBuffer* leftReadyLocalBuffer() const;

  [[nodiscard]] Z3DLocalColorBuffer* rightReadyLocalBuffer() const;

  std::mutex& targetSwitchMutex()
  {
    return m_globalParas->targetSwitchMutex;
  }

  [[nodiscard]] bool hasNewRenderingFlag() const
  {
    return m_globalParas->hasNewRendering.load();
  }

  void clearNewRenderingFlag()
  {
    m_globalParas->hasNewRendering = false;
  }

  // Debug helpers to directly save compositor outputs (bypass local buffer)
  void saveCurrentFrameColor(const QString& filename, Z3DEye eye = MonoEye);
  void saveCurrentFrameDepth(const QString& filename, Z3DEye eye = MonoEye);

  void cancelLongRendering()
  {
    if (Z3DRenderGlobalState::instance().hasCancellationSource()) {
      // VLOG(1) << "request cancel";
      Z3DRenderGlobalState::instance().requestCancellation();
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

  // Switch Vulkan physical device at runtime by sorted index. Recreates the
  // logical device and updates dependent components (scratch pool, caps).
  // Returns true on success. Call at a safe point (no in-flight rendering);
  // the engine will wait idle on the old device before switching.
  bool switchVulkanDeviceIndex(int index);

  void reportCancelError() const
  {
    Q_EMIT renderingError("cancelled");
  }

  // Rebuild the filter pipeline used for rendering based on the current
  // object views and compositor connections.
  void updatePipeline();

  // Scene apply helpers (run on engine thread)
  void beginScene3DApply();

  void applyView3DGeneral(const json::object& json);

  void applyView3DForId(size_t id, json::object json);

  // Return parameter list for a view-setting group without exposing the group itself
  std::vector<ZParameter*> parametersOfViewSetting(size_t id);

Q_SIGNALS:
  void objViewReady(size_t id);

  void sceneParaUpdated();

  void renderingFinished();

  void renderingError(const QString& error) const;

  void progressChanged(int v);

  void videoEncoderFinished();

  void initialized();

  void backendChanged();

  // Emitted when all queued scene 3D apply operations finish
  void scene3DApplyFinished();

  // Emitted when a view setting widgets group changes (any id)
  void viewSettingWidgetsGroupChanged(size_t id);

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

  ZImg localColorBufferToRGBAImg(const Z3DLocalColorBuffer& buffer);

  void onCanvasResized(size_t w, size_t h);

  void initGL();
  void ensureGLContext();
  // Vulkan init deferred

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

  void handleRenderBackendChanged();
  void applyBackendSwitch();

  // Execute one pass over the current filter pipeline. If cancellationToken
  // is provided, this will check for cancellation between filters.
  double processFrame(bool stereo,
                      bool progressiveRendering,
                      const folly::CancellationToken* cancellationToken = nullptr);

  // Update sizes for all filters in the pipeline, walking from sinks back
  // towards sources, using the engine's output size as the global target.
  void updateAllFilterSizes();

private:
  struct ScratchPoolDeleter
  {
    void operator()(Z3DScratchResourcePool* pool) const;
  };

  // Track widget groups we've already connected to avoid duplicate connects. Must outlive compositor.
  std::unordered_set<const ZWidgetsGroup*> m_observedWGs;

  // Set true during destruction to ignore late events and postings
  std::atomic_bool m_shuttingDown = false;

  // gl surface/context
  std::unique_ptr<QOffscreenSurface> m_offscreenSurface;
  std::unique_ptr<Z3DContext> m_context;

  // Vulkan context/device owned at engine level (mirrors GL ownership)
  std::unique_ptr<ZVulkanContext> m_vkContext;
  std::unique_ptr<ZVulkanDevice> m_vkDevice;

  ZDoc& m_doc;

  QPointer<Z3DCanvas> m_canvas;
  std::unique_ptr<Z3DGlobalParameters> m_globalParas;
  std::unique_ptr<Z3DScratchResourcePool, ScratchPoolDeleter> m_scratchPool;
  std::unique_ptr<Z3DCompositor> m_compositor;
  std::vector<std::unique_ptr<Z3DObjView>> m_3dObjViews;
  // Vulkan compositor bridge deferred

  // Linearized filter execution order: all object filters, then compositor.
  std::vector<Z3DFilter*> m_pipeline;

  // Global render target size for the 3D pipeline (canvas/screenshot size).
  glm::uvec2 m_outputSize{32u, 32u};

  struct FilterWrapper
  {
    virtual ~FilterWrapper() = default;
    virtual void beforeFilterProcess(const Z3DFilter*) {}
    virtual void afterFilterProcess(const Z3DFilter*) {}
    virtual void beforeNetworkProcess() {}
    virtual void afterNetworkProcess() {}
  };

  struct CheckOpenGLStateFilterWrapper : FilterWrapper
  {
    void afterFilterProcess(const Z3DFilter* p) override;
    void beforeNetworkProcess() override;

  private:
    void checkState(const Z3DFilter* p);
    static void warn(const Z3DFilter* p, const char* message);
  };

  struct ProfileFilterWrapper : FilterWrapper
  {
    ZBenchTimer m_benchTimer{"Network"};

    void afterFilterProcess(const Z3DFilter* p) override;
    void beforeNetworkProcess() override;
    void afterNetworkProcess() override;
  };

  std::vector<std::unique_ptr<FilterWrapper>> m_filterWrappers;

  ZBBox<glm::dvec3> m_boundBox;
  size_t m_numObjsBefore;

  std::vector<Z3DCanvasEventListener*> m_listeners;

  boost::unordered_flat_set<QEvent::Type> m_eventTypes;

  bool m_isRendering = false;

  std::mutex m_mutex;

  double m_progress = 0;

  // Backend switch deferred
  bool m_backendSwitchScheduled = false;

  // Pending per-object View3D json waiting for objViewReady
  std::unordered_map<size_t, json::object> m_pendingObjViewJson;
  int m_sceneApplyOutstanding = 0;

  // (m_observedWGs and m_shuttingDown declared above to ensure lifetime beyond compositor)


};

} // namespace nim
