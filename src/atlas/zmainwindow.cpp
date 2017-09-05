#include "zmainwindow.h"

#include "z3dcanvas.h"
#include "zsysteminfo.h"
#include "zdoc.h"
#include "zview.h"
#ifdef ATLAS_WITH_TESTS
#include "../../test/zrunbenchmark.h"
#include "../../test/zunittest.h"
#endif
#include "zcustomcommand.h"
#include "zviewsettingwidget.h"
#include "z3dmainwindow.h"
#include "z3dview.h"
#include "zobjwidget.h"
#include "zobjeditwidget.h"
#include "zobjdetailedinfowidget.h"
#include "zimgdoc.h"
#include "zimgview.h"
#include "zpunctadoc.h"
#include "zpunctaview.h"
#include "zswcdoc.h"
#include "zswcview.h"
#include "zmeshdoc.h"
#include "z2danimationdoc.h"
#include "z3danimationdoc.h"
#include "zroiview.h"
#include "zregionannotationdoc.h"
#include "zregionannotationview.h"
#include "zsvgdoc.h"
#include "zsvgview.h"
#include "zjson.h"
#include <QFileDialog>
#include <QMessageBox>
#include <QAction>
#include <QActionGroup>
#include <QToolBar>
#include <QMenu>
#include <QModelIndex>
#include <QSettings>
#include <QApplication>
#include <QDockWidget>
#include <QMimeData>
#include <QMenuBar>
#include <QToolBar>
#include <QToolButton>
#include <QStatusBar>
#include <QDesktopServices>
#include <QProcess>
#include <QTextStream>

