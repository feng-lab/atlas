#include "zneutubeautotraceprocess.h"

#include "zneutubetraceauto.h"
#include "zneutubetracezscale.h"

#include "zcancellation.h"
#include "zexception.h"
#include "zlog.h"
#include "zswcwriter.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QUuid>

#include <folly/ScopeGuard.h>

#include <cmath>
#include <utility>

namespace nim {

namespace {

void rescaleSwcInPlace(ZSwc& tree, double scaleX, double scaleY, double scaleZ, bool scaleRadius)
{
  if (scaleX == 1.0 && scaleY == 1.0 && scaleZ == 1.0) {
    return;
  }

  const double radiusScale = std::sqrt(scaleX * scaleY);

  for (auto it = tree.begin(); it != tree.end(); ++it) {
    it->x *= scaleX;
    it->y *= scaleY;
    it->z *= scaleZ;
    if (scaleRadius) {
      it->radius *= radiusScale;
    }
  }
}

[[nodiscard]] ZImgRegion selectedChannelTimeRegionOrThrow(const ZImgInfo& info, size_t c, size_t t)
{
  if (c >= info.numChannels || t >= info.numTimes) {
    throw ZException(
      fmt::format("Auto Trace failed: invalid channel/time selection (c={}, t={}) for signal <{}>.", c, t, info));
  }

  const auto cStart = static_cast<ZImgRegion::value_type>(c);
  const auto cEnd = static_cast<ZImgRegion::value_type>(c + 1);
  const auto tStart = static_cast<ZImgRegion::value_type>(t);
  const auto tEnd = static_cast<ZImgRegion::value_type>(t + 1);
  return ZImgRegion(0, -1, 0, -1, 0, -1, cStart, cEnd, tStart, tEnd);
}

[[nodiscard]] ZImgSource sourceWithRelativeRegion(const ZImgSource& source, const ZImgRegion& subregion)
{
  ZImgSource out = source;
  auto offsetStart = [](ZImgRegion::value_type base, ZImgRegion::value_type rel) -> ZImgRegion::value_type {
    CHECK(base >= 0);
    CHECK(rel >= 0);
    return base + rel;
  };
  auto offsetEnd = [](ZImgRegion::value_type baseStart,
                      ZImgRegion::value_type baseEnd,
                      ZImgRegion::value_type relEnd) -> ZImgRegion::value_type {
    if (relEnd == -1) {
      return baseEnd;
    }
    CHECK(baseStart >= 0);
    CHECK(relEnd >= 0);
    return baseStart + relEnd;
  };

  out.region.start.x = offsetStart(source.region.start.x, subregion.start.x);
  out.region.start.y = offsetStart(source.region.start.y, subregion.start.y);
  out.region.start.z = offsetStart(source.region.start.z, subregion.start.z);
  out.region.start.c = offsetStart(source.region.start.c, subregion.start.c);
  out.region.start.t = offsetStart(source.region.start.t, subregion.start.t);

  out.region.end.x = offsetEnd(source.region.start.x, source.region.end.x, subregion.end.x);
  out.region.end.y = offsetEnd(source.region.start.y, source.region.end.y, subregion.end.y);
  out.region.end.z = offsetEnd(source.region.start.z, source.region.end.z, subregion.end.z);
  out.region.end.c = offsetEnd(source.region.start.c, source.region.end.c, subregion.end.c);
  out.region.end.t = offsetEnd(source.region.start.t, source.region.end.t, subregion.end.t);
  return out;
}

} // namespace

ZImg ZNeutubeAutoTraceProcess::loadSelectedSignalOrThrow() const
{
  if (m_signalProvider) {
    maybeCancel(m_cancellationToken);
    ZImg signal = m_signalProvider(m_cancellationToken);
    maybeCancel(m_cancellationToken);

    if (!signal.isEmpty()) {
      CHECK(signal.numChannels() == 1);
      CHECK(signal.numTimes() == 1);
    }
    return signal;
  }

  if (!m_inputImageSource.has_value()) {
    throw ZException("Auto Trace failed: no signal provider or input image source configured.");
  }

  const ZImgInfo info = ZImg::readImgInfo(*m_inputImageSource);
  const ZImgRegion region = selectedChannelTimeRegionOrThrow(info, m_selectedChannel, m_selectedTime);
  const ZImgSource signalSource = sourceWithRelativeRegion(*m_inputImageSource, region);

  maybeCancel(m_cancellationToken);
  ZImg signal;
  signal.load(signalSource, m_signalDownsampleRatio[0], m_signalDownsampleRatio[1], m_signalDownsampleRatio[2]);
  maybeCancel(m_cancellationToken);

  if (!signal.isEmpty()) {
    CHECK(signal.numChannels() == 1);
    CHECK(signal.numTimes() == 1);
  }
  return signal;
}

TraceConfig ZNeutubeAutoTraceProcess::buildEffectiveTraceConfigOrThrow() const
{
  TraceConfig cfg;

  if (m_traceConfig) {
    const bool ok = loadTraceConfigLegacyLike(*m_traceConfig, cfg);
    if (!ok) {
      cfg = TraceConfig{};
    }
  } else if (!m_traceConfigPath.isEmpty()) {
    const bool ok = loadTraceConfigLegacyLike(m_traceConfigPath, cfg);
    if (!ok) {
      cfg = TraceConfig{};
    }
  }

  if (m_traceLevel > 0) {
    if (const json::object* levelOverride = selectTraceLevelOverrideLegacyLike(cfg, m_traceLevel)) {
      applyTraceConfigOverridesLegacyLike(*levelOverride, cfg);
    }
  }

  if (m_haveAlgoOverrides) {
    cfg.minAutoScore = m_algoOverrides.minAutoScore;
    cfg.minManualScore = m_algoOverrides.minManualScore;
    cfg.minSeedScore = m_algoOverrides.minSeedScore;
    cfg.min2dScore = m_algoOverrides.min2dScore;
    cfg.refit = m_algoOverrides.refit;
    cfg.spTest = m_algoOverrides.spTest;
    cfg.crossoverTest = m_algoOverrides.crossoverTest;
    cfg.tuneEnd = m_algoOverrides.tuneEnd;
    cfg.edgePath = m_algoOverrides.edgePath;
    cfg.enhanceMask = m_algoOverrides.enhanceMask;
    cfg.seedMethod = m_algoOverrides.seedMethod;
    cfg.recover = m_algoOverrides.recover;
    cfg.chainScreenCount = m_algoOverrides.chainScreenCount;
    cfg.maxEucDist = m_algoOverrides.maxEucDist;
  }

  if (m_docHasAnySwc) {
    cfg.recover = 0;
  }

  return cfg;
}

double ZNeutubeAutoTraceProcess::effectiveZToXYRatioOrThrow(const ZImgInfo& signalInfo) const
{
  if (m_zToXYRatio.has_value()) {
    return *m_zToXYRatio;
  }
  if (signalInfo.isEmpty()) {
    throw ZException("Auto Trace failed: missing zToXYRatio and signal metadata.");
  }
  return preferredZToXYRatioFromImgInfoLegacyLike(signalInfo, m_signalDownsampleRatio);
}

void ZNeutubeAutoTraceProcess::writeSwcAtomicOrThrow(ZSwc& tree) const
{
  if (m_outputSwcPath.isEmpty()) {
    throw ZException("Auto Trace failed: output SWC path is empty.");
  }

  const QDir dir = QFileInfo(m_outputSwcPath).dir();
  if (!dir.exists() && !dir.mkpath(QStringLiteral("."))) {
    throw ZException(fmt::format("Auto Trace failed: can not create output directory: {}", dir.absolutePath()));
  }

  const QString tmpSwcPath =
    m_outputSwcPath + QStringLiteral(".tmp_") + QUuid::createUuid().toString(QUuid::WithoutBraces);
  auto tmpGuard = folly::makeGuard([&]() {
    (void)QFile::remove(tmpSwcPath);
  });

  writeSwcLegacyNeuTuOrThrow(tree, tmpSwcPath, {});

  if (QFile::exists(m_outputSwcPath) && !QFile::remove(m_outputSwcPath)) {
    throw ZException(fmt::format("Auto Trace failed: can not overwrite output SWC: {}", m_outputSwcPath));
  }
  if (!QFile::rename(tmpSwcPath, m_outputSwcPath)) {
    throw ZException(fmt::format("Auto Trace failed: can not move temp SWC into place.\nTemp: {}\nFinal: {}",
                                 tmpSwcPath,
                                 m_outputSwcPath));
  }

  tmpGuard.dismiss();
}

void ZNeutubeAutoTraceProcess::doWork()
{
  m_hasResult = false;

  LOG(INFO) << "Atlas Auto Trace";
  LOG(INFO) << "Selected channel (0-based): " << m_selectedChannel;
  LOG(INFO) << "Selected time (0-based): " << m_selectedTime;
  LOG(INFO) << "Signal downsample ratio: [" << m_signalDownsampleRatio[0] << "," << m_signalDownsampleRatio[1] << ","
            << m_signalDownsampleRatio[2] << "]";
  LOG(INFO) << "Budget level override (0=default): " << m_traceLevel;
  LOG(INFO) << "Optimal node resampling: " << (m_doResampleAfterTracing ? "enabled" : "disabled");
  LOG(INFO) << "Trace config path: " << m_traceConfigPath;
  LOG(INFO) << "Output SWC: " << m_outputSwcPath;

  maybeCancel(m_cancellationToken);
  ZImg signal = loadSelectedSignalOrThrow();
  if (signal.isEmpty()) {
    LOG(INFO) << "Auto Trace: signal is empty.";
    return;
  }
  const double zToXYRatio = effectiveZToXYRatioOrThrow(signal.info());
  LOG(INFO) << fmt::format("Tracing zToXYRatio: {:.6g}", zToXYRatio);

  TraceConfig cfg = buildEffectiveTraceConfigOrThrow();

  LOG(INFO) << "Final TraceConfig:";
  LOG(INFO) << "  minAutoScore=" << cfg.minAutoScore;
  LOG(INFO) << "  minManualScore=" << cfg.minManualScore;
  LOG(INFO) << "  minSeedScore=" << cfg.minSeedScore;
  LOG(INFO) << "  min2dScore=" << cfg.min2dScore;
  LOG(INFO) << "  refit=" << (cfg.refit ? "true" : "false");
  LOG(INFO) << "  spTest=" << (cfg.spTest ? "true" : "false");
  LOG(INFO) << "  crossoverTest=" << (cfg.crossoverTest ? "true" : "false");
  LOG(INFO) << "  tuneEnd=" << (cfg.tuneEnd ? "true" : "false");
  LOG(INFO) << "  edgePath=" << (cfg.edgePath ? "true" : "false");
  LOG(INFO) << "  enhanceMask=" << (cfg.enhanceMask ? "true" : "false");
  LOG(INFO) << "  seedMethod=" << cfg.seedMethod;
  LOG(INFO) << "  recover=" << cfg.recover;
  LOG(INFO) << "  chainScreenCount=" << cfg.chainScreenCount;
  LOG(INFO) << "  maxEucDist=" << cfg.maxEucDist;

  maybeCancel(m_cancellationToken);
  std::unique_ptr<ZSwc> swc = traceNeuronAutoLegacyLike(std::move(signal),
                                                        cfg,
                                                        zToXYRatio,
                                                        /*diagnosis=*/false,
                                                        /*verbose=*/false,
                                                        /*doResampleAfterTracing=*/m_doResampleAfterTracing,
                                                        /*predefinedMask=*/nullptr,
                                                        m_cancellationToken);
  maybeCancel(m_cancellationToken);

  if (!swc || swc->empty()) {
    LOG(INFO) << "Auto Trace: no SWC generated.";
    return;
  }

  if (m_signalDownsampleRatio != std::array<size_t, 3>{1, 1, 1}) {
    LOG(INFO) << "Rescaling SWC back to original voxel coordinates...";
    rescaleSwcInPlace(*swc,
                      static_cast<double>(m_signalDownsampleRatio[0]),
                      static_cast<double>(m_signalDownsampleRatio[1]),
                      static_cast<double>(m_signalDownsampleRatio[2]),
                      /*scaleRadius=*/true);
    maybeCancel(m_cancellationToken);
  }

  LOG(INFO) << "Writing SWC...";
  writeSwcAtomicOrThrow(*swc);
  m_hasResult = true;

  LOG(INFO) << "Finished.";
}

void ZNeutubeAutoTraceProcess::read(const json::object& jo)
{
  m_inputImageSource.reset();
  if (auto it = jo.find("selected_channel"); it != jo.end()) {
    m_selectedChannel = json::value_to<size_t>(it->value());
  }
  if (auto it = jo.find("selected_time"); it != jo.end()) {
    m_selectedTime = json::value_to<size_t>(it->value());
  }
  if (const auto inputImageSourceIt = jo.find("input_image_source"); inputImageSourceIt != jo.end()) {
    if (inputImageSourceIt->value().is_object()) {
      m_inputImageSource = json::value_to<ZImgSource>(inputImageSourceIt->value());
    } else {
      throw ZException(
        fmt::format("Invalid input_image_source: expected object, got {}", jsonTypeName(inputImageSourceIt->value())));
    }
  } else if (const auto inputImagePathIt = jo.find("input_image_path"); inputImagePathIt != jo.end()) {
    m_inputImageSource = ZImgSource(json::value_to<QString>(inputImagePathIt->value()));
  }
  if (auto it = jo.find("z_scale"); it != jo.end()) {
    setZToXYRatio(json::value_to<double>(it->value()));
  }
  if (auto it = jo.find("signal_downsample_ratio"); it != jo.end() && it->value().is_array()) {
    const auto& a = it->value().as_array();
    CHECK(a.size() == 3);
    m_signalDownsampleRatio = {
      json::value_to<size_t>(a.at(0)),
      json::value_to<size_t>(a.at(1)),
      json::value_to<size_t>(a.at(2)),
    };
    CHECK(m_signalDownsampleRatio[0] > 0);
    CHECK(m_signalDownsampleRatio[1] > 0);
    CHECK(m_signalDownsampleRatio[2] > 0);
  }
  if (auto it = jo.find("trace_config_path"); it != jo.end()) {
    m_traceConfigPath = json::value_to<QString>(it->value());
  }
  m_traceConfig.reset();
  if (auto it = jo.find("trace_config"); it != jo.end()) {
    if (it->value().is_object()) {
      m_traceConfig = it->value().as_object();
    } else {
      throw ZException(fmt::format("Invalid trace_config: expected object, got {}", jsonTypeName(it->value())));
    }
  }
  if (auto it = jo.find("trace_level"); it != jo.end()) {
    m_traceLevel = json::value_to<int>(it->value());
  }
  if (auto it = jo.find("do_resample_after_tracing"); it != jo.end()) {
    m_doResampleAfterTracing = json::value_to<bool>(it->value());
  }
  if (auto it = jo.find("doc_has_any_swc"); it != jo.end()) {
    m_docHasAnySwc = json::value_to<bool>(it->value());
  }
  if (auto it = jo.find("output_swc_path"); it != jo.end()) {
    m_outputSwcPath = json::value_to<QString>(it->value());
  }

  if (auto it = jo.find("algo_overrides"); it != jo.end() && it->value().is_object()) {
    const auto& ao = it->value().as_object();
    TraceConfig cfg;
    if (auto f = ao.find("minAutoScore"); f != ao.end()) {
      cfg.minAutoScore = json::value_to<double>(f->value());
    }
    if (auto f = ao.find("minManualScore"); f != ao.end()) {
      cfg.minManualScore = json::value_to<double>(f->value());
    }
    if (auto f = ao.find("minSeedScore"); f != ao.end()) {
      cfg.minSeedScore = json::value_to<double>(f->value());
    }
    if (auto f = ao.find("min2dScore"); f != ao.end()) {
      cfg.min2dScore = json::value_to<double>(f->value());
    }
    if (auto f = ao.find("refit"); f != ao.end()) {
      cfg.refit = json::value_to<bool>(f->value());
    }
    if (auto f = ao.find("spTest"); f != ao.end()) {
      cfg.spTest = json::value_to<bool>(f->value());
    }
    if (auto f = ao.find("crossoverTest"); f != ao.end()) {
      cfg.crossoverTest = json::value_to<bool>(f->value());
    }
    if (auto f = ao.find("tuneEnd"); f != ao.end()) {
      cfg.tuneEnd = json::value_to<bool>(f->value());
    }
    if (auto f = ao.find("edgePath"); f != ao.end()) {
      cfg.edgePath = json::value_to<bool>(f->value());
    }
    if (auto f = ao.find("enhanceMask"); f != ao.end()) {
      cfg.enhanceMask = json::value_to<bool>(f->value());
    }
    if (auto f = ao.find("seedMethod"); f != ao.end()) {
      cfg.seedMethod = json::value_to<int>(f->value());
    }
    if (auto f = ao.find("recover"); f != ao.end()) {
      cfg.recover = json::value_to<int>(f->value());
    }
    if (auto f = ao.find("chainScreenCount"); f != ao.end()) {
      cfg.chainScreenCount = json::value_to<int>(f->value());
    }
    if (auto f = ao.find("maxEucDist"); f != ao.end()) {
      cfg.maxEucDist = json::value_to<double>(f->value());
    }
    setAlgoConfigOverrides(cfg);
  } else {
    clearAlgoConfigOverrides();
  }
}

void ZNeutubeAutoTraceProcess::write(json::object& jo) const
{
  if (m_inputImageSource) {
    jo["input_image_source"] = json::value_from(*m_inputImageSource);
  }
  jo["selected_channel"] = json::value_from(m_selectedChannel);
  jo["selected_time"] = json::value_from(m_selectedTime);
  const auto zToXYRatio = [&]() -> double {
    if (m_zToXYRatio.has_value()) {
      return *m_zToXYRatio;
    }
    if (m_inputImageSource) {
      return effectiveZToXYRatioOrThrow(ZImg::readImgInfo(*m_inputImageSource));
    }
    throw ZException(
      "Auto Trace task serialization failed: missing zToXYRatio. Set zToXYRatio explicitly or setInputImageSource().");
  }();
  jo["z_scale"] = json::value_from(zToXYRatio);
  {
    json::array a;
    a.push_back(json::value_from(m_signalDownsampleRatio[0]));
    a.push_back(json::value_from(m_signalDownsampleRatio[1]));
    a.push_back(json::value_from(m_signalDownsampleRatio[2]));
    jo["signal_downsample_ratio"] = std::move(a);
  }
  jo["trace_config_path"] = json::value_from(m_traceConfigPath);
  if (m_traceConfig) {
    jo["trace_config"] = *m_traceConfig;
  }
  jo["trace_level"] = json::value_from(m_traceLevel);
  jo["do_resample_after_tracing"] = json::value_from(m_doResampleAfterTracing);
  jo["doc_has_any_swc"] = json::value_from(m_docHasAnySwc);
  jo["output_swc_path"] = json::value_from(m_outputSwcPath);

  if (m_haveAlgoOverrides) {
    json::object ao;
    ao["minAutoScore"] = json::value_from(m_algoOverrides.minAutoScore);
    ao["minManualScore"] = json::value_from(m_algoOverrides.minManualScore);
    ao["minSeedScore"] = json::value_from(m_algoOverrides.minSeedScore);
    ao["min2dScore"] = json::value_from(m_algoOverrides.min2dScore);
    ao["refit"] = json::value_from(m_algoOverrides.refit);
    ao["spTest"] = json::value_from(m_algoOverrides.spTest);
    ao["crossoverTest"] = json::value_from(m_algoOverrides.crossoverTest);
    ao["tuneEnd"] = json::value_from(m_algoOverrides.tuneEnd);
    ao["edgePath"] = json::value_from(m_algoOverrides.edgePath);
    ao["enhanceMask"] = json::value_from(m_algoOverrides.enhanceMask);
    ao["seedMethod"] = json::value_from(m_algoOverrides.seedMethod);
    ao["recover"] = json::value_from(m_algoOverrides.recover);
    ao["chainScreenCount"] = json::value_from(m_algoOverrides.chainScreenCount);
    ao["maxEucDist"] = json::value_from(m_algoOverrides.maxEucDist);
    jo["algo_overrides"] = std::move(ao);
  }
}

} // namespace nim
