#include "zrunexport3dscene.h"

#include "z2danimationdoc.h"
#include "z3drenderingengine.h"
#include "z3danimationdoc.h"
#include "zcpuinfo.h"
#include "zdoc.h"
#include "zjson.h"
#include "zlog.h"
#include "zview.h"
#include "zstringutils.h"

#include "zcommandlineflags.h"

#include <folly/ScopeGuard.h>

#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QThread>

#include <string>
#include <utility>
#include <vector>

ABSL_FLAG(bool, run_export_3d_scene, false, "Enable exporting a 3D scene screenshot via command line");

ABSL_DECLARE_FLAG(std::string, filename);
ABSL_DECLARE_FLAG(std::string, output_filename);
ABSL_DECLARE_FLAG(int32_t, output_width);
ABSL_DECLARE_FLAG(int32_t, output_height);
ABSL_DECLARE_FLAG(bool, overwrite);
ABSL_DECLARE_FLAG(uint64_t, limit_memory_usage_in_gb_to);
ABSL_DECLARE_FLAG(int32_t, maximum_output_width);
ABSL_DECLARE_FLAG(int32_t, maximum_output_height);
ABSL_DECLARE_FLAG(int32_t, output_tile_size);
ABSL_DECLARE_FLAG(int32_t, output_tile_border);
ABSL_DECLARE_FLAG(std::vector<std::string>, use_gpu_devices);
ABSL_DECLARE_FLAG(uint32_t, use_gpu_device);

#if defined(__linux__)
ABSL_DECLARE_FLAG(bool, __use_EGL);
#endif

