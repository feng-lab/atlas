#include "z3dmainwindow.h"

#include "zdoc.h"
#include "zimgdoc.h"
#include "z3drenderingengine.h"
#include "zrpcservice.h"
#include "ztakescreenshotwidget.h"
#include "zviewsettingwidget.h"
#include "zobjdetailedinfowidget.h"
#include "z3dcanvas.h"
#include "zobjwidget.h"
#include "zobjeditwidget.h"
#include "zobjdoc.h"
#include "zmainwindow.h"
#include "zlog.h"
#include "zsysteminfo.h"
#include "ztheme.h"
#include "zmessageboxhelpers.h"
#include "ztracesettings.h"
#include "ztracesettingswidget.h"
#include "zbackgroundtaskmanagerwidget.h"
#include <QFileDialog>
#include <QMessageBox>
#include <QAction>
#include <QActionGroup>
#include <QDockWidget>
#include <QToolBar>
#include <QMimeData>
#include <QMenuBar>
#include <QSettings>
#include <QStatusBar>
#include <QProgressBar>
#include <QApplication>
#include <QDesktopServices>
#include <QFile>
#include <QMetaObject>
#include <QSignalBlocker>
#include "zservicemanager.h"

namespace nim {

Z3DMainWindow::Z3DMainWindow(ZDoc& doc, ZMainWindow& win2d, bool stereoView, QWidget* parent)
  : QMainWindow(parent)
  , m_doc(doc)
  , m_isStereoView(stereoView)
  , m_2dWindow(win2d)
{
  m_engine = new Z3DRenderingEngine(m_doc);
  m_engine->moveToThread(&m_renderingThread);
  m_renderingThread.setObjectName("RenderingThread");
  connect(&m_renderingThread, &QThread::finished, m_engine, &QObject::deleteLater);
  m_renderingThread.start();

  m_canvas = new Z3DCanvas("", 512, 512, this);
  m_canvas->setDoc(&m_doc);

  connect(m_engine, &Z3DRenderingEngine::initialized, this, &Z3DMainWindow::renderingEngineInitialized);

#if defined(ATLAS_USE_OPENGLWIDGET)
  connect(m_canvas, &Z3DCanvas::openGLContextInitialized, this, &Z3DMainWindow::initRenderingEngine);
#endif

  setCentralWidget(m_canvas);
  init();
  setCurrentFile("");

  connect(m_engine, &Z3DRenderingEngine::renderingError, this, &Z3DMainWindow::onRenderingError);
  connect(m_engine, &Z3DRenderingEngine::progressChanged, this, &Z3DMainWindow::onProgressChanged);

  // Keep a thread-safe snapshot of Trace Settings on the rendering thread so
  // Z3DImgFilter can decide whether to emit seed-trace menu requests.
  connect(&m_doc.traceSettings(), &ZTraceSettings::changed, this, [this]() {
    const bool enabled = m_doc.traceSettings().traceToolEnabled();
    const bool inProgress = m_doc.traceSettings().traceInProgress();
    const std::optional<size_t> sourceImgId = m_doc.traceSettings().sourceImageId();
    const size_t sourceChannel = m_doc.traceSettings().sourceChannel();

    Z3DRenderingEngine* engine = m_engine;
    QMetaObject::invokeMethod(
      engine,
      [engine, enabled, inProgress, sourceImgId, sourceChannel]() {
        engine->setSeedTraceUiState(enabled, inProgress, sourceImgId, sourceChannel);
      },
      Qt::QueuedConnection);
  });
  {
    const bool enabled = m_doc.traceSettings().traceToolEnabled();
    const bool inProgress = m_doc.traceSettings().traceInProgress();
    const std::optional<size_t> sourceImgId = m_doc.traceSettings().sourceImageId();
    const size_t sourceChannel = m_doc.traceSettings().sourceChannel();

    Z3DRenderingEngine* engine = m_engine;
    QMetaObject::invokeMethod(
      engine,
      [engine, enabled, inProgress, sourceImgId, sourceChannel]() {
        engine->setSeedTraceUiState(enabled, inProgress, sourceImgId, sourceChannel);
      },
      Qt::QueuedConnection);
  }

#if !defined(ATLAS_USE_OPENGLWIDGET)
  // Without QOpenGLWidget, the rendering thread can initialize very quickly. If we start initialization
  // before init() wires `initialized()` → `onViewReady()`, the signal can be missed and the right-side
  // dock widgets stay empty (the dock area exists, but no widgets are inserted).
  initRenderingEngine();
#endif
}

Z3DMainWindow::~Z3DMainWindow()
{
  if (!m_renderingThread.isFinished()) {
    m_engine->cancelLongRendering();
    QMetaObject::invokeMethod(m_engine,
                              &Z3DRenderingEngine::drainVulkanFrameExecutorForTeardown,
                              Qt::BlockingQueuedConnection);
    m_renderingThread.quit();
    m_renderingThread.wait();
  }
}

Z3DMainWindow::CanvasSizeInfo Z3DMainWindow::canvasSizeInfo() const
{
  CHECK(m_canvas);
  const glm::uvec2 logical = m_canvas->logicalSize();
  const glm::uvec2 physical = m_canvas->physicalSize();
  CanvasSizeInfo out;
  out.logicalWidth = static_cast<int>(logical.x);
  out.logicalHeight = static_cast<int>(logical.y);
  out.physicalWidth = static_cast<int>(physical.x);
  out.physicalHeight = static_cast<int>(physical.y);
  return out;
}

Z3DMainWindow::CanvasSizeInfo Z3DMainWindow::setCanvasLogicalSize(int logicalWidth, int logicalHeight)
{
  CHECK(m_canvas);
  CHECK(logicalWidth > 0);
  CHECK(logicalHeight > 0);
  CHECK(QThread::currentThread() == thread());

  showNormal();
  raise();
  activateWindow();

  const QSize targetCanvas(logicalWidth, logicalHeight);
  for (int attempt = 0; attempt < 3; ++attempt) {
    const QSize currentCanvas = m_canvas->size();
    if (currentCanvas == targetCanvas) {
      break;
    }

    const QSize currentWindow = size();
    const QSize targetWindow(currentWindow.width() + (targetCanvas.width() - currentCanvas.width()),
                             currentWindow.height() + (targetCanvas.height() - currentCanvas.height()));
    resize(targetWindow);
  }

  return canvasSizeInfo();
}

void Z3DMainWindow::openEditWidget(size_t id)
{
  ZObjDoc* doc = m_doc.idToDoc(id);
  if (doc->objUndoStack(id)) {
    doc->objUndoStack(id)->setActive();
  } else {
    m_doc.activateEmptyUndoStack();
  }
  if (!doc->typeName().contains("2D", Qt::CaseInsensitive)) {
    if (m_objEditWidget->showObjEditWidgetOfObj(id)) {
      m_editObjDockWidget->setVisible(true);
    }
  } else {
    // m_editObjDockWidget->setVisible(false);
    m_2dWindow.openEditWidget(id);
  }
}

void Z3DMainWindow::closeEvent(QCloseEvent* event)
{
  if (maybeSave()) {
    writeSettings();
    m_engine->cancelLongRendering();
    QMetaObject::invokeMethod(m_engine,
                              &Z3DRenderingEngine::drainVulkanFrameExecutorForTeardown,
                              Qt::BlockingQueuedConnection);
    m_renderingThread.quit();
    m_renderingThread.wait();
    event->accept();
  } else {
    event->ignore();
  }
}

void Z3DMainWindow::dragEnterEvent(QDragEnterEvent* event)
{
  if (event->mimeData()->hasFormat("text/uri-list")) {
    event->acceptProposedAction();
  }
}

void Z3DMainWindow::dropEvent(QDropEvent* event)
{
  QList<QUrl> urlList = event->mimeData()->urls();
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
    m_doc.loadFileList(fileList);
  }
}

