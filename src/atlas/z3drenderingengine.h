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
#include <QPoint>
#include <QPointer>
#include <boost/unordered/unordered_flat_set.hpp>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class QOffscreenSurface;
class QTimer;
class QString;

namespace folly {
class CancellationToken;
}

namespace nim {

class Z3DFilter;
class Z3DMeshFilter;
class Z3DCanvas;

class ZDoc;

class Z3DObjView;

class Z3DCompositor;

class Z3DCanvasEventListener;

class ZAnimation;

class Z3DScratchResourcePool;
class ZVulkanContext;
class ZVulkanDevice;
class ZQtExecutor;
class ZSwcPack;
struct ScreenSpaceSufficiencyAudit;

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

  // Benchmark-only helper: export the scalar MIP field before transfer-function mapping
  // for a rendered image object.
  [[nodiscard]] bool saveRawMIPImageForObject(size_t id, const QString& path, std::string& error);
  [[nodiscard]] bool
  screenSpaceSufficiencyAuditForObject(size_t id, ScreenSpaceSufficiencyAudit& audit, std::string& error);

  void exportFixedSize3DAnimation(const ZAnimation* animation,
                                  const QString& fn,
                                  int framePerSecond,
                                  int startFrame,
                                  int endFrame,
                                  int width,
                                  int height,
                                  bool overwriteFileIfExist = true,
                                  Z3DScreenShotType sst = Z3DScreenShotType::MonoView,
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

  // Seed-trace UI state is owned by ZDoc/Trace Settings on the UI thread. The
  // rendering engine keeps a small snapshot so Z3DImgFilter can decide whether
  // to emit trace-menu requests without reading UI-thread state directly.
  void setSeedTraceUiState(bool enabled, bool inProgress, std::optional<size_t> sourceImgObjId);
  void setSeedTraceUiState(bool enabled, bool inProgress, std::optional<size_t> sourceImgObjId, size_t sourceChannel);

  [[nodiscard]] bool seedTraceToolEnabled() const
  {
    return m_seedTraceToolEnabled;
  }

  [[nodiscard]] bool seedTraceInProgress() const
  {
    return m_seedTraceInProgress;
  }

  [[nodiscard]] std::optional<size_t> seedTraceSourceImgObjId() const
  {
    return m_seedTraceSourceImgObjId;
  }

  [[nodiscard]] size_t seedTraceSourceChannel() const
  {
    return m_seedTraceSourceChannel;
  }

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

  void cancelActiveRender();
  void cancelCapture();
  void cancelLongRendering();

  // Teardown helper: drain Vulkan frame-executor fences and execute any
  // fence-gated completion callbacks. This is intended to be invoked on the
  // rendering thread prior to shutting it down (e.g., when closing a 3D window),
  // so coroutines that await fence completion can finish deterministically.
  void drainVulkanFrameExecutorForTeardown();

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

  void appendDeferredRenderingError(const QString& error);

  // Render-thread executor: a folly::Executor that schedules work onto the
  // engine thread via QMetaObject::invokeMethod. Intended for Vulkan/GL
  // continuations that must run on the rendering thread.
  [[nodiscard]] ZQtExecutor& renderThreadExecutor();
  [[nodiscard]] const ZQtExecutor& renderThreadExecutor() const;

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

  // Emitted on the UI thread to show the seed-trace context menu for 3D tracing.
  void showSeedTraceContextMenu(QPoint globalPos, size_t imgObjId, size_t sc, float x, float y, float z);

  // Emitted on the UI thread when a background click resolves to a 3D voxel position in the source image.
  // This mirrors neuTube's `pointInVolumeLeftClicked` hook and is used to drive 3D SWC edit modes.
  void pointInVolumeLeftClicked(QPoint globalPos,
                                size_t imgObjId,
                                size_t sc,
                                float x,
                                float y,
                                float z,
                                Qt::KeyboardModifiers modifiers);

  // Emitted on the UI thread to show the SWC-node context menu for 3D editing.
  void showSwcNodeContextMenu(QPoint globalPos, ZSwcPack* swcPack, int64_t clickedNodeId);

  // Emitted on the UI thread to apply SWC-node edit operations driven by 3D interaction modes.
  void request3dSwcAddNeuronNode(ZSwcPack* swcPack, double x, double y, double z, double r);
  void request3dSwcPlainExtend(ZSwcPack* swcPack, double x, double y, double z, double r);
  void request3dSwcConnectToTarget(ZSwcPack* swcPack, int64_t targetNodeId);

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
                                                            Z3DScreenShotType sst,
                                                            bool reportProgress = false,
                                                            folly::CancellationToken cancellationToken = {});

