#include "zrunexport3dscene.h"

#include "z2danimationdoc.h"
#include "z3drenderingengine.h"
#include "z3danimationdoc.h"
#include "zcpuinfo.h"
#include "zdoc.h"
#include "zjson.h"
#include "zlog.h"
#include "zview.h"

#include <folly/ScopeGuard.h>

#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QThread>

DEFINE_bool(run_export_3d_scene, false, "Enable exporting a 3D scene screenshot via command line");

DECLARE_string(filename);
DECLARE_string(output_filename);
DECLARE_int32(output_width);
DECLARE_int32(output_height);
DECLARE_bool(overwrite);
DECLARE_uint64(limit_memory_usage_in_gb_to);
DECLARE_int32(maximum_output_width);
DECLARE_int32(maximum_output_height);
DECLARE_string(use_gpu_devices);
DECLARE_uint32(use_gpu_device);

#if defined(__linux__)
DECLARE_bool(__use_EGL);
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
  const QString gpuDevices = QString::fromStdString(FLAGS_use_gpu_devices).trimmed();
  if (gpuDevices.isEmpty()) {
    return true;
  }

#if defined(__linux__)
  const QStringList gpuIds = gpuDevices.split(',', Qt::SkipEmptyParts);
  if (gpuIds.size() != 1) {
    error = "Scene export supports exactly one GPU device id in --use_gpu_devices";
    return false;
  }

  bool ok = false;
  const uint32_t gpuId = gpuIds.front().trimmed().toUInt(&ok);
  if (!ok) {
    error = QString("invalid gpu device %1").arg(gpuIds.front().trimmed());
    return false;
  }

  FLAGS_use_gpu_device = gpuId;
  FLAGS___use_EGL = true;
  return true;
#else
  error = "Flag --use_gpu_devices is Linux only";
  return false;
#endif
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
  QMetaObject::Connection sceneApplyConn;
  if (waitFor3DApply) {
    sceneApplyConn = QObject::connect(
      &engine,
      &Z3DRenderingEngine::scene3DApplyFinished,
      &view,
      [&sceneApplyFinished]() {
        sceneApplyFinished = true;
      },
      Qt::QueuedConnection);
    engine.beginScene3DApply();
  }
  auto disconnectSceneApplyGuard = folly::makeGuard([&sceneApplyConn]() {
    if (sceneApplyConn) {
      QObject::disconnect(sceneApplyConn);
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

  while (!sceneApplyFinished) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    if (sceneApplyFinished) {
      break;
    }
    QThread::msleep(10);
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

  if (FLAGS_limit_memory_usage_in_gb_to >= 32) {
    ZCpuInfo::instance().setMemoryLimitInBytes(FLAGS_limit_memory_usage_in_gb_to * 1024 * 1024 * 1024);
  }

  const QString filename = QString::fromStdString(FLAGS_filename).trimmed();
  if (!QFile::exists(filename)) {
    LOG(ERROR) << fmt::format("input file ({}) does not exist", FLAGS_filename);
    return 1;
  }

  const QString outputFilename = QString::fromStdString(FLAGS_output_filename).trimmed();
  QString errorMsg;
  if (!prepareOutputFile(outputFilename, FLAGS_overwrite, errorMsg)) {
    LOG(ERROR) << errorMsg;
    return 1;
  }

  if (FLAGS_output_width <= 0 || FLAGS_output_height <= 0) {
    LOG(ERROR) << fmt::format("Invalid width {} or height {}", FLAGS_output_width, FLAGS_output_height);
    return 1;
  }
  if (FLAGS_output_width > FLAGS_maximum_output_width || FLAGS_output_height > FLAGS_maximum_output_height) {
    LOG(ERROR) << fmt::format("Does not support output size larger than {} x {}",
                              FLAGS_maximum_output_width,
                              FLAGS_maximum_output_height);
    return 1;
  }

  if (!configureSingleGpuFromFlags(errorMsg)) {
    LOG(ERROR) << errorMsg;
    return 1;
  }

#if defined(__linux__)
  if (QString::fromStdString(FLAGS_use_gpu_devices).trimmed().isEmpty()) {
    FLAGS___use_EGL = true;
  }
#endif

  ZDoc doc;
  ZView view(doc);
  doc.animation2DDoc().bindView(&view);

  Z3DRenderingEngine engine(doc);
  engine.init();
  doc.animation3DDoc().bindView(&engine);

  if (!loadSceneForExport(filename, doc, view, engine, errorMsg)) {
    LOG(ERROR) << "load scene file error: " << errorMsg;
    return 1;
  }
  if (!errorMsg.isEmpty()) {
    LOG(WARNING) << errorMsg;
  }

  doc.hideAnimation3DView();
  doc.deselectAllObjs();

  connect(&engine, &Z3DRenderingEngine::renderingError, this, &ZRunExport3DScene::logError);
  engine.takeFixedSizeScreenShot(outputFilename, FLAGS_output_width, FLAGS_output_height, Z3DScreenShotType::MonoView);

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
}

} // namespace nim
