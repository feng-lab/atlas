#include "zmainwindow.h"

#include "zsysteminfo.h"
#include "zdoc.h"
#include "zview.h"

#if ATLAS_ENABLE_CUSTOM_COMMAND
#include "zcustomcommand.h"
#endif
#include "zviewsettingwidget.h"
#include "z3drenderingengine.h"
#include "z3dmainwindow.h"
#include "zobjwidget.h"
#include "zobjeditwidget.h"
#include "zobjdetailedinfowidget.h"
#include "zimgview.h"
#include "zpunctaview.h"
#include "zswcview.h"
#include "zskeletonview.h"
#include "zmeshdoc.h"
#include "z2danimationdoc.h"
#include "z3danimationdoc.h"
#include "zroiview.h"
#include "zregionannotationview.h"
#include "zsvgview.h"
#include "zjson.h"
#include "zscenejsonio.h"
#include "zfileutils.h"
#include "zmessageboxhelpers.h"
#include "zmarkdownbrowser.h"
#include "ztheme.h"
#include "ztracesettings.h"
#include "ztracesettingswidget.h"
#include "zapprestartcontroller.h"
#include "zbackgroundtaskmanagerwidget.h"
#include "zdiskcacheutils.h"
#include "zflagsettingsdialog.h"

#include "zcommandlineflags.h"

#include <QFileDialog>
#include <QMessageBox>
#include <QAction>
#include <QActionGroup>
#include <QToolBar>
#include <QMenu>
#include <QSettings>
#include <QApplication>
#include <QDockWidget>
#include <QMimeData>
#include <QMenuBar>
#include <QToolButton>
#include <QStatusBar>
#include <QDesktopServices>
#include <QDialog>
#include <QVBoxLayout>
#include <QFile>
#include <QTextStream>
#include <QFileInfo>
#include <QDir>
#include <QCoreApplication>
#include <QProcess>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSignalSpy>
#include <QTimer>
#include <utility>
#include <memory>

ABSL_FLAG(bool,
          atlas_block_scene_3d_apply,
          false,
          "If true, block scene loading until all 3D settings have been applied by the rendering engine");