void Z3DMainWindow::open() {}

bool Z3DMainWindow::save()
{
  return m_doc.saveAllObjs();
}

bool Z3DMainWindow::saveAs()
{
  return m_doc.saveSelectedObjsAs();
}

void Z3DMainWindow::openRecentFile()
{
  if (auto action = qobject_cast<QAction*>(sender())) {
    QString fn = action->data().toString();
    if (fn.endsWith(".scene", Qt::CaseInsensitive)) {
      Q_EMIT loadJsonScene(fn);
    } else {
      m_doc.loadFile(action->data().toString());
    }
  }
}

void Z3DMainWindow::activateWindowIfNot()
{
  if (!isActiveWindow()) {
    this->activateWindow();
  }
}

void Z3DMainWindow::changeBackground()
{
  if (m_backgroundDockWidget->isHidden()) {
    m_backgroundDockWidget->show();
  }
  m_backgroundDockWidget->raise();
}

void Z3DMainWindow::changeAxis()
{
  if (m_axisDockWidget->isHidden()) {
    m_axisDockWidget->show();
  }
  m_axisDockWidget->raise();
}

void Z3DMainWindow::openScreenshotPanel()
{
  if (m_captureDockWidget->isHidden()) {
    m_captureDockWidget->show();
  }
  m_captureDockWidget->raise();
}