namespace nim {

namespace {

bool prepareOutputFile(const QString& outputFilename, bool overwriteExisting, QString& error)
{
  const QString trimmed = outputFilename.trimmed();
  if (trimmed.isEmpty()) {
    error = "output file name is empty";
    return false;
  }

  QFileInfo outputInfo(trimmed);
  QDir outputDir(outputInfo.absolutePath());
  if (!outputDir.exists() && !outputDir.mkpath(".")) {
    error = QString("Can not create folder %1").arg(outputDir.absolutePath());
    return false;
  }

  if (outputInfo.exists()) {
    if (!overwriteExisting) {
      error = QString("File %1 already exists").arg(outputInfo.absoluteFilePath());
      return false;
    }
    if (!QFile::remove(outputInfo.absoluteFilePath())) {
      error = QString("Can not replace existed file %1").arg(outputInfo.absoluteFilePath());
      return false;
    }
  }

  return true;
}

bool configureSingleGpuFromFlags(QString& error)
{
  const std::vector<std::string> gpuDevices = absl::GetFlag(FLAGS_use_gpu_devices);
  if (gpuDevices.empty()) {
    return true;
  }

#if defined(__linux__)
  if (gpuDevices.size() != 1) {
    error = "Scene export supports exactly one GPU device id in --use_gpu_devices";
    return false;
  }

  uint32_t gpuId = 0;
  if (!stringToValueNoThrow(gpuDevices.front(), gpuId)) {
    error = QString("invalid gpu device %1").arg(QString::fromStdString(gpuDevices.front()));
    return false;
  }

  absl::SetFlag(&FLAGS_use_gpu_device, gpuId);
  absl::SetFlag(&FLAGS___use_EGL, true);
  return true;
#else
  error = "Flag --use_gpu_devices is Linux only";
  return false;
#endif
}

std::pair<int, int> resolveSceneTileSettings()
{
  const auto tileSizeInfo = getCommandLineFlagInfoOrDie("output_tile_size");
  const auto tileBorderInfo = getCommandLineFlagInfoOrDie("output_tile_border");
  if (tileSizeInfo.isDefault && tileBorderInfo.isDefault) {
    return {0, 0};
  }
  return {absl::GetFlag(FLAGS_output_tile_size), absl::GetFlag(FLAGS_output_tile_border)};
}

bool loadSceneForExport(const QString& filename, ZDoc& doc, ZView& view, Z3DRenderingEngine& engine, QString& error)
{
  auto loadObj = loadJsonObject(filename);
  if (!loadObj.contains("Scene") || !loadObj.at("Scene").is_object()) {
    error = "File is not scene format";
    return false;
  }

  const auto& sceneObj = loadObj.at("Scene").as_object();
  if (!sceneObj.contains("Doc") || !sceneObj.at("Doc").is_object()) {
    error = "Scene payload is missing the Doc object";
    return false;
  }

  QDir::setCurrent(QFileInfo(filename).absolutePath());

  std::map<size_t, size_t> idmap = doc.read(sceneObj.at("Doc").as_object(), error);
  if (idmap.empty()) {
    LOG(WARNING) << "Scene " << filename << " contains zero objects";
  }

  bool has3DGeneral = sceneObj.contains("View3DGeneral");
  size_t numView3DPerObject = 0;
  for (const auto& [key, value] : sceneObj) {
    if (key == "Doc" || key == "Version" || key == "View2DGeneral" || key == "View3DGeneral") {
      continue;
    }
    if (value.is_object() && value.as_object().contains("View3D")) {
      ++numView3DPerObject;
    }
  }

  const bool waitFor3DApply = has3DGeneral || numView3DPerObject > 0;
  bool sceneApplyFinished = !waitFor3DApply;
  bool sceneApplyFailed = false;
  QMetaObject::Connection sceneApplyConn;
  QMetaObject::Connection renderErrorConn;
  if (waitFor3DApply) {
    sceneApplyConn = QObject::connect(
      &engine,
      &Z3DRenderingEngine::scene3DApplyFinished,
      &view,
      [&sceneApplyFinished]() {
        sceneApplyFinished = true;
      },
      Qt::QueuedConnection);
    renderErrorConn = QObject::connect(
      &engine,
      &Z3DRenderingEngine::renderingError,
      &view,
      [&sceneApplyFailed, &error](const QString& err) {
        sceneApplyFailed = true;
        if (!err.isEmpty()) {
          if (!error.isEmpty()) {
            error += '\n';
          }
          error += err;
        }
      },
      Qt::QueuedConnection);
    engine.beginScene3DApply();
  }
  auto disconnectSceneApplyGuard = folly::makeGuard([&sceneApplyConn, &renderErrorConn]() {
    if (sceneApplyConn) {
      QObject::disconnect(sceneApplyConn);
    }
    if (renderErrorConn) {
      QObject::disconnect(renderErrorConn);
    }
  });

  for (const auto& [key, value] : sceneObj) {
    if (key == "View2DGeneral") {
      view.read(value.as_object());
      continue;
    }
    if (key == "View3DGeneral") {
      engine.applyView3DGeneral(value.as_object());
      continue;
    }
    if (key == "Doc" || key == "Version") {
      continue;
    }

    QString qkey = QString::fromUtf8(key.data(), key.size());
    bool ok = false;
    const size_t objectId = qkey.toULongLong(&ok);
    if (!ok) {
      error += QString("Unknown scene key %1\n").arg(qkey);
      continue;
    }
    if (!idmap.contains(objectId)) {
      continue;
    }

    const size_t id = idmap.at(objectId);
    const auto& viewObj = value.as_object();
    if (viewObj.contains("View2D")) {
      view.read(id, viewObj.at("View2D").as_object());
    }
    if (viewObj.contains("View3D")) {
      engine.applyView3DForId(id, viewObj.at("View3D").as_object());
    }
  }

  if (!waitFor3DApply) {
    LOG(INFO) << "Finish loading scene";
    return true;
  }

  while (!sceneApplyFinished && !sceneApplyFailed) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    if (sceneApplyFinished || sceneApplyFailed) {
      break;
    }
    QThread::msleep(10);
  }

  if (sceneApplyFailed) {
    if (error.isEmpty()) {
      error = "3D scene apply failed";
    }
    return false;
  }

  LOG(INFO) << "Finish loading scene";
  return true;
}

} // namespace