namespace nim {

namespace {

constexpr auto kShortcutsDocRef = "USER_GUIDE.md#121-keyboard-and-mouse-shortcuts";

} // namespace

ZMainWindow::ZMainWindow(QString versionStr)
  : m_versionString(std::move(versionStr))
{
  init();
  setWindowTitle(QString("Atlas version %1").arg(m_versionString));
  // setCurrentFile("");
}

void ZMainWindow::updateRecentFileActions()
{
  QSettings settings;
  QStringList files = settings.value("recentFileList").toStringList();

  auto numRecentFiles = std::min(size_t(files.size()), m_recentFileActions.size());

  size_t idx = 0;
  for (size_t i = 0; i < numRecentFiles; ++i) {
    if (QFile::exists(files[i])) {
      QString text = QString("&%1 %2").arg(i + 1).arg(strippedName(files[i]));
      m_recentFileActions[idx]->setText(text);
      m_recentFileActions[idx]->setData(files[i]);
      m_recentFileActions[idx]->setToolTip(files[i]);
      m_recentFileActions[idx]->setStatusTip(files[i]);
      m_recentFileActions[idx++]->setVisible(true);
    }
  }
  for (size_t j = idx; j < m_recentFileActions.size(); ++j) {
    m_recentFileActions[j]->setVisible(false);
  }

  m_separatorAction->setVisible(idx > 0);
}

void ZMainWindow::openEditWidget(size_t id)
{
  ZObjDoc* doc = m_doc->idToDoc(id);
  if (doc->objUndoStack(id)) {
    doc->objUndoStack(id)->setActive();
  } else {
    m_doc->activateEmptyUndoStack();
  }
  if (!doc->typeName().contains("3D", Qt::CaseInsensitive)) {
    if (m_objEditWidget->showObjEditWidgetOfObj(id)) {
      m_editObjDockWidget->setVisible(true);
    }
  } else {
    // m_editObjDockWidget->setVisible(false);
    if (!m_3dWindow) {
      open3DWindow();
    }
    if (m_3dWindow) {
      m_3dWindow->openEditWidget(id);
    }
  }
}

void ZMainWindow::loadUrls(const QList<QUrl>& urlList)
{
  QStringList fileList;
  for (const auto& url : urlList) {
    // load files inside if is folder
    QFileInfo dirCheck(url.toLocalFile());
    if (dirCheck.isDir()) {
      QDir dir = dirCheck.absoluteDir();
      QFileInfoList list = dir.entryInfoList(QDir::Files | QDir::NoSymLinks);
      for (const auto& fi : list) {
        fileList.append(fi.canonicalFilePath());
      }
    } else {
      fileList.append(dirCheck.canonicalFilePath());
    }
  }
  QStringList::iterator it = fileList.begin();
  while (it != fileList.end()) {
    if (it->endsWith(".scene", Qt::CaseInsensitive)) {
      loadJsonScene(*it);
      it = fileList.erase(it);
    } else {
      ++it;
    }
  }
  if (!fileList.isEmpty()) {
    m_doc->loadFileList(fileList);
  }
}

void ZMainWindow::loadFileList(const QStringList& fileList)
{
  if (!fileList.isEmpty()) {
    m_doc->loadFileList(fileList);
  }
}

void ZMainWindow::removeAllObjs()
{
  m_doc->removeAllObjs();
}

Z3DMainWindow* ZMainWindow::get3DWindow()
{
  return m_3dWindow.data();
}

ZView* ZMainWindow::view()
{
  return m_view.get();
}

ZDoc* ZMainWindow::doc()
{
  return m_doc.get();
}

void ZMainWindow::ensure3DWindow()
{
  if (!m_3dWindow) {
    open3DWindow();
  } else {
    // Bring existing window to front to ensure engine stays active
    m_3dWindow->showNormal();
    m_3dWindow->raise();
    m_3dWindow->activateWindow();
  }
}

void ZMainWindow::checkForUpdates()
{
  QString updaterName("MaintenanceTool");
#ifdef Q_OS_MACOS
  QString program = ZSystemInfo::applicationInstallDirPath() + QString("/%1.app/Contents/MacOS/%1").arg(updaterName);
#elif defined(Q_OS_WIN64)
  QString program = ZSystemInfo::applicationInstallDirPath() + QString("/%1.exe").arg(updaterName);
#else
  QString program = ZSystemInfo::applicationInstallDirPath() + QString("/%1").arg(updaterName);
#endif
  if (QFileInfo(program).exists()) {
    QStringList arguments;
    arguments << "--updater";
    LOG(INFO) << program << " " << arguments.join(" ");
    QProcess::startDetached(program, arguments);
  } else {
    QMessageBox::critical(QApplication::activeWindow(),
                          QString("Could not find updater"),
                          QString("Path to MaintenanceTool could not be determined or does not exist."));
  }
}

void ZMainWindow::closeEvent(QCloseEvent* event)
{
  // Qt mac bug, use dock icon context menu -> quit will call this function twice and crash
  if (m_isClosed) {
    event->accept();
    return;
  }

  if (!ZAppRestartController::isRestartShutdownInProgress() && m_doc != nullptr && !m_doc->canClose(this)) {
    event->ignore();
    return;
  }

  delete m_3dWindow.data();
  m_3dWindow.clear();

  delete m_editObjDockWidget;
  writeSettings();
  event->accept();
  // otherwise it is very slow to close the application
  m_view.reset();
  m_doc.reset();
  m_isClosed = true;
}

void ZMainWindow::dragEnterEvent(QDragEnterEvent* event)
{
  if (event->mimeData()->hasFormat("text/uri-list")) {
    event->acceptProposedAction();
  }
}

void ZMainWindow::dropEvent(QDropEvent* event)
{
  loadUrls(event->mimeData()->urls());
}

void ZMainWindow::open() {}

bool ZMainWindow::save()
{
  return m_doc->saveAllObjs();
}

bool ZMainWindow::saveAs()
{
  return m_doc->saveSelectedObjsAs();
}

void ZMainWindow::openRecentFile()
{
  if (auto action = qobject_cast<QAction*>(sender())) {
    QString fn = action->data().toString();
    if (fn.endsWith(".scene", Qt::CaseInsensitive)) {
      loadJsonScene(fn);
    } else {
      m_doc->loadFile(action->data().toString());
    }
  }
}

void ZMainWindow::about()
{
  const QString apacheLicenseUrl = QStringLiteral("http://www.apache.org/licenses/LICENSE-2.0");
  QMessageBox::about(QApplication::activeWindow(),
                     tr("About Atlas"),
                     tr("<p>Atlas version %1</p>"
                        "<p>"
                        "Atlas<br/>"
                        "<br/>"
                        "Copyright (c) 2011 Linqing Feng and contributors &lt;fenglinqing@gmail.com&gt;<br/>"
                        "<br/>"
                        "Licensed under the Apache License, Version 2.0 (the \"License\");<br/>"
                        "you may not use this work except in compliance with the License.<br/>"
                        "You may obtain a copy of the License in the file LICENSE at the root of this repository<br/>"
                        "or at <a href=\"%2\">%2</a><br/>"
                        "<br/>"
                        "This repository contains third-party components under their respective licenses."
                        "</p>")
                       .arg(m_versionString, apacheLicenseUrl));
}

#ifdef Q_OS_LINUX
void ZMainWindow::createDesktopEntry()
{
  QFile file(QString("%1/.local/share/applications/atlas.desktop").arg(QDir::homePath()));
  if (file.open(QIODevice::WriteOnly | QIODevice::Text)) {
    QTextStream out(&file);
    out << "[Desktop Entry]\n";
    out << "Encoding=UTF-8\n";
    out << "Version=1.0\n";
    out << "Type=Application\n";
    out << "Name=Atlas\n";
    out << "Exec=" << QCoreApplication::applicationDirPath() << "/Atlas %F\n";
    out << "Icon=" << QCoreApplication::applicationDirPath() << "/Atlas.png\n";
    out << "Comment=Iamge Analysis\n";
    out << "Categories=Graphics;Science;Utility;\n";
    out << "Terminal=false\n";
    out << "StartupWMClass=Atlas\n";

    QMessageBox::information(this, QString("done"), QString("Desktop entry created."));
    return;
  }

  QMessageBox::critical(this, QString("error"), QString("Can not create desktop entry."));
}
#endif

void ZMainWindow::activateWindowIfNot()
{
  if (!isActiveWindow()) {
    this->activateWindow();
  }
}

void ZMainWindow::openScreenshotPanel()
{
  if (m_captureDockWidget->isHidden()) {
    m_captureDockWidget->show();
  }
  m_captureDockWidget->raise();
}

void ZMainWindow::openShortcutsReference()
{
  openDocMd(QString::fromLatin1(kShortcutsDocRef));
}

void ZMainWindow::openDocMd(const QString& name)
{
#ifdef Q_OS_MAC
  const QString docsRoot = QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("../Resources/docs/"));
#else
  const QString docsRoot = QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("Resources/docs/"));
#endif
  QWidget* parent = QApplication::activeWindow() ? static_cast<QWidget*>(QApplication::activeWindow()) : this;
  // Support optional fragment in name (e.g., DEVELOPER_GUIDE.md#section)
  QString fileName = name;
  QString frag;
  const int hashIdx = name.indexOf('#');
  if (hashIdx > 0) {
    fileName = name.left(hashIdx);
    frag = name.mid(hashIdx + 1);
  }
  const QString path = QDir(docsRoot).filePath(fileName);
  if (!QFileInfo::exists(path)) {
    QMessageBox::critical(parent, tr("Open documentation"), tr("Could not open %1").arg(path));
    return;
  }
  auto* dlg = new QDialog(parent);
  dlg->setAttribute(Qt::WA_DeleteOnClose);
  dlg->setWindowTitle(name);
  auto* layout = new QVBoxLayout(dlg);
  auto* view = new ZMarkdownBrowser(dlg);
  view->setSearchPaths(QStringList { docsRoot });
  // Back/Forward toolbar wired to browser history
  auto* tb = new QToolBar(dlg);
  QAction* actBack = tb->addAction(tr("Back"));
  actBack->setEnabled(false);
  QObject::connect(actBack, &QAction::triggered, view, &ZMarkdownBrowser::goBack);
  QAction* actFwd = tb->addAction(tr("Forward"));
  actFwd->setEnabled(false);
  QObject::connect(actFwd, &QAction::triggered, view, &ZMarkdownBrowser::goForward);
  QObject::connect(view, &QTextBrowser::backwardAvailable, actBack, &QAction::setEnabled);
  QObject::connect(view, &QTextBrowser::forwardAvailable, actFwd, &QAction::setEnabled);
  view->setOpenExternalLinks(true);
  QUrl start = QUrl::fromLocalFile(path);
  if (!frag.isEmpty()) {
    start.setFragment(frag);
  }
  // Home action to return to the starting document
  QAction* actHome = tb->addAction(tr("Home"));
  QObject::connect(actHome, &QAction::triggered, view, [view, start]() {
    view->navigateTo(start);
  });

  layout->addWidget(tb);
  layout->addWidget(view);
  dlg->resize(900, 700);
  dlg->show();

  // Delay the initial navigation until the dialog is shown so the view has a
  // window/screen (for correct HiDPI SVG rasterization).
  QTimer::singleShot(0, dlg, [view, start]() {
    view->navigateTo(start);
  });
}