namespace nim {

ZMainWindow::ZMainWindow(const QString& versionStr)
  : m_versionString(versionStr)
{
  init();
  setWindowTitle(QString("Atlas version %1").arg(m_versionString));
  //setCurrentFile("");
}

void ZMainWindow::initOpenglContext()
{
  m_sharedContext = new Z3DCanvas("Init Canvas", 32, 32, this);
  m_sharedContext->show();

  // initialize OpenGL
  if (!ZSystemInfo::instance().initializeGL()) {
    QString msg = ZSystemInfo::instance().errorMessage();
    msg += ". 3D functions will be disabled.";
    QMessageBox::warning(this, qApp->applicationName(), "OpenGL Initialization.\n" + msg);
  }

  ZSystemInfo::instance().setStereoSupported(m_sharedContext->format().stereo());
  m_sharedContext->hide();
}

void ZMainWindow::updateRecentFileActions()
{
  QSettings settings;
  QStringList files = settings.value("recentFileList").toStringList();

  int numRecentFiles = std::min(files.size(), m_recentFileActions.size());

  int idx = 0;
  for (int i = 0; i < numRecentFiles; ++i) {
    if (QFile::exists(files[i])) {
      QString text = tr("&%1 %2").arg(i + 1).arg(strippedName(files[i]));
      m_recentFileActions[idx]->setText(text);
      m_recentFileActions[idx]->setData(files[i]);
      m_recentFileActions[idx++]->setVisible(true);
    }
  }
  for (int j = idx; j < m_recentFileActions.size(); ++j)
    m_recentFileActions[j]->setVisible(false);

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
    //m_editObjDockWidget->setVisible(false);
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
  if (!fileList.isEmpty())
    m_doc->loadFileList(fileList);
}

void ZMainWindow::removeAllObjs()
{
  m_doc->removeAllObjs();
  QApplication::processEvents();
}

Z3DMainWindow* ZMainWindow::get3DWindow()
{
  return m_3dWindow.data();
}

void ZMainWindow::closeEvent(QCloseEvent* event)
{
  // Qt mac bug, use dock icon context menu -> quit will call this function twice and crash
  if (m_isClosed) {
    event->accept();
    return;
  }

  delete m_3dWindow.data();
  m_3dWindow.clear();

  if (maybeSave()) {
    writeSettings();
    event->accept();
    // otherwise it is very slow to close the application
    m_view.reset();
    m_doc.reset();
    m_isClosed = true;
  } else {
    event->ignore();
  }
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

void ZMainWindow::open()
{
}

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
  if (QAction* action = qobject_cast<QAction*>(sender())) {
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
  QMessageBox::about(this, QString("About Atlas"),
                     QString("<p>Atlas version %1</p>"
                               "<p>Atlas is developed by Linqing Feng (flq@live.com).</p>"
                               "<p>Jinny Kim Lab and Feng Lab, Center for Functional Connectomics, Korea Institute of Science and Technology</p>"
                               "<p>All rights reserved.</p>").arg(m_versionString));
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

    QMessageBox::information(this, QString("done"),
                             QString("Desktop entry created."));
    return;
  }

  QMessageBox::critical(this, QString("error"),
                        QString("Can not create desktop entry."));
}
#endif

void ZMainWindow::activateWindowIfNot()
{
  if (!isActiveWindow())
    this->activateWindow();
}

void ZMainWindow::openScreenshotPanel()
{
  if (m_captureDockWidget->isHidden()) {
    m_captureDockWidget->show();
  }
  m_captureDockWidget->raise();
}

void ZMainWindow::viewLog()
{
  //ZLogDialog logDialog(logModelSinkInstance(), this);
  //logDialog.exec();
  QStringList filters;
  filters << "atlas*_log.txt";
  QFileInfoList list = ZSystemInfo::instance().logDir().entryInfoList(filters,
                                                                  QDir::Files | QDir::NoSymLinks,
                                                                  QDir::Name);  // sorted by modification time
  if (!list.isEmpty()) {
    QDesktopServices::openUrl(QUrl::fromLocalFile(list.last().absoluteFilePath()));
  }
}

void ZMainWindow::openLogFolder()
{
  QDesktopServices::openUrl(QUrl::fromLocalFile(ZSystemInfo::instance().logDir().absolutePath()));
}

#ifdef ATLAS_WITH_TESTS

void ZMainWindow::runBenchmark()
{
  ZRunBenchmark::run();
}

void ZMainWindow::runUnitTest()
{
  if (ZUnitTest::run() == 0) {
    LOG(INFO) << "Unit Test Passed.";
  } else {
    LOG(ERROR) << "Unit Test Failed!";
  }
}

#endif

void ZMainWindow::runCustomCommand()
{
  ZCustomCommand::run();
}

void ZMainWindow::open3DWindow()
{
  if (ZSystemInfo::instance().is3DSupported()) {
    try {
      if (!m_3dWindow) {
        m_3dWindow = new Z3DMainWindow(m_doc.get(), *this, false);
        m_3dWindow->setWindowTitle(QString("3D View  %1").arg(windowTitle()));
        connect(m_3dWindow, &Z3DMainWindow::loadScene, this, &ZMainWindow::loadScene);
        connect(m_3dWindow, &Z3DMainWindow::saveScene, this, &ZMainWindow::saveScene);
        connect(m_3dWindow, &Z3DMainWindow::loadJsonScene, this, &ZMainWindow::loadJsonScene);
        QApplication::processEvents();
        m_doc->animation3DDoc().bindView(m_3dWindow->view());
      }

      m_3dWindow->showNormal();
      m_3dWindow->raise();
      m_3dWindow->activateWindow();
      //m_3dWindow->setWindowState(Qt::WindowActive);
    }
    catch (const ZException& e) {
      LOG(ERROR) << "Failed to open 3D window: " << e.what();
      QMessageBox::critical(this, qApp->applicationName(), "Failed to open 3D window.\n" + e.what());
      delete m_3dWindow.data();
      m_3dWindow.clear();
    }
  } else {
    QMessageBox::critical(this, qApp->applicationName(),
                          "3D functions are disabled.\n" + ZSystemInfo::instance().errorMessage());
  }
}

void ZMainWindow::loadScene()
{
  QString fn = QFileDialog::getOpenFileName(QApplication::activeWindow(), "Load scene",
                                            m_doc->lastOpenedFilePath(),
                                            tr("Scene file (*.scene)"));
  if (!fn.isEmpty()) {
    QString err;
    if (!loadJsonSceneImpl(fn, err)) {
      QMessageBox::critical(QApplication::activeWindow(), qApp->applicationName(),
                            tr("Can not load scene %1: %2").arg(fn).arg(err));
    } else {
      m_doc->setLastOpenedFilePath(fn);
      ZSystemInfo::instance().addFileToRecentFileList(fn);
    }
  }
}

void ZMainWindow::saveScene()
{
  if (!m_doc->saveAllObjs()) {
    QMessageBox::critical(QApplication::activeWindow(), qApp->applicationName(),
                          tr("Can not save scene because some objects have unsaved changes"));
    return;
  }

  QString fn = QFileDialog::getSaveFileName(QApplication::activeWindow(), "Save scene to file",
                                            m_doc->lastOpenedFilePath(),
                                            tr("Scene file (*.scene)"));
  if (!fn.isEmpty()) {
    QString err;
    if (!saveJsonSceneImpl(fn, err)) {
      QMessageBox::critical(QApplication::activeWindow(), qApp->applicationName(),
                            tr("Can not save scene %1: %2").arg(fn).arg(err));
    } else {
      m_doc->setLastOpenedFilePath(fn);
      ZSystemInfo::instance().addFileToRecentFileList(fn);
      QMessageBox::information(QApplication::activeWindow(),
                               qApp->applicationName(),
                               QString("scene saved as %1").arg(fn),
                               QMessageBox::Ok);
    }
  }
}

void ZMainWindow::loadJsonScene(const QString& fn)
{
  QString err;
  if (!loadJsonSceneImpl(fn, err)) {
    QMessageBox::critical(QApplication::activeWindow(), qApp->applicationName(),
                          tr("Can not load scene %1: %2").arg(fn).arg(err));
  } else {
    if (!err.isEmpty()) {
      QMessageBox::critical(QApplication::activeWindow(), qApp->applicationName(),
                            tr("Error while loading scene %1: %2").arg(fn).arg(err));
    }
    ZSystemInfo::instance().addFileToRecentFileList(fn);
  }
}

void ZMainWindow::openNewInstance()
{
#ifdef Q_OS_OSX
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
  setAttribute(Qt::WA_DeleteOnClose);
  setAcceptDrops(true);

  m_doc = std::make_unique<ZDoc>(this);
  m_view = std::make_unique<ZView>(*m_doc, this);

  //packages
  m_view->registerObjView(std::make_unique<ZImgView>(m_doc->imgDoc(), *m_view));

  m_view->registerObjView(std::make_unique<ZROIView>(m_doc->roiDoc(), *m_view));

  ZPunctaDoc* punctaDoc = new ZPunctaDoc(*m_doc);
  m_doc->registerObjDoc(punctaDoc);
  m_view->registerObjView(std::make_unique<ZPunctaView>(*punctaDoc, *m_view));

  ZSwcDoc* swcDoc = new ZSwcDoc(*m_doc);
  m_doc->registerObjDoc(swcDoc);
  m_view->registerObjView(std::make_unique<ZSwcView>(*swcDoc, *m_view));

  ZRegionAnnotationDoc* regionAnnotationDoc = new ZRegionAnnotationDoc(*m_doc);
  m_doc->registerObjDoc(regionAnnotationDoc);
  m_view->registerObjView(std::make_unique<ZRegionAnnotationView>(*regionAnnotationDoc, *m_view));

  ZSvgDoc* svgDoc = new ZSvgDoc(*m_doc);
  m_doc->registerObjDoc(svgDoc);
  m_view->registerObjView(std::make_unique<ZSvgView>(*svgDoc, *m_view));

  // UI
  setCentralWidget(m_view.get());

  m_doc->animation2DDoc().bindView(m_view.get());

  createActions();
  createMenus();
  createToolBars();
  createStatusBar();
  createDockWindows();

  readSettings();

  //const QList<QAction*> &loadActList = m_doc->loadFileActions();
  //for (int i=0; i<loadActList.size(); ++i)
  //connect(loadActList[i], &QAction::triggered, this, &ZMainWindow::activateWindowIfNot);
}

void ZMainWindow::createActions()
{
  // file
//  m_newAction = new QAction(QIcon(":/icons/file-512.png"), tr("&New"), this);
//  m_newAction->setShortcuts(QKeySequence::New);
//  m_newAction->setStatusTip(tr("Open a new window"));
//  connect(m_newAction, &QAction::triggered, this, &ZMainWindow::newWindow);

  m_openAction = new QAction(QIcon(":/icons/folder-512.png"), tr("&Open..."), this);
  m_openAction->setShortcuts(QKeySequence::Open);
  m_openAction->setStatusTip(tr("Open an existing scene file"));
  connect(m_openAction, &QAction::triggered, this, &ZMainWindow::loadScene);

  m_saveAction = new QAction(QIcon(":/icons/save-512.png"), tr("&Save"), this);
  m_saveAction->setShortcuts(QKeySequence::Save);
  m_saveAction->setStatusTip(tr("Save unsaved objects to disk"));
  connect(m_saveAction, &QAction::triggered, this, &ZMainWindow::save);

  m_saveAsAction = new QAction(QIcon(":/icons/save_as-512.png"), tr("Save &As..."), this);
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

  m_screenShotAction = new QAction(QIcon(":/icons/screenshot-512.png"), tr("&Screenshot"), this);
  m_screenShotAction->setStatusTip(tr("Screenshot"));
  connect(m_screenShotAction, &QAction::triggered, this, &ZMainWindow::openScreenshotPanel);

  m_exitAction = new QAction(tr("E&xit"), this);
  m_exitAction->setShortcuts(QKeySequence::Quit);
  m_exitAction->setStatusTip(tr("Exit the application"));
  connect(m_exitAction, &QAction::triggered, qApp, &QApplication::closeAllWindows);

  m_aboutAction = new QAction(tr("&About"), this);
  m_aboutAction->setStatusTip(tr("Show the application's About box"));
  connect(m_aboutAction, &QAction::triggered, this, &ZMainWindow::about);

  m_aboutQtAction = new QAction(tr("About &Qt"), this);
  m_aboutQtAction->setStatusTip(tr("Show the Qt library's About box"));
  connect(m_aboutQtAction, &QAction::triggered, qApp, &QApplication::aboutQt);

#ifdef Q_OS_LINUX
  m_createDesktopEntryAction = new QAction(tr("Create Desktop Entry..."), this);
  m_createDesktopEntryAction->setStatusTip(tr("Create Desktop Entry for Linux Desktop or Dock"));
  connect(m_createDesktopEntryAction, &QAction::triggered, this, &ZMainWindow::createDesktopEntry);
#endif

  //
  m_viewLogAction = new QAction(tr("&View Log"), this);
  m_viewLogAction->setStatusTip(tr("View Log"));
  connect(m_viewLogAction, &QAction::triggered, this, &ZMainWindow::viewLog);

  m_openLogFolderAction = new QAction(QIcon(":/icons/folder-512.png"), tr("&Open Log Folder"), this);
  m_openLogFolderAction->setStatusTip(tr("Open Log Folder"));
  connect(m_openLogFolderAction, &QAction::triggered, this, &ZMainWindow::openLogFolder);

#ifdef ATLAS_WITH_TESTS
  m_runBenchmarkAction = new QAction(QIcon(":/icons/run_command-512.png"), tr("&Run Benchmark"), this);
  m_runBenchmarkAction->setStatusTip(tr("Run Benchmark"));
  connect(m_runBenchmarkAction, &QAction::triggered, this, &ZMainWindow::runBenchmark);

  m_testAction = new QAction(QIcon(":/icons/test-512.png"), tr("&UnitTest"), this);
  m_testAction->setStatusTip(tr("Run Unit Test"));
  connect(m_testAction, &QAction::triggered, this, &ZMainWindow::runUnitTest);
#endif

  m_runCustomCommandAction = new QAction(QIcon(":/icons/run_command-512.png"), tr("&Run Custom Command"), this);
  m_runCustomCommandAction->setStatusTip(tr("Run Custom Command"));
  connect(m_runCustomCommandAction, &QAction::triggered, this, &ZMainWindow::runCustomCommand);

  m_openNewInstanceAction = new QAction(tr("Open Additional Instance of Atlas"), this);
  connect(m_openNewInstanceAction, &QAction::triggered, this, &ZMainWindow::openNewInstance);
}

void ZMainWindow::createMenus()
{
  m_fileMenu = menuBar()->addMenu(tr("&File"));
  //m_fileMenu->addAction(m_newAction);
  m_fileMenu->addAction(m_openAction);
  m_fileMenu->addAction(m_saveAction);
  m_fileMenu->addAction(m_saveAsAction);
  m_fileMenu->addSeparator();
  m_fileMenu->addAction(m_loadSceneAction);
  m_fileMenu->addAction(m_saveSceneAction);
  m_fileMenu->addSeparator();
  const QList<QAction*>& fileActList = m_doc->fileActions();
  for (int i = 0; i < fileActList.size(); ++i)
    m_fileMenu->addAction(fileActList[i]);
  m_separatorAction = m_fileMenu->addSeparator();
  for (int i = 0; i < m_recentFileActions.size(); ++i)
    m_fileMenu->addAction(m_recentFileActions[i]);
  m_fileMenu->addSeparator();
  m_fileMenu->addAction(m_closeAction);
  m_fileMenu->addAction(m_exitAction);
  updateRecentFileActions();

  m_editMenu = menuBar()->addMenu(tr("&Edit"));
  m_editMenu->addAction(m_doc->undoAction());
  m_editMenu->addAction(m_doc->redoAction());

  m_viewMenu = menuBar()->addMenu(tr("&View"));
  m_viewMenu->addAction(m_view->zoomInAction());
  m_viewMenu->addAction(m_view->zoomOutAction());
  m_viewMenu->addAction(m_view->fitIntoWindowAction());
  m_viewMenu->addSeparator();
  m_viewMenu->addAction(m_view->normalViewAction());
  m_viewMenu->addAction(m_view->maxZProjViewAction());
  m_viewMenu->addSeparator();
  m_viewMenu->addAction(m_open3DViewAction);
  m_viewMenu->addAction(m_screenShotAction);

  const QList<QMenu*>& menuList = m_doc->processObjMenu();
  for (int i = 0; i < menuList.size(); ++i) {
    menuBar()->addMenu(menuList[i]);
  }

  m_animationMenu = menuBar()->addMenu(tr("&Animation"));
  m_animationMenu->addAction(m_doc->make2DAnimationAction());
  m_animationMenu->addAction(m_doc->changeAnimationSettingAction());

  m_windowMenu = menuBar()->addMenu(tr("&Window"));

  menuBar()->addSeparator();

  m_helpMenu = menuBar()->addMenu(tr("&Help"));
  m_helpMenu->addAction(m_aboutAction);
  m_helpMenu->addAction(m_aboutQtAction);
#ifdef Q_OS_LINUX
  m_helpMenu->addAction(m_createDesktopEntryAction);
#endif
  m_helpMenu->addSeparator();
  m_helpMenu->addAction(m_viewLogAction);
  m_helpMenu->addAction(m_openLogFolderAction);
#ifdef ATLAS_WITH_TESTS
  m_helpMenu->addAction(m_runBenchmarkAction);
  m_helpMenu->addAction(m_testAction);
#endif
  m_helpMenu->addAction(m_runCustomCommandAction);

  m_dockMenu = new QMenu(this);
  m_dockMenu->addAction(m_openNewInstanceAction);
#ifdef Q_OS_OSX
  m_dockMenu->setAsDockMenu();
#endif
}

void ZMainWindow::createToolBars()
{
  QSize iconSize(22, 22);
  m_fileToolBar = addToolBar(tr("File"));
  //m_fileToolBar->addAction(m_newAction);
  m_fileToolBar->addAction(m_openAction);
  m_fileToolBar->addAction(m_saveAction);
  //const QList<QAction*> &loadFileActList = m_doc->loadFileActions();
  //for (int i=0; i<loadFileActList.size(); ++i)
  //m_fileToolBar->addAction(loadFileActList[i]);
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
  m_viewToolBar->addAction(m_open3DViewAction);
  m_viewToolBar->addAction(m_screenShotAction);
  m_viewToolBar->setIconSize(iconSize);

  m_dragModeToolBar = addToolBar(tr("Drag Mode"));
  m_dragModeToolBar->addAction(m_view->scrollHandDragAction());
  m_dragModeToolBar->addAction(m_view->rubberBandDragAction());
  m_dragModeToolBar->setIconSize(iconSize);

  m_roiToolBar = addToolBar(tr("ROI"));
  m_roiToolBar->addWidget(m_view->createROIToolButton(this));
  m_roiToolBar->setIconSize(iconSize);

  //m_helpToolBar = addToolBar(tr("Help"));
  //m_helpToolBar->addAction(m_openLogFolderAction);
  //#ifdef ATLAS_WITH_TESTS
  //  m_helpToolBar->addAction(m_testAction);
  //#endif
  //  m_helpToolBar->addAction(m_runCustomCommandAction);
  //m_helpToolBar->setIconSize(iconSize);
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
  m_viewSettingDockWidget->setWidget(new ZViewSettingWidget(m_doc.get(), m_view.get(), this));
  addDockWidget(Qt::RightDockWidgetArea, m_viewSettingDockWidget);
  m_windowMenu->addAction(m_viewSettingDockWidget->toggleViewAction());

  m_objectDetailedInfoDockWidget = new QDockWidget(tr("Object Detailed Info"), this);
  m_objectDetailedInfoDockWidget->setFeatures(QDockWidget::DockWidgetClosable |
                                              QDockWidget::DockWidgetMovable |
                                              QDockWidget::DockWidgetFloatable);
  m_objectDetailedInfoDockWidget->setAllowedAreas(Qt::RightDockWidgetArea);
  m_objectDetailedInfoDockWidget->setWidget(new ZObjDetailedInfoWidget(m_doc.get(), this));
  addDockWidget(Qt::RightDockWidgetArea, m_objectDetailedInfoDockWidget);
  m_windowMenu->addAction(m_objectDetailedInfoDockWidget->toggleViewAction());
  m_objectDetailedInfoDockWidget->setVisible(false);

  m_captureDockWidget = new QDockWidget(tr("Capture"), this);
  m_captureDockWidget->setFeatures(QDockWidget::DockWidgetClosable |
                                   QDockWidget::DockWidgetMovable |
                                   QDockWidget::DockWidgetFloatable);
  m_captureDockWidget->setAllowedAreas(Qt::RightDockWidgetArea);
  m_captureDockWidget->setWidget(m_view->captureWidget());
  addDockWidget(Qt::RightDockWidgetArea, m_captureDockWidget);
  m_windowMenu->addAction(m_captureDockWidget->toggleViewAction());
  m_captureDockWidget->setVisible(false);

  m_editObjDockWidget = new QDockWidget(tr("Edit and Output"), this);
  m_editObjDockWidget->setFeatures(QDockWidget::DockWidgetClosable);
  m_editObjDockWidget->setAllowedAreas(Qt::BottomDockWidgetArea);
  m_objEditWidget = new ZObjEditWidget(m_doc.get(), this);
  m_editObjDockWidget->setWidget(m_objEditWidget);
  addDockWidget(Qt::BottomDockWidgetArea, m_editObjDockWidget);
  m_windowMenu->addAction(m_editObjDockWidget->toggleViewAction());
  m_editObjDockWidget->setVisible(false);
}

void ZMainWindow::readSettings()
{
  QSettings settings;
  QPoint pos = settings.value("pos", QPoint(200, 200)).toPoint();
  QSize size = settings.value("size", QSize(400, 400)).toSize();
  move(pos);
  resize(size);
}

void ZMainWindow::writeSettings()
{
  QSettings settings;
  settings.setValue("pos", pos());
  settings.setValue("size", size());
}

bool ZMainWindow::maybeSave()
{
  return m_doc->saveOrDiscard(m_doc->objs());
}

//void ZMainWindow::loadWorkspace(const QString &fileName)
//{
//  setCurrentFile(fileName);
//  statusBar()->showMessage(tr("Workspace loaded"), 2000);
//  activateWindowIfNot();
//}

//bool ZMainWindow::saveFile(const QString &fileName)
//{
//  setCurrentFile(fileName);
//  statusBar()->showMessage(tr("Workspace saved"), 2000);
//  return true;
//}

//void ZMainWindow::setCurrentFile(const QString &fileName)
//{
//  static int sequenceNumber = 1;

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

ZMainWindow* ZMainWindow::findMainWindow(const QString& /*unused*/)
{
  return nullptr;
}

bool ZMainWindow::loadJsonSceneImpl(const QString& fn, QString& err)
{
  QFile file(fn);
  if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
    err = tr("Can not open file");
    return false;
  }