int ZRunExport3DScene::run()
{
  LOG(INFO) << "Export 3D Scene Start";
  auto guard = folly::makeGuard([]() {
    LOG(INFO) << "Export 3D Scene End";
  });

  m_hasError = false;

  if (absl::GetFlag(FLAGS_limit_memory_usage_in_gb_to) >= 32) {
    ZCpuInfo::instance().setMemoryLimitInBytes(absl::GetFlag(FLAGS_limit_memory_usage_in_gb_to) * 1024 * 1024 * 1024);
  }

  const QString filename = QString::fromStdString(absl::GetFlag(FLAGS_filename)).trimmed();
  if (!QFile::exists(filename)) {
    LOG(ERROR) << fmt::format("input file ({}) does not exist", absl::GetFlag(FLAGS_filename));
    return 1;
  }

  const QString outputFilename = QString::fromStdString(absl::GetFlag(FLAGS_output_filename)).trimmed();
  QString errorMsg;
  if (!prepareOutputFile(outputFilename, absl::GetFlag(FLAGS_overwrite), errorMsg)) {
    LOG(ERROR) << errorMsg;
    return 1;
  }

  if (absl::GetFlag(FLAGS_output_width) <= 0 || absl::GetFlag(FLAGS_output_height) <= 0) {
    LOG(ERROR) << fmt::format("Invalid width {} or height {}",
                              absl::GetFlag(FLAGS_output_width),
                              absl::GetFlag(FLAGS_output_height));
    return 1;
  }
  if (absl::GetFlag(FLAGS_output_width) > absl::GetFlag(FLAGS_maximum_output_width) ||
      absl::GetFlag(FLAGS_output_height) > absl::GetFlag(FLAGS_maximum_output_height)) {
    LOG(ERROR) << fmt::format("Does not support output size larger than {} x {}",
                              absl::GetFlag(FLAGS_maximum_output_width),
                              absl::GetFlag(FLAGS_maximum_output_height));
    return 1;
  }

  if (!configureSingleGpuFromFlags(errorMsg)) {
    LOG(ERROR) << errorMsg;
    return 1;
  }

#if defined(__linux__)
  if (absl::GetFlag(FLAGS_use_gpu_devices).empty()) {
    absl::SetFlag(&FLAGS___use_EGL, true);
  }
#endif

  ZDoc doc;
  ZView view(doc);
  doc.animation2DDoc().bindView(&view);

  Z3DRenderingEngine engine(doc);
  m_engine = &engine;
  auto resetEngineGuard = folly::makeGuard([this]() {
    m_engine = nullptr;
  });
  engine.init();
  doc.animation3DDoc().bindView(&engine);
  connect(&engine, &Z3DRenderingEngine::renderingError, this, &ZRunExport3DScene::logError);

  if (!loadSceneForExport(filename, doc, view, engine, errorMsg)) {
    LOG(ERROR) << "load scene file error: " << errorMsg;
    return 1;
  }
  if (!errorMsg.isEmpty()) {
    LOG(ERROR) << "load scene file error: " << errorMsg;
    return 1;
  }

  doc.hideAnimation3DView();
  doc.deselectAllObjs();
  if (m_hasError) {
    return 1;
  }

  const auto [tileSize, tileBorder] = resolveSceneTileSettings();
  engine.takeFixedSizeScreenShot(outputFilename,
                                 absl::GetFlag(FLAGS_output_width),
                                 absl::GetFlag(FLAGS_output_height),
                                 Z3DScreenShotType::MonoView,
                                 tileSize,
                                 tileBorder);

  if (!m_hasError && !QFile::exists(outputFilename)) {
    LOG(ERROR) << "scene export did not produce an output image";
    return 1;
  }

  return m_hasError ? 1 : 0;
}

void ZRunExport3DScene::logError(const QString& err)
{
  LOG(ERROR) << err;
  m_hasError = true;
  if (m_engine != nullptr) {
    m_engine->cancelCapture();
    m_engine->cancelLongRendering();
  }
}

} // namespace nim