void ZMainWindow::raiseViewSettingDockWidget()
{
  if (m_viewSettingDockWidget->isHidden()) {
    m_viewSettingDockWidget->show();
  }
  m_viewSettingDockWidget->raise();
}

void ZMainWindow::raiseGlobalSettingDockWidget()
{
  if (m_globalSettingDockWidget->isHidden()) {
    m_globalSettingDockWidget->show();
  }
  m_globalSettingDockWidget->raise();
}

void ZMainWindow::openLogFolder()
{
  QDesktopServices::openUrl(QUrl::fromLocalFile(ZSystemInfo::logDir().absolutePath()));
}

void ZMainWindow::openDiskCacheFolder()
{
  const QString cacheDir = atlasDiskCacheDirFromFlags();

  // Best-effort: create the directory so users can see where it will live even before the first download.
  if (!QDir(cacheDir).exists()) {
    QDir mk;
    (void)mk.mkpath(cacheDir);
  }

  QDesktopServices::openUrl(QUrl::fromLocalFile(cacheDir));
}

void ZMainWindow::openSettingsDialog()
{
  QWidget* dialogParent = QApplication::activeWindow();
  if (dialogParent == nullptr || !dialogParent->isWindow()) {
    dialogParent = this;
  }

  auto showRestartRequiredMessage = [this, dialogParent]() {
    if (auto* statusWindow = qobject_cast<QMainWindow*>(dialogParent)) {
      statusWindow->statusBar()->showMessage(tr("Settings saved. Restart Atlas to apply changes."), 6000);
    } else {
      statusBar()->showMessage(tr("Settings saved. Restart Atlas to apply changes."), 6000);
    }
  };

  ZFlagSettingsDialog dlg(dialogParent);
  if (dlg.exec() == QDialog::Accepted) {
    if (dlg.restartRequested()) {
      QTimer::singleShot(0, this, [this, dialogParent]() {
        if (!ZAppRestartController::requestRestart(*this, dialogParent)) {
          if (auto* statusWindow = qobject_cast<QMainWindow*>(dialogParent)) {
            statusWindow->statusBar()->showMessage(tr("Settings saved. Restart Atlas to apply changes."), 6000);
          } else {
            statusBar()->showMessage(tr("Settings saved. Restart Atlas to apply changes."), 6000);
          }
        }
      });
      return;
    }
    showRestartRequiredMessage();
  }
}

void ZMainWindow::runCustomCommand()
{
#if ATLAS_ENABLE_CUSTOM_COMMAND
  ZCustomCommand::run();
#endif
}

void ZMainWindow::open3DWindow()
{
  try {
    if (!m_3dWindow) {
      m_3dWindow = new Z3DMainWindow(*m_doc, *this, false);
      connect(m_3dWindow, &Z3DMainWindow::renderingEngineInitialized, this, &ZMainWindow::window3DReady);
      m_3dWindow->setWindowTitle(QString("3D View  %1").arg(windowTitle()));
      connect(m_3dWindow, &Z3DMainWindow::loadScene, this, &ZMainWindow::loadScene);
      connect(m_3dWindow, &Z3DMainWindow::saveScene, this, &ZMainWindow::saveScene);
      connect(m_3dWindow, &Z3DMainWindow::loadJsonScene, this, &ZMainWindow::loadJsonScene);
      connect(m_3dWindow, &Z3DMainWindow::viewReady, &m_doc->animation3DDoc(), &Z3DAnimationDoc::bindView);
    }

    m_3dWindow->showNormal();
    m_3dWindow->raise();
    m_3dWindow->activateWindow();
    // m_3dWindow->setWindowState(Qt::WindowActive);
  }
  catch (const ZException& e) {
    LOG(ERROR) << "Failed to open 3D window: " << e.what();
    QMessageBox::critical(this,
                          QApplication::applicationName(),
                          QString("Failed to open 3D window:\n%1").arg(e.what()));
    delete m_3dWindow.data();
    m_3dWindow.clear();
  }
}

void ZMainWindow::loadScene()
{
  QString fn = QFileDialog::getOpenFileName(QApplication::activeWindow(),
                                            "Load scene",
                                            ZDoc::lastOpenedFilePath(),
                                            tr("Scene file (*.scene)"));
  if (!fn.isEmpty()) {
    QString err;
    if (!loadJsonSceneImpl(fn, err)) {
      showCriticalWithDetails(QApplication::activeWindow(), tr("Can not load scene %1").arg(fn), err);
    } else {
      m_doc->setLastOpenedFilePath(fn);
      ZSystemInfo::instance().addFileToRecentFileList(fn);
    }
  }
}

