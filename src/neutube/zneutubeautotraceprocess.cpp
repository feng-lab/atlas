#include "zneutubeautotraceprocess.h"

#include "zneutubetraceauto.h"

#include "zcancellation.h"
#include "zexception.h"
#include "zlog.h"
#include "zswcwriter.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QUuid>

#include <folly/ScopeGuard.h>

#include <utility>

namespace nim {

namespace {

[[nodiscard]] QString qulonglongStr(size_t v)
{
  return QString::number(static_cast<qulonglong>(v));
}

} // namespace

ZImg ZNeutubeAutoTraceProcess::loadSelectedSignalOrThrow() const
{
  if (!m_signalProvider) {
    throw ZException("Auto Trace failed: no signal provider configured.");
  }

  maybeCancel(m_cancellationToken);
  ZImg signal = m_signalProvider(m_cancellationToken);
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

  if (!m_traceConfigPath.isEmpty()) {
    const bool ok = loadTraceConfigLegacyLike(m_traceConfigPath.toStdString(), cfg);
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

void ZNeutubeAutoTraceProcess::writeSwcAtomicOrThrow(ZSwc& tree) const
{
  if (m_outputSwcPath.isEmpty()) {
    throw ZException("Auto Trace failed: output SWC path is empty.");
  }

  const QDir dir = QFileInfo(m_outputSwcPath).dir();
  if (!dir.exists() && !dir.mkpath(QStringLiteral("."))) {
    throw ZException(QStringLiteral("Auto Trace failed: can not create output directory: %1").arg(dir.absolutePath()));
  }

  const QString tmpSwcPath =
    m_outputSwcPath + QStringLiteral(".tmp_") + QUuid::createUuid().toString(QUuid::WithoutBraces);
  auto tmpGuard = folly::makeGuard([&]() {
    (void)QFile::remove(tmpSwcPath);
  });

  writeSwcLegacyNeuTuOrThrow(tree, tmpSwcPath.toStdString(), {});

  if (QFile::exists(m_outputSwcPath) && !QFile::remove(m_outputSwcPath)) {
    throw ZException(QStringLiteral("Auto Trace failed: can not overwrite output SWC: %1").arg(m_outputSwcPath));
  }
  if (!QFile::rename(tmpSwcPath, m_outputSwcPath)) {
    throw ZException(QStringLiteral("Auto Trace failed: can not move temp SWC into place.\nTemp: %1\nFinal: %2")
                       .arg(tmpSwcPath, m_outputSwcPath));
  }

  tmpGuard.dismiss();
}

void ZNeutubeAutoTraceProcess::doWork()
{
  m_hasResult = false;

  LOG(INFO) << "Atlas Auto Trace";
  LOG(INFO) << "Selected channel (0-based): " << qulonglongStr(m_selectedChannel);
  LOG(INFO) << "Selected time (0-based): " << qulonglongStr(m_selectedTime);
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

  LOG(INFO) << "Writing SWC...";
  writeSwcAtomicOrThrow(*swc);
  m_hasResult = true;

  LOG(INFO) << "Finished.";
}

void ZNeutubeAutoTraceProcess::read(const json::object& jo)
{
  if (auto it = jo.find("selected_channel"); it != jo.end()) {
    m_selectedChannel = static_cast<size_t>(json::value_to<qulonglong>(it->value()));
  }
  if (auto it = jo.find("selected_time"); it != jo.end()) {
    m_selectedTime = static_cast<size_t>(json::value_to<qulonglong>(it->value()));
  }
  if (auto it = jo.find("trace_config_path"); it != jo.end()) {
    m_traceConfigPath = json::value_to<QString>(it->value());
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
  jo["selected_channel"] = json::value_from(static_cast<qulonglong>(m_selectedChannel));
  jo["selected_time"] = json::value_from(static_cast<qulonglong>(m_selectedTime));
  jo["trace_config_path"] = json::value_from(m_traceConfigPath);
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
