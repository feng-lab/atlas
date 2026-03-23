#include "zapprestartcontroller.h"

#include "z3dmainwindow.h"
#include "z3drenderingengine.h"
#include "zbackgroundtaskmanager.h"
#include "zdoc.h"
#include "zmainwindow.h"
#include "zobjdoc.h"
#include "zscenejsonio.h"
#include "zsysteminfo.h"

#include <QApplication>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QMessageBox>
#include <QPointer>
#include <QProcess>
#include <QPushButton>
#include <QStandardPaths>

namespace nim {
namespace {

bool s_restartShutdownInProgress = false;

[[nodiscard]] std::vector<size_t> unsavedObjectIds(const ZDoc& doc)
{
  std::vector<size_t> ids;
  for (const size_t id : doc.objs()) {
    ZObjDoc* objDoc = doc.idToDoc(id);
    CHECK(objDoc != nullptr);
    if (objDoc->objHasUnsavedChange(id)) {
      ids.push_back(id);
    }
  }
  return ids;
}

[[nodiscard]] size_t activeBackgroundTaskCount(const ZBackgroundTaskManager& manager)
{
  size_t activeTaskCount = 0;
  for (const ZBackgroundTask* task : manager.tasks()) {
    if (task != nullptr && !task->isTerminal()) {
      ++activeTaskCount;
    }
  }
  return activeTaskCount;
}

[[nodiscard]] bool confirmCancelBackgroundTasksForRestart(QWidget* parent, ZBackgroundTaskManager& manager)
{
  const size_t activeTaskCount = activeBackgroundTaskCount(manager);
  if (activeTaskCount == 0) {
    return true;
  }

  QMessageBox box(parent);
  box.setIcon(QMessageBox::Warning);
  box.setWindowTitle(QObject::tr("Cancel Background Tasks?"));
  box.setText(activeTaskCount == 1
                ? QObject::tr("Atlas has 1 background task still in progress.")
                : QObject::tr("Atlas has %1 background tasks still in progress.").arg(activeTaskCount));
  box.setInformativeText(
    QObject::tr("Restarting Atlas will cancel the active tasks before the current instance closes. "
                "Choose Keep Atlas Open to let them continue running."));
  auto* cancelTasksAndRestartButton = box.addButton(QObject::tr("Cancel Tasks and Restart"), QMessageBox::AcceptRole);
  auto* keepAtlasOpenButton = box.addButton(QObject::tr("Keep Atlas Open"), QMessageBox::RejectRole);
  box.setDefaultButton(keepAtlasOpenButton);
  box.setEscapeButton(keepAtlasOpenButton);
  box.exec();

  return box.clickedButton() == cancelTasksAndRestartButton;
}

[[nodiscard]] bool confirmRestartForUnsavedObjects(QWidget* parent, size_t unsavedObjectCount, bool* reopenWorkspace)
{
  CHECK(reopenWorkspace != nullptr);

  QMessageBox box(parent);
  box.setIcon(QMessageBox::Warning);
  box.setWindowTitle(QObject::tr("Unsaved Changes"));
  box.setText(unsavedObjectCount == 1
                ? QObject::tr("Atlas has 1 loaded object with unsaved changes.")
                : QObject::tr("Atlas has %1 loaded objects with unsaved changes.").arg(unsavedObjectCount));
  box.setInformativeText(QObject::tr("Restart can reopen the workspace only after those changes are saved. "
                                     "Choose Restart Without Reopening Workspace to start fresh after restart."));
  auto* saveAndRestartButton = box.addButton(QObject::tr("Save Modified Objects and Restart"), QMessageBox::AcceptRole);
  auto* restartFreshButton =
    box.addButton(QObject::tr("Restart Without Reopening Workspace"), QMessageBox::DestructiveRole);
  auto* cancelButton = box.addButton(QMessageBox::Cancel);
  box.setDefaultButton(saveAndRestartButton);
  box.setEscapeButton(cancelButton);
  box.exec();

  if (box.clickedButton() == saveAndRestartButton) {
    *reopenWorkspace = true;
    return true;
  }
  if (box.clickedButton() == restartFreshButton) {
    *reopenWorkspace = false;
    return true;
  }
  return false;
}

[[nodiscard]] QString restartScenePath()
{
  QString tempRoot = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
  if (tempRoot.trimmed().isEmpty()) {
    tempRoot = ZSystemInfo::configDir().absolutePath();
  }

  QDir dir(tempRoot);
  if (!dir.exists()) {
    (void)dir.mkpath(QStringLiteral("."));
  }

  const QString timestamp = QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMdd-hhmmss-zzz"));
  const QString filename =
    QStringLiteral("atlas-restart-%1-%2.scene").arg(QCoreApplication::applicationPid()).arg(timestamp);
  return dir.filePath(filename);
}

[[nodiscard]] QStringList restartArguments(const QString& snapshotScenePath)
{
  QStringList args;
  const QStringList currentArgs = QCoreApplication::arguments();
  for (int i = 1; i < currentArgs.size(); ++i) {
    const QString& arg = currentArgs.at(i);
    if (arg.startsWith(QStringLiteral("-psn_"))) {
      continue;
    }
    if (arg.startsWith(QLatin1Char('-'))) {
      args.append(arg);
    }
  }
  if (!snapshotScenePath.isEmpty()) {
    args.append(snapshotScenePath);
  }
  return args;
}

[[nodiscard]] bool launchDetachedAtlas(const QStringList& arguments, QString* error)
{
#ifdef Q_OS_MACOS
  const QString bundleName = QStringLiteral("%1.app").arg(QCoreApplication::applicationName());
  const QString bundlePath = QDir(ZSystemInfo::applicationInstallDirPath()).filePath(bundleName);
  if (QFileInfo::exists(bundlePath)) {
    QStringList openArgs;
    openArgs << QStringLiteral("-n") << bundlePath;
    if (!arguments.isEmpty()) {
      openArgs << QStringLiteral("--args");
      openArgs << arguments;
    }
    if (QProcess::startDetached(QStringLiteral("open"), openArgs)) {
      return true;
    }
  }
#endif

  if (QProcess::startDetached(QCoreApplication::applicationFilePath(), arguments)) {
    return true;
  }

  if (error != nullptr) {
    *error = QObject::tr("Could not relaunch %1 with arguments:\n%2")
               .arg(QCoreApplication::applicationFilePath(), arguments.join(QLatin1Char(' ')));
  }
  return false;
}

} // namespace

bool ZAppRestartController::requestRestart(ZMainWindow& mainWindow, QWidget* promptParent)
{
  if (s_restartShutdownInProgress) {
    return false;
  }

  QWidget* parent = promptParent != nullptr ? promptParent : QApplication::activeWindow();
  if (parent == nullptr) {
    parent = &mainWindow;
  }

  ZDoc* doc = mainWindow.doc();
  CHECK(doc != nullptr);

  bool reopenWorkspace = doc->hasObj();
  if (reopenWorkspace) {
    const std::vector<size_t> dirtyObjects = unsavedObjectIds(*doc);
    if (!dirtyObjects.empty()) {
      if (!confirmRestartForUnsavedObjects(parent, dirtyObjects.size(), &reopenWorkspace)) {
        return false;
      }
      if (reopenWorkspace && !doc->saveAllObjs()) {
        return false;
      }
    }
  }

  if (!confirmCancelBackgroundTasksForRestart(parent, doc->backgroundTaskManager())) {
    return false;
  }
  doc->backgroundTaskManager().cancelAllTasksAndWait();

  QString snapshotScenePath;
  if (reopenWorkspace) {
    snapshotScenePath = restartScenePath();
    QString snapshotError;
    Z3DRenderingEngine* engineOrNull = nullptr;
    if (Z3DMainWindow* window3d = mainWindow.get3DWindow()) {
      engineOrNull = window3d->engine();
    }
    if (!ZSceneJsonIO::saveToPath(doc, mainWindow.view(), engineOrNull, snapshotScenePath, snapshotError)) {
      QMessageBox::critical(parent,
                            QApplication::applicationName(),
                            QObject::tr("Could not prepare a restart workspace snapshot:\n%1").arg(snapshotError));
      return false;
    }
  }

  QString launchError;
  if (!launchDetachedAtlas(restartArguments(snapshotScenePath), &launchError)) {
    QMessageBox::critical(parent, QApplication::applicationName(), launchError);
    return false;
  }

  s_restartShutdownInProgress = true;
  QPointer<ZMainWindow> guard(&mainWindow);
  QApplication::closeAllWindows();
  if (guard && guard->isVisible()) {
    s_restartShutdownInProgress = false;
    QMessageBox::warning(
      parent,
      QApplication::applicationName(),
      QObject::tr("Started a new Atlas instance, but the current instance could not close automatically. "
                  "Close it manually if you do not want two Atlas instances running."));
    return false;
  }

  return true;
}

bool ZAppRestartController::isRestartShutdownInProgress()
{
  return s_restartShutdownInProgress;
}

} // namespace nim