void ZMainWindow::saveScene()
{
  if (!m_doc->saveAllObjs()) {
    QMessageBox::critical(QApplication::activeWindow(),
                          QApplication::applicationName(),
                          tr("Can not save scene because some objects have unsaved changes"));
    return;
  }

  QString fn = ZFileUtils::getSaveFileName(QApplication::activeWindow(),
                                           "Save scene to file",
                                           ZDoc::lastOpenedFilePath(),
                                           tr("Scene file (*.scene)"));
  if (!fn.isEmpty()) {
    QString err;
    if (!saveJsonSceneImpl(fn, err)) {
      showCriticalWithDetails(QApplication::activeWindow(), tr("Can not save scene %1").arg(fn), err);
    } else {
      m_doc->setLastOpenedFilePath(fn);
      ZSystemInfo::instance().addFileToRecentFileList(fn);
      QMessageBox::information(QApplication::activeWindow(),
                               QApplication::applicationName(),
                               QString("scene saved as %1").arg(fn),
                               QMessageBox::Ok);
    }
  }
}

void ZMainWindow::loadJsonScene(const QString& fn)
{
  QString err;
  if (!loadJsonSceneImpl(fn, err)) {
    showCriticalWithDetails(QApplication::activeWindow(), tr("Can not load scene %1").arg(fn), err);
  } else {
    if (!err.isEmpty()) {
      showCriticalWithDetails(QApplication::activeWindow(), tr("Error while loading scene %1").arg(fn), err);
    }
    ZSystemInfo::instance().addFileToRecentFileList(fn);
  }
}

void ZMainWindow::openNewInstance()
{
#ifdef Q_OS_MACOS
  QDir dir = QDir(QCoreApplication::applicationDirPath());
  dir.cdUp();
  dir.cdUp();
  QProcess process;
  process.start("open", QStringList() << "-n" << dir.absolutePath());
  process.waitForFinished();
#endif
}

void ZMainWindow::init()
{
  setAttribute(Qt::WA_DeleteOnClose, true);
  setAcceptDrops(true);

  m_doc = std::make_unique<ZDoc>(this);
  m_view = std::make_unique<ZView>(*m_doc, this);

  // packages
  m_view->registerObjView(std::make_unique<ZImgView>(m_doc->imgDoc(), *m_view));
  m_view->registerObjView(std::make_unique<ZROIView>(m_doc->roiDoc(), *m_view));
  m_view->registerObjView(std::make_unique<ZPunctaView>(m_doc->punctaDoc(), *m_view));
  m_view->registerObjView(std::make_unique<ZSwcView>(m_doc->swcDoc(), *m_view));
  m_view->registerObjView(std::make_unique<ZSkeletonView>(m_doc->skeletonDoc(), *m_view));
  m_view->registerObjView(std::make_unique<ZRegionAnnotationView>(m_doc->regionAnnotationDoc(), *m_view));
  m_view->registerObjView(std::make_unique<ZSvgView>(m_doc->svgDoc(), *m_view));

  // UI
  setCentralWidget(m_view.get());

  m_doc->animation2DDoc().bindView(m_view.get());

  createActions();
  createMenus();
  createToolBars();
  createStatusBar();
  createDockWindows();

  readSettings();
}