  QByteArray saveData = file.readAll();

  QJsonParseError jsonError;
  QJsonDocument loadDoc(QJsonDocument::fromJson(saveData, &jsonError));
  if (loadDoc.isNull() || loadDoc.isEmpty() || !loadDoc.isObject()) {
    err = QString("Incorrect file format <%1>").arg(jsonError.errorString());
    return false;
  }

  QJsonObject loadObj = loadDoc.object();
  if (!loadObj.contains("Scene") || !loadObj["Scene"].isObject()) {
    err = tr("File is not scene format");
    return false;
  }

  QJsonObject sceneObj = loadObj["Scene"].toObject();

  QDir::setCurrent(QFileInfo(fn).absolutePath());

  std::map<size_t, size_t> idmap = m_doc->read(sceneObj["Doc"].toObject(), err);
  QApplication::processEvents();
  if (sceneObj.contains("View3DGeneral")) {
    if (!m_3dWindow) {
      open3DWindow();
      QApplication::processEvents();
    }
  }

  if (idmap.empty()) {
    LOG(WARNING) << "Scene " << fn << " contains zero objects";
  }

  for (QJsonObject::const_iterator it = sceneObj.begin();
       it != sceneObj.end(); ++it) {
    if (it.key() == "View2DGeneral") {
      m_view->read(it.value().toObject());
    } else if (it.key() == "View3DGeneral") {
      if (m_3dWindow) {
        m_3dWindow->view()->read(it.value().toObject());
      }
    } else if (it.key() != "Doc" && it.key() != "Version") {
      bool ok;
      size_t objectId = it.key().toLongLong(&ok);
      if (ok) {
        if (idmap.find(objectId) != idmap.end()) {
          size_t id = idmap.at(objectId);
          QJsonObject viewObj = it.value().toObject();
          if (viewObj.contains("View2D")) {
            m_view->read(id, viewObj["View2D"].toObject());
          }
          if (viewObj.contains("View3D")) {
            if (!m_3dWindow) {
              open3DWindow();
              QApplication::processEvents();
            }
            if (m_3dWindow) {
              m_3dWindow->view()->read(id, viewObj["View3D"].toObject());
            }
          }
        }
      } else {
        err += QString("Unknown scene key %1\n").arg(it.key());
      }
    }
  }
  QApplication::processEvents();
  LOG(INFO) << "Finish loading scene";