  void takeFixedSizeScreenShotWithoutResetCanvasSizeByTilePrivate(const QString& filename,
                                                                  const QString& rightFilename,
                                                                  int width,
                                                                  int height,
                                                                  Z3DScreenShotType sst,
                                                                  int tileSize = 0,
                                                                  int tileBorder = 0,
                                                                  int tileStartX = 0,
                                                                  int tileStartY = 0,
                                                                  folly::CancellationToken cancellationToken = {});

  // private version will throw exception on error
  void takeScreenShotPrivate(const QString& filename,
                             Z3DScreenShotType sst,
                             bool reportProgress = false,
                             folly::CancellationToken cancellationToken = {});

  void resetOutputSizeToMatchCanvasSize();

  void handleRenderBackendChanged();
  void applyBackendSwitch();
  void pollVulkanCompletionsOnce();
  void maybeStartVulkanCompletionPolling();
  void stopVulkanCompletionPolling();
  [[nodiscard]] bool shouldDeferVulkanNetworkProcessing() const;
  void deferRenderUntilVulkanIdle(QEvent::Type deferredType);
  void maybeKickDeferredRenderAfterVulkanPoll();
  [[nodiscard]] bool beginDeferredRenderingErrorFrame();
  void endDeferredRenderingErrorFrame(bool startedFrame);
  void clearDeferredRenderingErrors();
  void reportDeferredRenderingErrorsIfAny();

  // Execute one pass over the current filter pipeline, polling
  // cancellationToken between filters.
  double processFrame(bool stereo, bool progressiveRendering, folly::CancellationToken cancellationToken = {});

  // Update sizes for all filters in the pipeline, walking from sinks back
  // towards sources, using the engine's output size as the global target.
  void updateAllFilterSizes();

  void prepareMeshFiltersForExport(const glm::uvec2& exportSize, folly::CancellationToken cancellationToken = {});
  void finishMeshFiltersForExport();

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
  std::unique_ptr<ZQtExecutor> m_renderThreadExecutor;
  QTimer* m_vkCompletionPollTimer = nullptr;

  ZDoc& m_doc;

  QPointer<Z3DCanvas> m_canvas;
  bool m_seedTraceToolEnabled = false;
  bool m_seedTraceInProgress = false;
  std::optional<size_t> m_seedTraceSourceImgObjId;
  size_t m_seedTraceSourceChannel = 0;
  std::unique_ptr<Z3DGlobalParameters> m_globalParas;
  std::unique_ptr<Z3DScratchResourcePool, ScratchPoolDeleter> m_scratchPool;
  std::unique_ptr<Z3DCompositor> m_compositor;
  std::vector<std::unique_ptr<Z3DObjView>> m_3dObjViews;
  // Vulkan compositor bridge deferred

  // Linearized filter execution order: all object filters, then compositor.
  std::vector<Z3DFilter*> m_pipeline;
  std::vector<Z3DMeshFilter*> m_exportPreparedMeshFilters;

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
  std::vector<QString> m_deferredRenderingErrors;
  bool m_deferredRenderingErrorFrameActive = false;

  // Backend switch deferred
  bool m_backendSwitchScheduled = false;

  // When Vulkan readback is non-blocking, we may finish CPU submission while
  // the GPU is still processing the previous frame. Coalesce additional render
  // requests by deferring the expensive network processing until the Vulkan
  // frame executor has capacity (inFlightCount < maxFramesInFlight), then post
  // a single event to re-run rendering.
  std::optional<QEvent::Type> m_vkDeferredRenderEventType;

  // Pending per-object View3D json waiting for objViewReady
  std::unordered_map<size_t, json::object> m_pendingObjViewJson;
  int m_sceneApplyOutstanding = 0;

  // (m_observedWGs and m_shuttingDown declared above to ensure lifetime beyond compositor)


};

} // namespace nim