void ZMainWindow::createActions()
{
  // file
  //  m_newAction = new QAction(ZTheme::instance().icon(ZTheme::FileIcon), tr("&New"), this);
  //  m_newAction->setShortcuts(QKeySequence::New);
  //  m_newAction->setStatusTip(tr("Open a new window"));
  //  connect(m_newAction, &QAction::triggered, this, &ZMainWindow::newWindow);

  m_openAction = new QAction(ZTheme::instance().icon(ZTheme::OpenFolderIcon), tr("&Open..."), this);
  ZTheme::instance().bindIcon(m_openAction, ZTheme::OpenFolderIcon);
  m_openAction->setShortcuts(QKeySequence::Open);
  m_openAction->setStatusTip(tr("Open an existing scene file"));
  connect(m_openAction, &QAction::triggered, this, &ZMainWindow::loadScene);

  m_saveAction = new QAction(ZTheme::instance().icon(ZTheme::SaveIcon), tr("&Save"), this);
  ZTheme::instance().bindIcon(m_saveAction, ZTheme::SaveIcon);
  m_saveAction->setShortcuts(QKeySequence::Save);
  m_saveAction->setStatusTip(tr("Save unsaved objects to disk"));
  connect(m_saveAction, &QAction::triggered, this, &ZMainWindow::save);

  m_saveAsAction = new QAction(ZTheme::instance().icon(ZTheme::SaveAsIcon), tr("Save &As..."), this);
  ZTheme::instance().bindIcon(m_saveAsAction, ZTheme::SaveAsIcon);
  m_saveAsAction->setShortcuts(QKeySequence::SaveAs);
  m_saveAsAction->setStatusTip(tr("Save selected objects under a new name"));
  connect(m_saveAsAction, &QAction::triggered, this, &ZMainWindow::saveAs);

  m_loadSceneAction = new QAction(tr("Load &Scene..."), this);
  m_loadSceneAction->setStatusTip(tr("Load scene"));
  connect(m_loadSceneAction, &QAction::triggered, this, &ZMainWindow::loadScene);

  m_saveSceneAction = new QAction(tr("Save &Scene..."), this);
  m_saveSceneAction->setStatusTip(tr("Save scene"));
  connect(m_saveSceneAction, &QAction::triggered, this, &ZMainWindow::saveScene);

  m_closeAction = new QAction(tr("&Close"), this);
  m_closeAction->setShortcut(QKeySequence::Close);
  m_closeAction->setStatusTip(tr("Close this window"));
  connect(m_closeAction, &QAction::triggered, this, &ZMainWindow::close);

  for (int i = 0; i < ZSystemInfo::instance().maxNumRecentFiles(); ++i) {
    m_recentFileActions.push_back(new QAction(this));
    m_recentFileActions[i]->setVisible(false);
    connect(m_recentFileActions[i], &QAction::triggered, this, &ZMainWindow::openRecentFile);
  }

  // edit

  // view
  m_open3DViewAction = new QAction(tr("Open &3D Window"), this);
  m_open3DViewAction->setStatusTip(tr("Open 3D Window"));
  connect(m_open3DViewAction, &QAction::triggered, this, &ZMainWindow::open3DWindow);

  m_screenShotAction = new QAction(ZTheme::instance().icon(ZTheme::ScreenshotIcon), tr("&Screenshot"), this);
  ZTheme::instance().bindIcon(m_screenShotAction, ZTheme::ScreenshotIcon);
  m_screenShotAction->setStatusTip(tr("Screenshot"));
  connect(m_screenShotAction, &QAction::triggered, this, &ZMainWindow::openScreenshotPanel);

  m_shortcutsAction = new QAction(ZTheme::instance().icon(ZTheme::HelpIcon), tr("&Shortcuts"), this);
  ZTheme::instance().bindIcon(m_shortcutsAction, ZTheme::HelpIcon);
  m_shortcutsAction->setStatusTip(tr("Open keyboard and mouse shortcuts reference"));
  connect(m_shortcutsAction, &QAction::triggered, this, &ZMainWindow::openShortcutsReference);

  m_traceToolAction = new QAction(ZTheme::instance().icon(ZTheme::TraceIcon), tr("Trace"), this);
  ZTheme::instance().bindIcon(m_traceToolAction, ZTheme::TraceIcon);
  m_traceToolAction->setStatusTip(tr("Enable trace tool (left-click to trace)"));
  m_traceToolAction->setCheckable(true);
  m_traceToolAction->setChecked(m_doc->traceSettings().traceToolEnabled());
  connect(m_traceToolAction, &QAction::toggled, this, [this](bool on) {
    m_doc->traceSettings().setTraceToolEnabled(on);
    if (on && m_traceDockWidget != nullptr) {
      m_traceDockWidget->setVisible(true);
      m_traceDockWidget->raise();
    }
  });
  connect(&m_doc->traceSettings(), &ZTraceSettings::changed, this, [this]() {
    if (m_traceToolAction == nullptr) {
      return;
    }
    const bool on = m_doc->traceSettings().traceToolEnabled();
    if (m_traceToolAction->isChecked() == on) {
      return;
    }
    const QSignalBlocker blocker(*m_traceToolAction);
    m_traceToolAction->setChecked(on);
  });

  m_exitAction = new QAction(tr("E&xit"), this);
  m_exitAction->setShortcuts(QKeySequence::Quit);
  m_exitAction->setStatusTip(tr("Exit the application"));
  connect(m_exitAction, &QAction::triggered, qApp, &QApplication::closeAllWindows);

  m_aboutAction = new QAction(tr("&About"), this);
  m_aboutAction->setStatusTip(tr("Show the application's About box"));
  connect(m_aboutAction, &QAction::triggered, this, &ZMainWindow::about);
  m_aboutAction->setMenuRole(QAction::AboutRole);

  m_aboutQtAction = new QAction(tr("About &Qt"), this);
  m_aboutQtAction->setStatusTip(tr("Show the Qt library's About box"));
  connect(m_aboutQtAction, &QAction::triggered, qApp, &QApplication::aboutQt);
  m_aboutQtAction->setMenuRole(QAction::AboutQtRole);

  m_checkForUpdatesAction = new QAction(tr("Check for Updates..."), this);
  m_checkForUpdatesAction->setStatusTip(tr("Check for Updates"));
  connect(m_checkForUpdatesAction, &QAction::triggered, this, &ZMainWindow::checkForUpdates);
  m_checkForUpdatesAction->setMenuRole(QAction::ApplicationSpecificRole);

#ifdef Q_OS_LINUX
  m_createDesktopEntryAction = new QAction(tr("Create Desktop Entry..."), this);
  m_createDesktopEntryAction->setStatusTip(tr("Create Desktop Entry for Linux Desktop or Dock"));
  connect(m_createDesktopEntryAction, &QAction::triggered, this, &ZMainWindow::createDesktopEntry);
#endif

  //
  m_openLogFolderAction = new QAction(ZTheme::instance().icon(ZTheme::OpenFolderIcon), tr("&Open Log Folder"), this);
  ZTheme::instance().bindIcon(m_openLogFolderAction, ZTheme::OpenFolderIcon);
  m_openLogFolderAction->setStatusTip(tr("Open Log Folder"));
  connect(m_openLogFolderAction, &QAction::triggered, this, &ZMainWindow::openLogFolder);

  m_openSettingsAction = new QAction(tr("&Settings..."), this);
  m_openSettingsAction->setStatusTip(tr("Open Atlas Settings"));
  m_openSettingsAction->setMenuRole(QAction::PreferencesRole);
  connect(m_openSettingsAction, &QAction::triggered, this, &ZMainWindow::openSettingsDialog);

  m_restartAction = new QAction(tr("&Restart Atlas"), this);
  m_restartAction->setStatusTip(tr("Restart Atlas"));
  connect(m_restartAction, &QAction::triggered, this, [this]() {
    QTimer::singleShot(0, this, [this]() {
      (void)ZAppRestartController::requestRestart(*this, QApplication::activeWindow());
    });
  });

  m_openDiskCacheFolderAction =
    new QAction(ZTheme::instance().icon(ZTheme::OpenFolderIcon), tr("Open Disk Cache Folder"), this);
  ZTheme::instance().bindIcon(m_openDiskCacheFolderAction, ZTheme::OpenFolderIcon);
  m_openDiskCacheFolderAction->setStatusTip(tr("Open Disk Cache Folder"));
  connect(m_openDiskCacheFolderAction, &QAction::triggered, this, &ZMainWindow::openDiskCacheFolder);

#if ATLAS_ENABLE_CUSTOM_COMMAND
  m_runCustomCommandAction =
    new QAction(ZTheme::instance().icon(ZTheme::RunCommandIcon), tr("&Run Custom Command"), this);
  ZTheme::instance().bindIcon(m_runCustomCommandAction, ZTheme::RunCommandIcon);
  m_runCustomCommandAction->setStatusTip(tr("Run Custom Command"));
  connect(m_runCustomCommandAction, &QAction::triggered, this, &ZMainWindow::runCustomCommand);
#endif

  m_openNewInstanceAction = new QAction(tr("Open Additional Instance of Atlas"), this);
  connect(m_openNewInstanceAction, &QAction::triggered, this, &ZMainWindow::openNewInstance);
}

