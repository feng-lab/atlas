#include "zneutubeskeletonizeprocess.h"

#include "zneutubeskeletonizer.h"

#include "zcancellation.h"
#include "zexception.h"
#include "zimg.h"
#include "zjson.h"
#include "zlog.h"
#include "zswcwriter.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QUuid>

#include <folly/ScopeGuard.h>

namespace nim {

namespace {

void applySkeletonizeConfig(const json::object& cfg,
                            const std::optional<std::array<int, 3>>& overrideDownsampleInterval,
                            ZNeutubeSkeletonizer* skeletonizer)
{
  CHECK(skeletonizer != nullptr);

  if (auto it = cfg.find("minimalLength"); it != cfg.end() && it->value().is_number()) {
    skeletonizer->setLengthThreshold(it->value().to_number<double>());
  }
  if (auto it = cfg.find("lengthThreshold"); it != cfg.end() && it->value().is_number()) {
    skeletonizer->setLengthThreshold(it->value().to_number<double>());
  }
  if (auto it = cfg.find("finalMinimalLength"); it != cfg.end() && it->value().is_number()) {
    skeletonizer->setFinalLengthThreshold(it->value().to_number<double>());
  }
  if (auto it = cfg.find("finalLengthThreshold"); it != cfg.end() && it->value().is_number()) {
    skeletonizer->setFinalLengthThreshold(it->value().to_number<double>());
  }
  if (auto it = cfg.find("maximalDistance"); it != cfg.end() && it->value().is_number()) {
    skeletonizer->setDistanceThreshold(it->value().to_number<double>());
  }
  if (auto it = cfg.find("distanceThreshold"); it != cfg.end() && it->value().is_number()) {
    skeletonizer->setDistanceThreshold(it->value().to_number<double>());
  }
  if (auto it = cfg.find("keepingSingleObject"); it != cfg.end() && it->value().is_bool()) {
    skeletonizer->setKeepingSingleObject(it->value().as_bool());
  }
  if (auto it = cfg.find("rebase"); it != cfg.end() && it->value().is_bool()) {
    skeletonizer->setRebase(it->value().as_bool());
  }
  if (auto it = cfg.find("fillingHole"); it != cfg.end() && it->value().is_bool()) {
    skeletonizer->setFillingHole(it->value().as_bool());
  }
  if (auto it = cfg.find("minimalObjectSize"); it != cfg.end() && it->value().is_int64()) {
    skeletonizer->setMinObjSize(static_cast<int>(it->value().as_int64()));
  }
  if (auto it = cfg.find("level"); it != cfg.end() && it->value().is_int64()) {
    skeletonizer->setLevel(static_cast<int>(it->value().as_int64()));
  }
  if (auto it = cfg.find("grayOp"); it != cfg.end() && it->value().is_int64()) {
    skeletonizer->setLevelOp(static_cast<int>(it->value().as_int64()));
  }
  if (auto it = cfg.find("levelOp"); it != cfg.end() && it->value().is_int64()) {
    skeletonizer->setLevelOp(static_cast<int>(it->value().as_int64()));
  }
  if (auto it = cfg.find("interpolating"); it != cfg.end() && it->value().is_bool()) {
    skeletonizer->setInterpolating(it->value().as_bool());
  }
  if (auto it = cfg.find("removingBorder"); it != cfg.end() && it->value().is_bool()) {
    skeletonizer->setRemovingBorder(it->value().as_bool());
  }
  if (auto it = cfg.find("connectingBranch"); it != cfg.end() && it->value().is_bool()) {
    skeletonizer->setConnectingBranch(it->value().as_bool());
  }
  if (auto it = cfg.find("usingOriginalSignal"); it != cfg.end() && it->value().is_bool()) {
    skeletonizer->useOriginalSignal(it->value().as_bool());
  }
  if (auto it = cfg.find("resampleSwc"); it != cfg.end() && it->value().is_bool()) {
    skeletonizer->setResampleSwc(it->value().as_bool());
  }
  if (auto it = cfg.find("autoGrayThreshold"); it != cfg.end() && it->value().is_bool()) {
    skeletonizer->setAutoGrayThreshold(it->value().as_bool());
  }
  if (auto it = cfg.find("resolution"); it != cfg.end() && it->value().is_array()) {
    const auto& arr = it->value().as_array();
    if (arr.size() == 2 && arr[0].is_number() && arr[1].is_number()) {
      skeletonizer->setResolution(arr[0].to_number<double>(), arr[1].to_number<double>());
    } else if (arr.size() == 3 && arr[0].is_number() && arr[2].is_number()) {
      skeletonizer->setResolution(arr[0].to_number<double>(), arr[2].to_number<double>());
    } else {
      LOG(WARNING) << "Invalid skeletonize.resolution; expected array[2] or array[3] of numbers";
    }
  }

  std::optional<std::array<int, 3>> cfgIntv;
  if (auto it = cfg.find("downsampleInterval"); it != cfg.end() && it->value().is_array()) {
    const auto& arr = it->value().as_array();
    if (arr.size() == 3 && arr[0].is_int64() && arr[1].is_int64() && arr[2].is_int64()) {
      cfgIntv = std::array<int, 3>{static_cast<int>(arr[0].as_int64()),
                                   static_cast<int>(arr[1].as_int64()),
                                   static_cast<int>(arr[2].as_int64())};
    } else {
      LOG(WARNING) << "Invalid skeletonize.downsampleInterval; expected array[3] of int";
    }
  }

  if (overrideDownsampleInterval) {
    skeletonizer->setDownsampleInterval((*overrideDownsampleInterval)[0],
                                        (*overrideDownsampleInterval)[1],
                                        (*overrideDownsampleInterval)[2]);
  } else if (cfgIntv) {
    skeletonizer->setDownsampleInterval((*cfgIntv)[0], (*cfgIntv)[1], (*cfgIntv)[2]);
  }
}

} // namespace

void ZNeutubeSkeletonizeProcess::writeSwcAtomicOrThrow(ZSwc& tree) const
{
  if (m_outputSwcPath.isEmpty()) {
    throw ZException("Binary -> SWC failed: output SWC path is empty.");
  }

  const QDir dir = QFileInfo(m_outputSwcPath).dir();
  if (!dir.exists() && !dir.mkpath(QStringLiteral("."))) {
    throw ZException(
      QStringLiteral("Binary -> SWC failed: can not create output directory: %1").arg(dir.absolutePath()));
  }

  const QString tmpSwcPath =
    m_outputSwcPath + QStringLiteral(".tmp_") + QUuid::createUuid().toString(QUuid::WithoutBraces);
  auto tmpGuard = folly::makeGuard([&]() {
    (void)QFile::remove(tmpSwcPath);
  });

  writeSwcLegacyNeuTuOrThrow(tree, tmpSwcPath.toStdString(), {});

  if (QFile::exists(m_outputSwcPath) && !QFile::remove(m_outputSwcPath)) {
    throw ZException(QStringLiteral("Binary -> SWC failed: can not overwrite output SWC: %1").arg(m_outputSwcPath));
  }
  if (!QFile::rename(tmpSwcPath, m_outputSwcPath)) {
    throw ZException(QStringLiteral("Binary -> SWC failed: can not move temp SWC into place.\nTemp: %1\nFinal: %2")
                       .arg(tmpSwcPath, m_outputSwcPath));
  }

  tmpGuard.dismiss();
}

void ZNeutubeSkeletonizeProcess::doWork()
{
  m_hasResult = false;

  if (m_inputImagePath.trimmed().isEmpty()) {
    throw ZException("Binary -> SWC failed: input image path is empty.");
  }
  if (m_outputSwcPath.trimmed().isEmpty()) {
    throw ZException("Binary -> SWC failed: output SWC path is empty.");
  }

  LOG(INFO) << "Binary -> SWC (skeletonize)";
  LOG(INFO) << "Input image: " << m_inputImagePath;
  LOG(INFO) << "Skeletonize config: " << m_skeletonizeConfigPath;
  LOG(INFO) << "Output SWC: " << m_outputSwcPath;

  maybeCancel(m_cancellationToken);

  json::object cfg;
  if (m_skeletonizeConfig) {
    cfg = *m_skeletonizeConfig;
  } else if (!m_skeletonizeConfigPath.trimmed().isEmpty()) {
    cfg = loadJsonObject(m_skeletonizeConfigPath);
  }

  ZNeutubeSkeletonizer skeletonizer;
  applySkeletonizeConfig(cfg, m_downsampleIntervalOverride, &skeletonizer);

  LOG(INFO) << "==========Skeletonize configuration (effective)========";
  LOG(INFO) << nim::jsonToFormattedString(cfg);
  skeletonizer.print();
  LOG(INFO) << "======================================================";

  maybeCancel(m_cancellationToken);

  ZImg img(m_inputImagePath);

  maybeCancel(m_cancellationToken);

  std::unique_ptr<ZSwc> tree = skeletonizer.makeSkeleton(img);

  maybeCancel(m_cancellationToken);

  if (!tree || tree->empty()) {
    if (QFile::exists(m_outputSwcPath) && !QFile::remove(m_outputSwcPath)) {
      throw ZException(
        QStringLiteral("Binary -> SWC failed: can not remove stale output SWC: %1").arg(m_outputSwcPath));
    }
    LOG(INFO) << "No SWC generated.";
    return;
  }

  writeSwcAtomicOrThrow(*tree);
  m_hasResult = true;
  reportProgress(1.0);
}

void ZNeutubeSkeletonizeProcess::read(const json::object& jo)
{
  if (auto it = jo.find("input_image_path"); it != jo.end()) {
    m_inputImagePath = json::value_to<QString>(it->value());
  }
  if (auto it = jo.find("skeletonize_config_path"); it != jo.end()) {
    m_skeletonizeConfigPath = json::value_to<QString>(it->value());
  }
  m_skeletonizeConfig.reset();
  if (auto it = jo.find("skeletonize_config"); it != jo.end()) {
    if (it->value().is_object()) {
      m_skeletonizeConfig = it->value().as_object();
    } else {
      throw ZException(QStringLiteral("Invalid skeletonize_config: expected object, got %1")
                         .arg(QString::fromStdString(jsonTypeName(it->value()))));
    }
  }
  if (auto it = jo.find("verbose"); it != jo.end()) {
    m_verbose = json::value_to<bool>(it->value());
  }
  if (auto it = jo.find("output_swc_path"); it != jo.end()) {
    m_outputSwcPath = json::value_to<QString>(it->value());
  }

  m_downsampleIntervalOverride.reset();
  if (auto it = jo.find("downsample_interval_override"); it != jo.end() && it->value().is_array()) {
    const auto& arr = it->value().as_array();
    if (arr.size() == 3 && arr[0].is_int64() && arr[1].is_int64() && arr[2].is_int64()) {
      m_downsampleIntervalOverride = std::array<int, 3>{static_cast<int>(arr[0].as_int64()),
                                                        static_cast<int>(arr[1].as_int64()),
                                                        static_cast<int>(arr[2].as_int64())};
    }
  }
}

void ZNeutubeSkeletonizeProcess::write(json::object& jo) const
{
  jo["input_image_path"] = json::value_from(m_inputImagePath);
  jo["skeletonize_config_path"] = json::value_from(m_skeletonizeConfigPath);
  if (m_skeletonizeConfig) {
    jo["skeletonize_config"] = *m_skeletonizeConfig;
  }
  jo["verbose"] = json::value_from(m_verbose);
  jo["output_swc_path"] = json::value_from(m_outputSwcPath);

  if (m_downsampleIntervalOverride) {
    jo["downsample_interval_override"] = json::value_from(*m_downsampleIntervalOverride);
  }
}

} // namespace nim