void Z3DMainWindow::openHelpPanel()
{
  if (m_helpDockWidget->isHidden()) {
    m_helpDockWidget->show();
  }
  m_helpDockWidget->raise();
}

void Z3DMainWindow::raiseViewSettingDockWidget()
{
  if (m_viewSettingDockWidget->isHidden()) {
    m_viewSettingDockWidget->show();
  }
  m_viewSettingDockWidget->raise();
}

void Z3DMainWindow::raiseGlobalSettingDockWidget()
{
  if (m_globalSettingDockWidget->isHidden()) {
    m_globalSettingDockWidget->show();
  }
  m_globalSettingDockWidget->raise();
}

void Z3DMainWindow::init()
{
  setAttribute(Qt::WA_DeleteOnClose);
  setAcceptDrops(true);

  createActions();
  createMenus();
  createToolBars();
  createStatusBar();
  createDockWindows();
  connect(m_engine, &Z3DRenderingEngine::initialized, this, &Z3DMainWindow::onViewReady);

  readSettings();
}

void Z3DMainWindow::createActions()
{
  // file
  m_openAction = new QAction(ZTheme::instance().icon(ZTheme::OpenFolderIcon), tr("&Open..."), this);
  m_openAction->setShortcuts(QKeySequence::Open);
  m_openAction->setStatusTip(tr("Open an existing scene file"));
  connect(m_openAction, &QAction::triggered, this, &Z3DMainWindow::loadScene);

  m_saveAction = new QAction(ZTheme::instance().icon(ZTheme::SaveIcon), tr("&Save"), this);
  m_saveAction->setShortcuts(QKeySequence::Save);
  m_saveAction->setStatusTip(tr("Save unsaved objects to disk"));
  connect(m_saveAction, &QAction::triggered, this, &Z3DMainWindow::save);

  m_saveAsAction = new QAction(ZTheme::instance().icon(ZTheme::SaveAsIcon), tr("Save &As..."), this);
  m_saveAsAction->setShortcuts(QKeySequence::SaveAs);
  m_saveAsAction->setStatusTip(tr("Save selected objects under a new name"));
  connect(m_saveAsAction, &QAction::triggered, this, &Z3DMainWindow::saveAs);

  m_loadSceneAction = new QAction(tr("Load &Scene..."), this);
  m_loadSceneAction->setStatusTip(tr("Load scene"));
  connect(m_loadSceneAction, &QAction::triggered, this, &Z3DMainWindow::loadScene);

  m_saveSceneAction = new QAction(tr("Save &Scene..."), this);
  m_saveSceneAction->setStatusTip(tr("Save scene"));
  connect(m_saveSceneAction, &QAction::triggered, this, &Z3DMainWindow::saveScene);

  m_closeAction = new QAction(tr("&Close"), this);
  m_closeAction->setShortcut(QKeySequence::Close);
  m_closeAction->setStatusTip(tr("Close this window"));
  connect(m_closeAction, &QAction::triggered, this, &Z3DMainWindow::close);

  // edit

  // view
  m_zoomInAction = new QAction(ZTheme::instance().icon(ZTheme::ZoomInIcon), tr("Zoom &In"), this);
  QList<QKeySequence> zoomInKey;
  zoomInKey << QKeySequence::ZoomIn << QKeySequence(Qt::Key_Plus) << QKeySequence(Qt::Key_Equal);
  m_zoomInAction->setShortcuts(zoomInKey);
  m_zoomInAction->setStatusTip(tr("Zoom in"));
  connect(m_zoomInAction, &QAction::triggered, this, &Z3DMainWindow::zoomIn);

  m_zoomOutAction = new QAction(ZTheme::instance().icon(ZTheme::ZoomOutIcon), tr("Zoom &Out"), this);
  QList<QKeySequence> zoomOutKey;
  zoomOutKey << QKeySequence::ZoomOut << QKeySequence(Qt::Key_Minus);
  m_zoomOutAction->setShortcuts(zoomOutKey);
  m_zoomOutAction->setStatusTip(tr("Zoom out"));
  connect(m_zoomOutAction, &QAction::triggered, this, &Z3DMainWindow::zoomOut);

  m_resetCameraAction = new QAction(tr("&Reset Camera"), this);
  m_resetCameraAction->setStatusTip(tr("Reset camera to show all objects in scene"));
  connect(m_resetCameraAction, &QAction::triggered, this, &Z3DMainWindow::resetCamera);

  m_changeBackgroundAction =
    new QAction(ZTheme::instance().icon(ZTheme::BackgroundIcon), tr("&Change Background"), this);
  m_changeBackgroundAction->setStatusTip(tr("Change background of 3d view"));
  connect(m_changeBackgroundAction, &QAction::triggered, this, &Z3DMainWindow::changeBackground);

  m_changeAxisAction = new QAction(ZTheme::instance().icon(ZTheme::AxisIcon), tr("&Change Axis"), this);
  m_changeAxisAction->setStatusTip(tr("Change axis of 3d view"));
  connect(m_changeAxisAction, &QAction::triggered, this, &Z3DMainWindow::changeAxis);

  m_screenShotAction = new QAction(ZTheme::instance().icon(ZTheme::ScreenshotIcon), tr("&Screenshot"), this);
  m_screenShotAction->setStatusTip(tr("Screenshot"));
  connect(m_screenShotAction, &QAction::triggered, this, &Z3DMainWindow::openScreenshotPanel);

  m_helpAction = new QAction(ZTheme::instance().icon(ZTheme::HelpIcon), tr("&Help"), this);
  m_helpAction->setStatusTip(tr("Help"));
  connect(m_helpAction, &QAction::triggered, this, &Z3DMainWindow::openHelpPanel);

  m_traceToolAction = new QAction(ZTheme::instance().icon(ZTheme::TraceIcon), tr("Trace"), this);
  m_traceToolAction->setStatusTip(tr("Enable trace tool (left-click to trace)"));
  m_traceToolAction->setCheckable(true);
  m_traceToolAction->setChecked(m_doc.traceSettings().traceToolEnabled());
  connect(m_traceToolAction, &QAction::toggled, this, [this](bool on) {
    m_doc.traceSettings().setTraceToolEnabled(on);
    if (on && m_traceDockWidget != nullptr) {
      m_traceDockWidget->setVisible(true);
      m_traceDockWidget->raise();
    }
  });
  connect(&m_doc.traceSettings(), &ZTraceSettings::changed, this, [this]() {
    if (m_traceToolAction == nullptr) {
      return;
    }
    const bool on = m_doc.traceSettings().traceToolEnabled();
    if (m_traceToolAction->isChecked() == on) {
      return;
    }
    const QSignalBlocker blocker(*m_traceToolAction);
    m_traceToolAction->setChecked(on);
  });

  m_cancelAction = new QAction(ZTheme::instance().icon(ZTheme::CancelIcon), tr("&Cancel Rendering"), this);
  m_cancelAction->setStatusTip(tr("Cancel Rendering"));
  connect(m_cancelAction, &QAction::triggered, this, &Z3DMainWindow::cancelRendering);
}