void ZMainWindow::createMenus()
{
  m_fileMenu = menuBar()->addMenu(tr("&File"));
  // m_fileMenu->addAction(m_newAction);
  m_fileMenu->addAction(m_openAction);
  m_fileMenu->addAction(m_saveAction);
  m_fileMenu->addAction(m_saveAsAction);
  m_fileMenu->addSeparator();
  m_fileMenu->addAction(m_loadSceneAction);
  m_fileMenu->addAction(m_saveSceneAction);
  m_fileMenu->addSeparator();
  for (auto act : m_doc->fileActions()) {
    m_fileMenu->addAction(act);
  }
  m_separatorAction = m_fileMenu->addSeparator();
  for (auto& recentFileAction : m_recentFileActions) {
    m_fileMenu->addAction(recentFileAction);
  }
  m_fileMenu->addSeparator();
  m_fileMenu->addAction(m_closeAction);
  m_fileMenu->addAction(m_exitAction);
  updateRecentFileActions();

  m_editMenu = menuBar()->addMenu(tr("&Edit"));
  m_editMenu->addAction(m_doc->undoAction());
  m_editMenu->addAction(m_doc->redoAction());
  m_editMenu->addSeparator();
  m_editMenu->addAction(m_view->copyAction());
  m_editMenu->addAction(m_view->pasteAction());
  m_editMenu->addSeparator();
  m_editMenu->addAction(m_openSettingsAction);
  // m_editMenu->addAction(m_engine->deleteAction());

  m_viewMenu = menuBar()->addMenu(tr("&View"));
  m_viewMenu->addAction(m_view->zoomInAction());
  m_viewMenu->addAction(m_view->zoomOutAction());
  m_viewMenu->addAction(m_view->fitIntoWindowAction());
  m_viewMenu->addSeparator();
  m_viewMenu->addAction(m_view->normalViewAction());
  m_viewMenu->addAction(m_view->maxZProjViewAction());
  m_viewMenu->addAction(m_view->montageViewAction());
  m_viewMenu->addSeparator();
  ZTheme::instance().addThemeMenu(m_viewMenu);
  m_viewMenu->addSeparator();
  m_viewMenu->addAction(m_open3DViewAction);
  m_viewMenu->addAction(m_screenShotAction);

  for (auto menu : m_doc->processObjMenu()) {
    menuBar()->addMenu(menu);
  }

  m_animationMenu = menuBar()->addMenu(tr("&Animation"));
  m_animationMenu->addAction(m_doc->make2DAnimationAction());
  m_animationMenu->addAction(m_doc->changeAnimationSettingAction());

  m_windowMenu = menuBar()->addMenu(tr("&Window"));

  menuBar()->addSeparator();

  m_helpMenu = menuBar()->addMenu(tr("&Help"));
  m_helpMenu->addAction(m_aboutAction);
  m_helpMenu->addAction(m_aboutQtAction);
  m_helpMenu->addAction(m_checkForUpdatesAction);
  // In-app documentation (Markdown)
  m_userGuideAction = new QAction(tr("User Guide"), this);
  connect(m_userGuideAction, &QAction::triggered, this, [this]() {
    openDocMd(QStringLiteral("USER_GUIDE.md"));
  });
  m_helpMenu->addAction(m_userGuideAction);
  m_devGuideAction = new QAction(tr("Developer Guide"), this);
  connect(m_devGuideAction, &QAction::triggered, this, [this]() {
    openDocMd(QStringLiteral("DEVELOPER_GUIDE.md"));
  });
  m_helpMenu->addAction(m_devGuideAction);
  m_thirdPartyNoticesAction = new QAction(tr("Third-Party Notices"), this);
  connect(m_thirdPartyNoticesAction, &QAction::triggered, this, [this]() {
    openDocMd(QStringLiteral("THIRD_PARTY_NOTICES.md"));
  });
  m_helpMenu->addAction(m_thirdPartyNoticesAction);
  m_helpMenu->addAction(m_shortcutsAction);
#ifdef Q_OS_LINUX
  m_helpMenu->addAction(m_createDesktopEntryAction);
#endif
  m_helpMenu->addSeparator();
  m_helpMenu->addAction(m_openLogFolderAction);
  m_helpMenu->addAction(m_openDiskCacheFolderAction);
#if ATLAS_ENABLE_CUSTOM_COMMAND
  m_helpMenu->addAction(m_runCustomCommandAction);
#endif

  m_dockMenu = new QMenu(this);
  m_dockMenu->addAction(m_openNewInstanceAction);
#ifdef Q_OS_MACOS
  m_dockMenu->setAsDockMenu();
#endif
}

void ZMainWindow::createToolBars()
{
  QSize iconSize(22, 22);
  m_fileToolBar = addToolBar(tr("File"));
  // m_fileToolBar->addAction(m_newAction);
  m_fileToolBar->addAction(m_openAction);
  m_fileToolBar->addAction(m_saveAction);
  m_fileToolBar->setIconSize(iconSize);

  m_editToolBar = addToolBar(tr("Edit"));
  m_editToolBar->addAction(m_doc->undoAction());
  m_editToolBar->addAction(m_doc->redoAction());
  m_editToolBar->setIconSize(iconSize);

  m_viewToolBar = addToolBar(tr("View"));
  m_viewToolBar->addAction(m_view->zoomInAction());
  m_viewToolBar->addAction(m_view->zoomOutAction());
  m_viewToolBar->addWidget(m_view->createScaleWidget(m_viewToolBar));
  m_viewToolBar->addAction(m_view->fitIntoWindowAction());
  m_viewToolBar->addAction(m_view->normalViewAction());
  m_viewToolBar->addAction(m_view->maxZProjViewAction());
  m_viewToolBar->addAction(m_view->montageViewAction());
  m_viewToolBar->addAction(m_traceToolAction);
  m_viewToolBar->addAction(m_doc->imgDoc().autoTraceAction());
  m_viewToolBar->addAction(m_open3DViewAction);
  m_viewToolBar->addAction(m_screenShotAction);
  m_viewToolBar->setIconSize(iconSize);

  m_dragModeToolBar = addToolBar(tr("Drag Mode"));
  m_dragModeToolBar->addAction(m_view->scrollHandDragAction());
  m_dragModeToolBar->addAction(m_view->rubberBandDragAction());
  m_dragModeToolBar->setIconSize(iconSize);

  m_roiToolBar = addToolBar(tr("ROI"));
  m_roiToolBar->addWidget(m_view->createROIToolButton(this));
  m_roiToolBar->addWidget(m_view->createROIModeWidget(this));
  m_roiToolBar->setIconSize(iconSize);

  m_shortcutsToolBar = addToolBar(tr("Shortcuts"));
  m_shortcutsToolBar->addAction(m_shortcutsAction);
  m_shortcutsToolBar->setIconSize(iconSize);
}