  return true;
}

bool ZMainWindow::saveJsonSceneImpl(const QString& fn, QString& err)
{
  QFile file(fn);
  if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
    err = tr("Can not open file");
    return false;
  }

  QJsonObject sceneObj;
  sceneObj.insert("Version", QJsonValue(1.0));

  QJsonObject docObj;
  m_doc->write(docObj, true);
  sceneObj.insert("Doc", docObj);

  QList<size_t> objs = m_doc->objs();
  for (int i = 0; i < objs.size(); ++i) {
    size_t id = objs[i];
    QJsonObject jObj;

    QJsonObject view2DObj;
    m_view->write(id, view2DObj);
    jObj.insert("View2D", view2DObj);

    if (m_3dWindow) {
      QJsonObject view3DObj;
      m_3dWindow->view()->write(id, view3DObj);
      jObj.insert("View3D", view3DObj);
    }

    sceneObj.insert(QString("%1").arg(id), jObj);
  }

  QJsonObject view2DGeneralObj;
  m_view->write(view2DGeneralObj);
  sceneObj.insert("View2DGeneral", view2DGeneralObj);

  if (m_3dWindow) {
    QJsonObject view3DGeneralObj;
    m_3dWindow->view()->write(view3DGeneralObj);
    sceneObj.insert("View3DGeneral", view3DGeneralObj);
  }

  QJsonObject saveObj;
  saveObj.insert("Scene", sceneObj);

  QJsonDocument saveDoc(saveObj);
  if (file.write(saveDoc.toJson()) == -1) {
    err = file.errorString();
    return false;
  }

  return true;
}

} // namespace nim