void Z3DMainWindow::createMenus()
{
  m_fileMenu = menuBar()->addMenu(tr("&File"));
  m_fileMenu->addAction(m_openAction);
  m_fileMenu->addAction(m_saveAction);
  m_fileMenu->addAction(m_saveAsAction);
  m_fileMenu->addSeparator();
  m_fileMenu->addAction(m_loadSceneAction);
  m_fileMenu->addAction(m_saveSceneAction);
  m_fileMenu->addSeparator();
  for (auto act : m_doc.fileActions()) {
    m_fileMenu->addAction(act);
  }
  m_separatorAction = m_fileMenu->addSeparator();
  for (auto recentFileAction : m_2dWindow.recentFileActions()) {
    m_fileMenu->addAction(recentFileAction);
  }
  m_fileMenu->addSeparator();
  m_fileMenu->addAction(m_closeAction);
  m_fileMenu->addAction(m_2dWindow.exitAction());

  m_editMenu = menuBar()->addMenu(tr("&Edit"));
  m_editMenu->addAction(m_doc.undoAction());
  m_editMenu->addAction(m_doc.redoAction());

  m_viewMenu = menuBar()->addMenu(tr("&View"));
  m_viewMenu->addAction(m_zoomInAction);
  m_viewMenu->addAction(m_zoomOutAction);
  m_viewMenu->addAction(m_resetCameraAction);
  m_viewMenu->addSeparator();
  m_viewMenu->addAction(m_changeBackgroundAction);
  m_viewMenu->addAction(m_changeAxisAction);
  m_viewMenu->addSeparator();
  m_viewMenu->addAction(m_screenShotAction);

  for (auto menu : m_doc.processObjMenu()) {
    menuBar()->addMenu(menu);
  }

  m_animationMenu = menuBar()->addMenu(tr("&Animation"));
  m_animationMenu->addAction(m_doc.make3DAnimationAction());
  m_animationMenu->addAction(m_doc.changeAnimationSettingAction());

  m_windowMenu = menuBar()->addMenu(tr("&Window"));

  menuBar()->addSeparator();

  m_helpMenu = menuBar()->addMenu(tr("&Help"));
  m_helpMenu->addAction(m_2dWindow.aboutAction());
  m_helpMenu->addAction(m_2dWindow.aboutQtAction());
  m_helpMenu->addAction(m_2dWindow.checkForUpdatesAction());
  // Share doc actions with 2D window
  m_helpMenu->addAction(m_2dWindow.userGuideAction());
  m_helpMenu->addAction(m_2dWindow.developerGuideAction());
  m_helpMenu->addAction(m_helpAction);
  m_helpMenu->addSeparator();
  m_helpMenu->addAction(m_2dWindow.openLogFolderAction());
  m_helpMenu->addAction(m_2dWindow.openConfigFolderAction());
  m_helpMenu->addAction(m_2dWindow.openDiskCacheFolderAction());
}