void ZMainWindow::createStatusBar()
{
  statusBar()->showMessage(tr("Ready"));
}

void ZMainWindow::createDockWindows()
{
  m_objectsDockWidget = new QDockWidget(tr("Objects Manager"), this);
  m_objectsDockWidget->setFeatures(QDockWidget::DockWidgetClosable);
  m_objectsDockWidget->setAllowedAreas(Qt::RightDockWidgetArea);
  ZObjWidget* objWidget = m_doc->createObjWidget(this);
  m_objectsDockWidget->setWidget(objWidget);
  connect(m_doc.get(), &ZDoc::openEditWidget, this, &ZMainWindow::openEditWidget);
  addDockWidget(Qt::RightDockWidgetArea, m_objectsDockWidget);
  m_windowMenu->addAction(m_objectsDockWidget->toggleViewAction());

  m_viewSettingDockWidget = new QDockWidget(tr("Object View Setting"), this);
  m_viewSettingDockWidget->setFeatures(QDockWidget::DockWidgetClosable);
  m_viewSettingDockWidget->setAllowedAreas(Qt::RightDockWidgetArea);
  connect(m_doc.get(), &ZDoc::showViewSetting, this, &ZMainWindow::raiseViewSettingDockWidget);
  m_viewSettingDockWidget->setWidget(new ZViewSettingWidget(*m_doc, m_view.get(), this));
  addDockWidget(Qt::RightDockWidgetArea, m_viewSettingDockWidget);
  m_windowMenu->addAction(m_viewSettingDockWidget->toggleViewAction());

  m_objectDetailedInfoDockWidget = new QDockWidget(tr("Object Detailed Info"), this);
  m_objectDetailedInfoDockWidget->setFeatures(QDockWidget::DockWidgetClosable | QDockWidget::DockWidgetMovable |
                                              QDockWidget::DockWidgetFloatable);
  m_objectDetailedInfoDockWidget->setAllowedAreas(Qt::RightDockWidgetArea);
  m_objectDetailedInfoDockWidget->setWidget(new ZObjDetailedInfoWidget(*m_doc, this));
  addDockWidget(Qt::RightDockWidgetArea, m_objectDetailedInfoDockWidget);
  m_windowMenu->addAction(m_objectDetailedInfoDockWidget->toggleViewAction());
  m_objectDetailedInfoDockWidget->setVisible(false);

  m_globalSettingDockWidget = new QDockWidget(tr("Global View Setting"), this);
  m_globalSettingDockWidget->setFeatures(QDockWidget::DockWidgetClosable | QDockWidget::DockWidgetMovable |
                                         QDockWidget::DockWidgetFloatable);
  m_globalSettingDockWidget->setAllowedAreas(Qt::RightDockWidgetArea);
  m_globalSettingDockWidget->setWidget(m_view->globalParasWidget());
  addDockWidget(Qt::RightDockWidgetArea, m_globalSettingDockWidget);
  m_windowMenu->addAction(m_globalSettingDockWidget->toggleViewAction());
  tabifyDockWidget(m_viewSettingDockWidget, m_globalSettingDockWidget);

  m_traceDockWidget = new QDockWidget(tr("Trace Settings"), this);
  m_traceDockWidget->setFeatures(QDockWidget::DockWidgetClosable | QDockWidget::DockWidgetMovable |
                                 QDockWidget::DockWidgetFloatable);
  m_traceDockWidget->setAllowedAreas(Qt::RightDockWidgetArea);
  m_traceDockWidget->setWidget(new ZTraceSettingsWidget(*m_doc, m_traceDockWidget));
  addDockWidget(Qt::RightDockWidgetArea, m_traceDockWidget);
  tabifyDockWidget(m_globalSettingDockWidget, m_traceDockWidget);
  auto* traceToggle = m_traceDockWidget->toggleViewAction();
  ZTheme::instance().bindIcon(traceToggle, ZTheme::TraceIcon);
  m_windowMenu->addAction(traceToggle);
  m_traceDockWidget->setVisible(false);

  m_tasksDockWidget = new QDockWidget(tr("Tasks"), this);
  m_tasksDockWidget->setFeatures(QDockWidget::DockWidgetClosable | QDockWidget::DockWidgetMovable |
                                 QDockWidget::DockWidgetFloatable);
  m_tasksDockWidget->setAllowedAreas(Qt::RightDockWidgetArea);
  m_tasksDockWidget->setWidget(new ZBackgroundTaskManagerWidget(m_doc->backgroundTaskManager(), m_tasksDockWidget));
  addDockWidget(Qt::RightDockWidgetArea, m_tasksDockWidget);
  tabifyDockWidget(m_traceDockWidget, m_tasksDockWidget);
  m_windowMenu->addAction(m_tasksDockWidget->toggleViewAction());
  m_tasksDockWidget->setVisible(false);

  connect(m_doc.get(), &ZDoc::showBackgroundTasksPanel, this, [this]() {
    if (m_tasksDockWidget == nullptr) {
      return;
    }
    if (m_tasksDockWidget->isHidden()) {
      m_tasksDockWidget->show();
    }
    m_tasksDockWidget->raise();
  });

  m_captureDockWidget = new QDockWidget(tr("Capture"), this);
  m_captureDockWidget->setFeatures(QDockWidget::DockWidgetClosable | QDockWidget::DockWidgetMovable |
                                   QDockWidget::DockWidgetFloatable);
  m_captureDockWidget->setAllowedAreas(Qt::RightDockWidgetArea);
  m_captureDockWidget->setWidget(m_view->captureWidget());
  addDockWidget(Qt::RightDockWidgetArea, m_captureDockWidget);
  m_windowMenu->addAction(m_captureDockWidget->toggleViewAction());
  m_captureDockWidget->setVisible(false);

  m_editObjDockWidget = new QDockWidget(tr("Edit and Output"), this);
  m_editObjDockWidget->setFeatures(QDockWidget::DockWidgetClosable);
  m_editObjDockWidget->setAllowedAreas(Qt::BottomDockWidgetArea);
  m_objEditWidget = new ZObjEditWidget(*m_doc, this);
  m_editObjDockWidget->setWidget(m_objEditWidget);
  addDockWidget(Qt::BottomDockWidgetArea, m_editObjDockWidget);
  m_windowMenu->addAction(m_editObjDockWidget->toggleViewAction());
  m_editObjDockWidget->setVisible(false);
}

