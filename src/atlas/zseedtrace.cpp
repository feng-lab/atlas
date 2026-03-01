#include "zseedtrace.h"

#include "zbackgroundtaskmanager.h"
#include "zcancellation.h"
#include "zdoc.h"
#include "zimg.h"
#include "zimgdoc.h"
#include "zimgpack.h"
#include "zswcdoc.h"
#include "zswcpack.h"
#include "zswcresampler.h"
#include "ztracesettings.h"
#include "zneutubetraceconfig.h"
#include "zneutubetraceinteractive.h"

#include <QApplication>
#include <QDir>
#include <QMessageBox>
#include <QMetaObject>
#include <QPointer>
#include <QUuid>

#include <folly/CancellationToken.h>
#include <folly/coro/Invoke.h>
#include <folly/executors/GlobalExecutor.h>

namespace nim {

namespace {

struct SeededTraceAsyncResult
{
  QString error;
  bool cancelled = false;
  std::shared_ptr<ZSwc> swc;
  size_t newNodes = 0;
};

[[nodiscard]] std::optional<ZSwc::SwcTreeNode> findSelectedBranchRoot(ZSwc& swc)
{
  std::optional<ZSwc::SwcTreeNode> rootOpt;
  for (auto it = swc.begin(); it != swc.end(); ++it) {
    if (!it->selected) {
      continue;
    }

    const auto parent = ZSwc::parent(it);
    if (!ZSwc::isNull(parent) && parent->selected) {
      continue;
    }

    if (rootOpt.has_value()) {
      LOG(WARNING) << "Seed trace: multiple selected branch roots found; skipping resample.";
      return std::nullopt;
    }
    rootOpt = it;
  }

  return rootOpt;
}

[[nodiscard]] bool resampleSelectedBranchInPlace(ZSwc& swc, size_t* outNewNodes)
{
  CHECK(outNewNodes != nullptr);
  *outNewNodes = 0;

  std::optional<ZSwc::SwcTreeNode> branchRootOpt = findSelectedBranchRoot(swc);
  if (!branchRootOpt.has_value()) {
    return false;
  }

  ZSwc::SwcTreeNode branchRoot = *branchRootOpt;
  const ZSwc::SwcTreeNode attachParent = ZSwc::parent(branchRoot);

  std::vector<SwcNode> chainNodes;
  chainNodes.reserve(128);

  ZSwc::SwcTreeNode cur = branchRoot;
  while (!ZSwc::isNull(cur) && cur->selected) {
    if (ZSwc::isBranchNode(cur)) {
      LOG(WARNING) << "Seed trace: selected branch contains a branch-point; skipping resample.";
      return false;
    }

    chainNodes.push_back(*cur);

    const auto child = ZSwc::firstChild(cur);
    if (ZSwc::isNull(child) || !child->selected) {
      break;
    }
    cur = child;
  }

  if (chainNodes.size() <= 1) {
    return false;
  }

  ZSwc branch;
  ZSwc::SwcTreeNode branchIt = branch.appendRoot(chainNodes.front());
  branchIt->selected = true;
  for (size_t i = 1; i < chainNodes.size(); ++i) {
    branchIt = branch.appendChild(branchIt, chainNodes[i]);
    branchIt->selected = true;
  }

  ZNeutubeSwcResampler resampler;
  resampler.optimalDownsample(branch);
  if (branch.size() <= 1) {
    return false;
  }

  std::vector<SwcNode> resampledNodes;
  resampledNodes.reserve(branch.size());
  for (auto it = branch.begin(); it != branch.end(); ++it) {
    if (ZSwc::isBranchNode(it)) {
      LOG(WARNING) << "Seed trace: resampler produced a branch-point unexpectedly; skipping resample.";
      return false;
    }
    resampledNodes.push_back(*it);
  }

  swc.eraseSubtree(branchRoot);

  ZSwc::SwcTreeNode inserted = swc.end();
  for (size_t i = 0; i < resampledNodes.size(); ++i) {
    SwcNode node = resampledNodes[i];
    node.selected = true;
    if (i == 0) {
      inserted = ZSwc::isNull(attachParent) ? swc.appendRoot(node) : swc.appendChild(attachParent, node);
    } else {
      inserted = swc.appendChild(inserted, node);
    }
  }

  *outNewNodes = resampledNodes.size();
  return true;
}

[[nodiscard]] SeededTraceAsyncResult runSeedTraceLegacyLike(const std::shared_ptr<ZImgPack>& imgPack,
                                                            size_t sc,
                                                            size_t t,
                                                            const std::array<double, 3>& seed,
                                                            const QString& traceConfigPath,
                                                            bool haveAlgoConfig,
                                                            const ZTraceSettings::AlgoConfig& algoCfg,
                                                            std::optional<std::pair<size_t, ZSwc>> hostSwcOpt,
                                                            folly::CancellationToken cancellationToken)
{
  SeededTraceAsyncResult res;

  try {
    maybeCancel(cancellationToken);

    const ZImgInfo info = imgPack->imgInfo();
    if (sc >= info.numChannels || t >= info.numTimes) {
      res.error = QStringLiteral("Trace failed: invalid channel/time selection (c=%1, t=%2).")
                    .arg(static_cast<qulonglong>(sc))
                    .arg(static_cast<qulonglong>(t));
      return res;
    }

    const ZImg* signalPtr = nullptr;
    ZImg ownedSignal;
    if (imgPack->isDiskCached()) {
      ownedSignal = imgPack->wholeImg();
      signalPtr = &ownedSignal;
    } else {
      signalPtr = &imgPack->img();
    }
    CHECK(signalPtr != nullptr);
    const ZImg& signal = *signalPtr;

    if (signal.isEmpty()) {
      res.error = QStringLiteral("Trace failed: the image is empty.");
      return res;
    }

    TraceConfig cfg;
    if (!traceConfigPath.isEmpty()) {
      const bool ok = loadTraceConfigLegacyLike(traceConfigPath.toStdString(), cfg);
      if (!ok) {
        cfg = TraceConfig{};
      }
    } else {
      cfg = TraceConfig{};
    }

    if (haveAlgoConfig) {
      cfg.minAutoScore = algoCfg.minAutoScore;
      cfg.minManualScore = algoCfg.minManualScore;
      cfg.minSeedScore = algoCfg.minSeedScore;
      cfg.min2dScore = algoCfg.min2dScore;
      cfg.refit = algoCfg.refit;
      cfg.spTest = algoCfg.spTest;
      cfg.crossoverTest = algoCfg.crossoverTest;
      cfg.tuneEnd = algoCfg.tuneEnd;
      cfg.edgePath = algoCfg.edgePath;
      cfg.enhanceMask = algoCfg.enhanceMask;
      cfg.seedMethod = algoCfg.seedMethod;
      cfg.recover = algoCfg.recover;
      cfg.chainScreenCount = algoCfg.chainScreenCount;
      cfg.maxEucDist = algoCfg.maxEucDist;
    }

    maybeCancel(cancellationToken);
    if (hostSwcOpt.has_value()) {
      SeedTraceResult tr =
        traceSeedIntoHostSwcLegacyLike(signal, hostSwcOpt->second, seed, cfg, sc, t, cancellationToken);
      if (tr.swc) {
        size_t resampledNewNodes = tr.newNodes;
        size_t tmpNewNodes = 0;
        if (resampleSelectedBranchInPlace(*tr.swc, &tmpNewNodes)) {
          resampledNewNodes = tmpNewNodes;
        }
        res.swc = std::shared_ptr<ZSwc>(tr.swc.release());
        res.newNodes = resampledNewNodes;
      }
    } else {
      SeedTraceResult tr = traceSeedNewSwcLegacyLike(signal, seed, cfg, sc, t, cancellationToken);
      if (tr.swc) {
        ZNeutubeSwcResampler resampler;
        resampler.optimalDownsample(*tr.swc);
        res.swc = std::shared_ptr<ZSwc>(tr.swc.release());
        res.newNodes = res.swc ? res.swc->size() : 0;
      }
    }
  }
  catch (const ZCancellationException&) {
    res.cancelled = true;
  }
  catch (const std::exception& e) {
    res.error = QString("Trace failed: %1").arg(QString::fromUtf8(e.what()));
  }

  return res;
}

} // namespace

void startSeedTraceInteractive(ZDoc& doc,
                               const QString& actionName,
                               size_t sourceImgObjId,
                               const std::shared_ptr<ZImgPack>& imgPack,
                               size_t sc,
                               size_t t,
                               std::array<double, 3> seed,
                               const QString& traceConfigPath,
                               std::optional<std::pair<size_t, ZSwc>> hostSwcOpt,
                               bool promoteNewSwcToExistingTarget,
                               std::function<void(size_t newSwcId)> onNewSwcCreated)
{
  CHECK(imgPack);

  ZTraceSettings& traceSettings = doc.traceSettings();
  if (traceSettings.traceInProgress()) {
    QMessageBox::information(QApplication::activeWindow(),
                             QApplication::applicationName(),
                             QStringLiteral("A trace is already running. Please wait for it to finish."));
    return;
  }
  traceSettings.setTraceInProgress(true);

  const bool haveAlgoConfig = traceSettings.algoConfigInitialized();
  const ZTraceSettings::AlgoConfig algoCfg = traceSettings.algoConfig();

  const ZImgInfo info = imgPack->imgInfo();
  const QString channelLabel = (sc < info.channelNames.size())
                                 ? info.displayChannelName(sc)
                                 : QStringLiteral("Ch%1").arg(static_cast<qulonglong>(sc + 1));
  const QString timeLabel = QStringLiteral("T%1").arg(static_cast<qulonglong>(t + 1));

  const QString swcTargetLabel =
    hostSwcOpt.has_value() ? doc.objNameWithModifiedMarkerAndID(hostSwcOpt->first) : QStringLiteral("new SWC");

  const QString seedLabel = QStringLiteral("(%1, %2, %3)")
                              .arg(QString::number(seed[0], 'f', 1))
                              .arg(QString::number(seed[1], 'f', 1))
                              .arg(QString::number(seed[2], 'f', 1));

  ZBackgroundTaskManager& tm = doc.backgroundTaskManager();
  auto cancellationSource = std::make_shared<folly::CancellationSource>();
  const folly::CancellationToken cancellationToken = cancellationSource->getToken();
  ZBackgroundTaskManager::TaskOptions taskOptions;
  taskOptions.useFakeProgress = true;
  taskOptions.cancelCallback = [cancellationSource]() {
    cancellationSource->requestCancellation();
  };

  const QString taskTitle = QStringLiteral("Seed Trace: %1, %2, %3, seed=%4 -> %5")
                              .arg(doc.objNameWithModifiedMarkerAndID(sourceImgObjId))
                              .arg(channelLabel)
                              .arg(timeLabel)
                              .arg(seedLabel)
                              .arg(swcTargetLabel);

  auto* task = tm.createTask(taskTitle, std::move(taskOptions));
  tm.startTask(task, QStringLiteral("running"));
  doc.showBackgroundTasksPanel();

  const std::optional<size_t> hostSwcId =
    hostSwcOpt.has_value() ? std::optional<size_t>(hostSwcOpt->first) : std::optional<size_t>();

  QPointer<ZDoc> docPtr(&doc);
  QPointer<ZBackgroundTask> taskPtr(task);

  tm.spawnDetachedTask(
    folly::getGlobalCPUExecutor(),
    folly::coro::co_invoke([docPtr,
                            taskPtr,
                            imgPack,
                            actionName,
                            sourceImgObjId,
                            sc,
                            t,
                            seed,
                            traceConfigPath,
                            haveAlgoConfig,
                            algoCfg,
                            hostSwcOpt = std::move(hostSwcOpt),
                            promoteNewSwcToExistingTarget,
                            hostSwcId,
                            onNewSwcCreated = std::move(onNewSwcCreated),
                            cancellationToken]() mutable -> folly::coro::Task<void> {
      const SeededTraceAsyncResult res = runSeedTraceLegacyLike(imgPack,
                                                                sc,
                                                                t,
                                                                seed,
                                                                traceConfigPath,
                                                                haveAlgoConfig,
                                                                algoCfg,
                                                                std::move(hostSwcOpt),
                                                                cancellationToken);

      if (docPtr == nullptr) {
        co_return;
      }

      QMetaObject::invokeMethod(
        docPtr,
        [docPtr,
         taskPtr,
         actionName,
         sourceImgObjId,
         sc,
         promoteNewSwcToExistingTarget,
         hostSwcId,
         res,
         onNewSwcCreated = std::move(onNewSwcCreated)]() mutable {
          if (docPtr == nullptr) {
            return;
          }

          ZDoc& doc = *docPtr;
          ZTraceSettings& traceSettings = doc.traceSettings();
          traceSettings.setTraceInProgress(false);

          if (taskPtr == nullptr) {
            return;
          }

          ZBackgroundTaskManager& tm = doc.backgroundTaskManager();

          if (res.cancelled) {
            tm.cancelTask(taskPtr, QStringLiteral("cancelled"));
            return;
          }

          if (!res.error.isEmpty()) {
            tm.failTask(taskPtr, res.error);
            QMessageBox::information(QApplication::activeWindow(), QApplication::applicationName(), res.error);
            return;
          }

          if (!res.swc) {
            tm.succeedTask(taskPtr, QStringLiteral("no trace result"));
            QMessageBox::information(QApplication::activeWindow(),
                                     QApplication::applicationName(),
                                     QStringLiteral("No trace result was produced."));
            return;
          }

          if (hostSwcId.has_value()) {
            const size_t swcId = *hostSwcId;
            ZSwcDoc& swcDoc = doc.swcDoc();
            if (!swcDoc.hasObjWithID(swcId)) {
              tm.cancelTask(taskPtr, QStringLiteral("target SWC no longer exists"));
              return;
            }
            if (res.newNodes == 0) {
              tm.succeedTask(taskPtr, QStringLiteral("no branch traced"));
              QMessageBox::information(QApplication::activeWindow(),
                                       QApplication::applicationName(),
                                       QStringLiteral("No branch was traced at this seed location."));
              return;
            }
            ZSwcPack& pack = swcDoc.swcPack(swcId);
            pack.replaceSwcWithUndo(actionName, std::move(*res.swc));
            tm.succeedTask(taskPtr, QStringLiteral("updated SWC #%1").arg(static_cast<qulonglong>(swcId)));
            return;
          }

          if (res.newNodes == 0) {
            tm.succeedTask(taskPtr, QStringLiteral("no branch traced"));
            QMessageBox::information(QApplication::activeWindow(),
                                     QApplication::applicationName(),
                                     QStringLiteral("No branch was traced at this seed location."));
            return;
          }

          ZSwcDoc& swcDoc = doc.swcDoc();
          const QString displayPath = QDir::temp().absoluteFilePath(
            QStringLiteral("AtlasTrace_%1.swc").arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
          const size_t newSwcId = swcDoc.addSwcFromMemory(std::move(*res.swc), displayPath);
          if (newSwcId == 0) {
            tm.failTask(taskPtr, QStringLiteral("Failed to create new SWC."));
            return;
          }

          // neuTube UX parity: newly traced SWCs should not start with every node selected.
          swcDoc.swcPack(newSwcId).setSelectedNodes({});

          tm.succeedTask(taskPtr, QStringLiteral("created SWC #%1").arg(static_cast<qulonglong>(newSwcId)));

          if (onNewSwcCreated) {
            try {
              onNewSwcCreated(newSwcId);
            }
            catch (const std::exception& e) {
              LOG(WARNING) << "Seed trace: onNewSwcCreated() threw: " << e.what();
            }
          }

          if (promoteNewSwcToExistingTarget) {
            traceSettings.promoteNewSwcTargetToExistingIfStillNew(sourceImgObjId, sc, newSwcId);
          }
        },
        Qt::QueuedConnection);

      co_return;
    }),
    "Seed Trace");
}

} // namespace nim