void Z3DMainWindow::createToolBars()
{
  QSize iconSize(22, 22);
  m_fileToolBar = addToolBar(tr("File"));
  m_fileToolBar->addAction(m_openAction);
  m_fileToolBar->addAction(m_saveAction);
  m_fileToolBar->setIconSize(iconSize);

  m_editToolBar = addToolBar(tr("Edit"));
  m_editToolBar->addAction(m_doc.undoAction());
  m_editToolBar->addAction(m_doc.redoAction());
  m_editToolBar->setIconSize(iconSize);

  m_viewToolBar = addToolBar(tr("View"));
  m_viewToolBar->addAction(m_zoomInAction);
  m_viewToolBar->addAction(m_zoomOutAction);
  m_viewToolBar->addAction(m_resetCameraAction);
  m_viewToolBar->addAction(m_changeBackgroundAction);
  m_viewToolBar->addAction(m_changeAxisAction);
  m_viewToolBar->addAction(m_traceToolAction);
  m_viewToolBar->addAction(m_doc.imgDoc().autoTraceAction());
  m_viewToolBar->addAction(m_screenShotAction);
  m_viewToolBar->setIconSize(iconSize);

  m_helpToolBar = addToolBar(tr("Help"));
  m_helpToolBar->addAction(m_helpAction);
  m_helpToolBar->setIconSize(iconSize);

  m_progressToolBar = addToolBar(tr("Progress"));
  m_progressBarWidget = new QProgressBar();
  m_progressBarWidget->setMaximumWidth(150);
  m_progressBarAction = m_progressToolBar->addWidget(m_progressBarWidget);
  m_progressToolBar->addAction(m_cancelAction);
  m_progressToolBar->setIconSize(iconSize);
}

void Z3DMainWindow::createStatusBar()
{
  statusBar()->showMessage(tr("Ready"));
}