void ZMainWindow::readSettings()
{
  QSettings settings;
  // cause problem on windows with multiple monitors
  // QPoint pos = settings.value("pos", QPoint(200, 200)).toPoint();
  QSize size = settings.value("size", QSize(400, 400)).toSize();
  // move(pos);
  resize(size);
}

void ZMainWindow::writeSettings()
{
  QSettings settings;
  settings.setValue("pos", pos());
  settings.setValue("size", size());
}

// void ZMainWindow::loadWorkspace(const QString &fileName)
//{
//   setCurrentFile(fileName);
//   statusBar()->showMessage(tr("Workspace loaded"), 2000);
//   activateWindowIfNot();
// }

// bool ZMainWindow::saveFile(const QString &fileName)
//{
//   setCurrentFile(fileName);
//   statusBar()->showMessage(tr("Workspace saved"), 2000);
//   return true;
// }

// void ZMainWindow::setCurrentFile(const QString &fileName)
//{
//   static int sequenceNumber = 1;

//  if (fileName.isEmpty()) {
//    setWindowFilePath(tr("Workspace%1").arg(sequenceNumber++));
//  } else {
//    setWindowFilePath(QFileInfo(fileName).canonicalFilePath());
//  }

//  setWindowModified(false);
//}

QString ZMainWindow::strippedName(const QString& fullFileName)
{
  return QFileInfo(fullFileName).fileName();
}

ZMainWindow* ZMainWindow::findMainWindow(const QString&)
{
  return nullptr;
}

bool ZMainWindow::loadJsonSceneImpl(const QString& fn, QString& err)
{
  try {
    auto loadObj = loadJsonObject(fn);
    if (!loadObj.contains("Scene") || !loadObj.at("Scene").is_object()) {
      err = tr("File is not scene format");
      return false;
    }

    const auto& sceneObj = loadObj.at("Scene").as_object();

    QDir::setCurrent(QFileInfo(fn).absolutePath());

    std::map<size_t, size_t> idmap = m_doc->read(sceneObj.at("Doc").as_object(), err);

    if (idmap.empty()) {
      LOG(WARNING) << "Scene " << fn << " contains zero objects";
    }

    // If 3D state exists, ensure 3D window/engine is ready and set up a deferred apply session
    bool has3DGeneral = sceneObj.contains("View3DGeneral");
    size_t numView3DPerObject = 0;
    for (const auto& [key, value] : sceneObj) {
      if (key != "Doc" && key != "Version" && key != "View2DGeneral" && key != "View3DGeneral") {
        const auto& viewObjCandidate = value.as_object();
        if (viewObjCandidate.contains("View3D")) {
          ++numView3DPerObject;
        }
      }
    }

    if ((has3DGeneral || numView3DPerObject > 0) && !m_3dWindow) {
      QSignalSpy spy(this, &ZMainWindow::window3DReady);
      open3DWindow();
      while (!spy.wait(5000)) {
        LOG(INFO) << "waiting for 3d window initialization";
      }
    }

    std::unique_ptr<QSignalSpy> sceneApplySpy;
    if (m_3dWindow && (has3DGeneral || numView3DPerObject > 0)) {
      // Listen before posting tasks to avoid missing early finish
      sceneApplySpy = std::make_unique<QSignalSpy>(m_3dWindow->engine(), &Z3DRenderingEngine::scene3DApplyFinished);
      // Reset engine-side apply session
      QMetaObject::invokeMethod(
        m_3dWindow->engine(),
        [eng = m_3dWindow->engine()]() {
          eng->beginScene3DApply();
        },
        Qt::BlockingQueuedConnection);
    }

    for (const auto& [key, value] : sceneObj) {
      if (key == "View2DGeneral") {
        m_view->read(value.as_object());
      } else if (key == "View3DGeneral") {
        if (m_3dWindow) {
          auto j = value.as_object();
          QMetaObject::invokeMethod(
            m_3dWindow->engine(),
            [eng = m_3dWindow->engine(), j]() {
              eng->applyView3DGeneral(j);
            },
            Qt::QueuedConnection);
        }
      } else if (key != "Doc" && key != "Version") {
        QString qkey = QString::fromUtf8(key.data(), key.size());
        bool ok;
        size_t objectId = qkey.toLongLong(&ok);
        if (ok) {
          if (idmap.contains(objectId)) {
            size_t id = idmap.at(objectId);
            // VLOG(1) << id;
            const auto& viewObj = value.as_object();
            if (viewObj.contains("View2D")) {
              m_view->read(id, viewObj.at("View2D").as_object());
            }
            if (viewObj.contains("View3D")) {
              if (m_3dWindow) {
                auto j = viewObj.at("View3D").as_object();
                QMetaObject::invokeMethod(
                  m_3dWindow->engine(),
                  [eng = m_3dWindow->engine(), id, j]() {
                    eng->applyView3DForId(id, j);
                  },
                  Qt::QueuedConnection);
              }
            }
          }
        } else {
          err += QString("Unknown scene key %1\n").arg(qkey);
        }
      }
    }
    // Optionally wait until all 3D settings have been applied
    if (m_3dWindow && (has3DGeneral || numView3DPerObject > 0) && absl::GetFlag(FLAGS_atlas_block_scene_3d_apply)) {
      while (sceneApplySpy && sceneApplySpy->count() == 0 && !sceneApplySpy->wait(5000)) {
        LOG(INFO) << "waiting for 3D scene apply to finish";
      }
    }
    LOG(INFO) << "Finish loading scene";

    return true;
  }
  catch (const std::exception& e) {
    err += e.what();
    return false;
  }
}

bool ZMainWindow::saveJsonSceneImpl(const QString& fn, QString& err)
{
  auto* engineOrNull = m_3dWindow ? m_3dWindow->engine() : nullptr;
  return ZSceneJsonIO::saveToPath(m_doc.get(), m_view.get(), engineOrNull, fn, err);
}

} // namespace nim