void Z3DMainWindow::createDockWindows()
{
  m_objectsDockWidget = new QDockWidget(tr("Objects Manager"), this);
  m_objectsDockWidget->setFeatures(QDockWidget::DockWidgetClosable);
  m_objectsDockWidget->setAllowedAreas(Qt::RightDockWidgetArea);
  connect(&m_doc, &ZDoc::openEditWidget, this, &Z3DMainWindow::openEditWidget);
  addDockWidget(Qt::RightDockWidgetArea, m_objectsDockWidget);
  m_windowMenu->addAction(m_objectsDockWidget->toggleViewAction());

  m_viewSettingDockWidget = new QDockWidget(tr("Object View Setting"), this);
  m_viewSettingDockWidget->setFeatures(QDockWidget::DockWidgetClosable);
  m_viewSettingDockWidget->setAllowedAreas(Qt::RightDockWidgetArea);
  connect(&m_doc, &ZDoc::showViewSetting, this, &Z3DMainWindow::raiseViewSettingDockWidget);
  addDockWidget(Qt::RightDockWidgetArea, m_viewSettingDockWidget);
  m_windowMenu->addAction(m_viewSettingDockWidget->toggleViewAction());

  m_objectDetailedInfoDockWidget = new QDockWidget(tr("Object Detailed Info"), this);
  m_objectDetailedInfoDockWidget->setFeatures(QDockWidget::DockWidgetClosable | QDockWidget::DockWidgetMovable |
                                              QDockWidget::DockWidgetFloatable);
  m_objectDetailedInfoDockWidget->setAllowedAreas(Qt::RightDockWidgetArea);
  m_objDetailedInfoWidget = new ZObjDetailedInfoWidget(m_doc, this);
  if (m_doc.viewSettingId() > 0) {
    m_objDetailedInfoWidget->showWidgetOfObj(m_doc.viewSettingId());
  }
  m_objectDetailedInfoDockWidget->setWidget(m_objDetailedInfoWidget);
  addDockWidget(Qt::RightDockWidgetArea, m_objectDetailedInfoDockWidget);
  m_windowMenu->addAction(m_objectDetailedInfoDockWidget->toggleViewAction());
  m_objectDetailedInfoDockWidget->setVisible(false);

  m_globalSettingDockWidget = new QDockWidget(tr("Global View Setting"), this);
  m_globalSettingDockWidget->setFeatures(QDockWidget::DockWidgetClosable | QDockWidget::DockWidgetMovable |
                                         QDockWidget::DockWidgetFloatable);
  m_globalSettingDockWidget->setAllowedAreas(Qt::RightDockWidgetArea);
  addDockWidget(Qt::RightDockWidgetArea, m_globalSettingDockWidget);
  m_windowMenu->addAction(m_globalSettingDockWidget->toggleViewAction());
  tabifyDockWidget(m_viewSettingDockWidget, m_globalSettingDockWidget);

  m_traceDockWidget = new QDockWidget(tr("Trace Settings"), this);
  m_traceDockWidget->setFeatures(QDockWidget::DockWidgetClosable | QDockWidget::DockWidgetMovable |
                                 QDockWidget::DockWidgetFloatable);
  m_traceDockWidget->setAllowedAreas(Qt::RightDockWidgetArea);
  m_traceDockWidget->setWidget(new ZTraceSettingsWidget(m_doc, m_traceDockWidget));
  addDockWidget(Qt::RightDockWidgetArea, m_traceDockWidget);
  tabifyDockWidget(m_globalSettingDockWidget, m_traceDockWidget);
  auto* traceToggle = m_traceDockWidget->toggleViewAction();
  traceToggle->setIcon(ZTheme::instance().icon(ZTheme::TraceIcon));
  m_windowMenu->addAction(traceToggle);
  m_traceDockWidget->setVisible(false);

  m_tasksDockWidget = new QDockWidget(tr("Tasks"), this);
  m_tasksDockWidget->setFeatures(QDockWidget::DockWidgetClosable | QDockWidget::DockWidgetMovable |
                                 QDockWidget::DockWidgetFloatable);
  m_tasksDockWidget->setAllowedAreas(Qt::RightDockWidgetArea);
  m_tasksDockWidget->setWidget(new ZBackgroundTaskManagerWidget(m_doc.backgroundTaskManager(), m_tasksDockWidget));
  addDockWidget(Qt::RightDockWidgetArea, m_tasksDockWidget);
  tabifyDockWidget(m_traceDockWidget, m_tasksDockWidget);
  m_windowMenu->addAction(m_tasksDockWidget->toggleViewAction());
  m_tasksDockWidget->setVisible(false);

  connect(&m_doc, &ZDoc::showBackgroundTasksPanel, this, [this]() {
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
  addDockWidget(Qt::RightDockWidgetArea, m_captureDockWidget);
  m_windowMenu->addAction(m_captureDockWidget->toggleViewAction());
  m_captureDockWidget->setVisible(false);

  m_helpDockWidget = new QDockWidget(tr("Help"), this);
  m_helpDockWidget->setFeatures(QDockWidget::DockWidgetClosable | QDockWidget::DockWidgetMovable |
                                QDockWidget::DockWidgetFloatable);
  m_helpDockWidget->setVisible(false);
  addDockWidget(Qt::LeftDockWidgetArea, m_helpDockWidget);
  m_windowMenu->addAction(m_helpDockWidget->toggleViewAction());
  m_helpDockWidget->setFloating(true);

  m_backgroundDockWidget = new QDockWidget(tr("Background"), this);
  m_backgroundDockWidget->setFeatures(QDockWidget::DockWidgetClosable | QDockWidget::DockWidgetMovable |
                                      QDockWidget::DockWidgetFloatable);
  m_backgroundDockWidget->setAllowedAreas(Qt::RightDockWidgetArea);
  addDockWidget(Qt::RightDockWidgetArea, m_backgroundDockWidget);
  m_windowMenu->addAction(m_backgroundDockWidget->toggleViewAction());
  m_backgroundDockWidget->setVisible(false);

  m_axisDockWidget = new QDockWidget(tr("Axis"), this);
  m_axisDockWidget->setFeatures(QDockWidget::DockWidgetClosable | QDockWidget::DockWidgetMovable |
                                QDockWidget::DockWidgetFloatable);
  m_axisDockWidget->setAllowedAreas(Qt::RightDockWidgetArea);
  addDockWidget(Qt::RightDockWidgetArea, m_axisDockWidget);
  m_windowMenu->addAction(m_axisDockWidget->toggleViewAction());
  m_axisDockWidget->setVisible(false);

  m_editObjDockWidget = new QDockWidget(tr("Edit and Output"), this);
  m_editObjDockWidget->setFeatures(QDockWidget::DockWidgetClosable);
  m_editObjDockWidget->setAllowedAreas(Qt::BottomDockWidgetArea);
  m_objEditWidget = new ZObjEditWidget(m_doc, this);
  m_editObjDockWidget->setWidget(m_objEditWidget);
  addDockWidget(Qt::BottomDockWidgetArea, m_editObjDockWidget);
  m_windowMenu->addAction(m_editObjDockWidget->toggleViewAction());
  m_editObjDockWidget->setVisible(false);
}

void Z3DMainWindow::fillDockWindows()
{
  CHECK(m_engine);
  CHECK(m_objectsDockWidget);
  CHECK(m_viewSettingDockWidget);
  CHECK(m_globalSettingDockWidget);
  CHECK(m_captureDockWidget);
  CHECK(m_backgroundDockWidget);
  CHECK(m_axisDockWidget);
  CHECK(m_helpDockWidget);

  ZObjWidget* objWidget = m_doc.createObjWidget(this);
  m_objectsDockWidget->setWidget(objWidget);

  m_viewSettingWidget = new ZViewSettingWidget(m_doc, m_engine, this);
  if (m_doc.viewSettingId() > 0) {
    m_viewSettingWidget->showViewSettingWidgetOfObj(m_doc.viewSettingId());
  }
  m_viewSettingDockWidget->setWidget(m_viewSettingWidget);

  m_globalSettingDockWidget->setWidget(m_engine->globalParasWidget());

  m_captureDockWidget->setWidget(createCaptureWidget());

  m_backgroundDockWidget->setWidget(m_engine->backgroundWidget());

  m_axisDockWidget->setWidget(m_engine->axisWidget());

  m_helpDockWidget->setWidget(createHelpWidget());
}

void Z3DMainWindow::readSettings()
{
  QSettings settings;
  QPoint pos = settings.value("pos3d", QPoint(200, 200)).toPoint();
  QSize size = settings.value("size3d", QSize(400, 400)).toSize();
  // VLOG(1) << pos << " " << size.width() << " " <<  size.height();
  move(pos);
  resize(size);
}

void Z3DMainWindow::writeSettings()
{
  QSettings settings;
  settings.setValue("pos3d", pos());
  settings.setValue("size3d", size());
}

bool Z3DMainWindow::maybeSave()
{
  if (false) {
    QMessageBox::StandardButton ret;
    ret = QMessageBox::warning(this,
                               QApplication::applicationName(),
                               tr("This workspace has been modified.\n"
                                  "Do you want to save your changes?"),
                               QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel);
    if (ret == QMessageBox::Save) {
      return save();
    } else if (ret == QMessageBox::Cancel) {
      return false;
    }
  }
  return true;
}

void Z3DMainWindow::loadWorkspace(const QString& fileName)
{
  setCurrentFile(fileName);
  statusBar()->showMessage(tr("Workspace loaded"), 2000);
  activateWindowIfNot();
}

bool Z3DMainWindow::saveFile(const QString& fileName)
{
  setCurrentFile(fileName);
  statusBar()->showMessage(tr("Workspace saved"), 2000);
  return true;
}

void Z3DMainWindow::setCurrentFile(const QString& fileName)
{
  static int sequenceNumber = 1;

  if (fileName.isEmpty()) {
    setWindowFilePath(tr("Workspace%1").arg(sequenceNumber++));
  } else {
    setWindowFilePath(QFileInfo(fileName).canonicalFilePath());
  }

  setWindowModified(false);
}

void Z3DMainWindow::onViewReady()
{
  fillDockWindows();
  Q_EMIT viewReady(m_engine);
}

QWidget* Z3DMainWindow::createCaptureWidget() const
{
  auto m_screenShotWidget = new ZTakeScreenShotWidget(false, false, nullptr);
  m_screenShotWidget->setCaptureStereoImage(m_isStereoView);
  connect(m_screenShotWidget, &ZTakeScreenShotWidget::take3DScreenShot, m_engine, &Z3DRenderingEngine::takeScreenShot);
  connect(m_screenShotWidget,
          &ZTakeScreenShotWidget::takeFixedSize3DScreenShot,
          m_engine,
          &Z3DRenderingEngine::takeFixedSizeScreenShot);

  return m_screenShotWidget;
}

void Z3DMainWindow::initRenderingEngine()
{
  QMetaObject::invokeMethod(m_engine, &Z3DRenderingEngine::initAndAttachToCanvas, m_canvas);
}

void Z3DMainWindow::onRenderingError(const QString& error)
{
  showCriticalWithDetails(this, tr("Rendering error"), error);
}

QWidget* Z3DMainWindow::createHelpWidget()
{
  auto edt = new QPlainTextEdit();
  edt->setReadOnly(true);
  edt->appendPlainText("zoom/dolly:");
  edt->appendPlainText("    1) mouse wheel scroll");
  edt->appendPlainText("    2) [(optional) command/control key] + =(+)/- key");
  edt->appendPlainText("rotate:");
  edt->appendPlainText("    1) [(optional) command/control key] + mouse drag");
  edt->appendPlainText("    2) command/control key + Left/Right/Up/Down key");
  edt->appendPlainText("shift:");
  edt->appendPlainText("    1) shift key + mouse drag");
  edt->appendPlainText("    2) shift key + Left/Right/Up/Down key");
  edt->appendPlainText("roll:");
  edt->appendPlainText("    1) alt key + mouse drag");
  edt->appendPlainText("    2) alt key + Left/Right key");
  edt->moveCursor(QTextCursor::Start);
  edt->ensureCursorVisible();
  return edt;
}

void Z3DMainWindow::onProgressChanged(int v)
{
  if (v == 100) {
    if (m_progressBarAction->isVisible()) {
      m_progressBarAction->setVisible(false);
      m_cancelAction->setVisible(false);
    }
  } else {
    if (!m_progressBarAction->isVisible()) {
      m_progressBarAction->setVisible(true);
      m_cancelAction->setVisible(true);
    }
    m_progressBarWidget->setValue(v);
  }
}

void Z3DMainWindow::zoomIn()
{
  m_engine->cancelLongRendering();
  m_engine->zoomIn();
}

void Z3DMainWindow::zoomOut()
{
  m_engine->cancelLongRendering();
  m_engine->zoomOut();
}

void Z3DMainWindow::resetCamera()
{
  m_engine->cancelLongRendering();
  m_engine->resetCamera();
}

void Z3DMainWindow::cancelRendering()
{
  m_engine->cancelLongRendering();
}

// 3D window uses shared actions from 2D; no local doc loader needed.

} // namespace nim
