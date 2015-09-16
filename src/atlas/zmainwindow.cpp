#include "z3dcanvas.h"
#include "zsysteminfo.h"

#include <QFileDialog>
#include <QMessageBox>
#include <QAction>
#include <QActionGroup>
#include <QToolBar>
#include <QMenu>
#include <QModelIndex>
#include <QJsonDocument>
#include <QSettings>
#include <QApplication>
#include <QDockWidget>
#include <QMimeData>
#include <QMenuBar>
#include <QToolBar>
#include <QToolButton>
#include <QStatusBar>
#include <QDesktopServices>

#include "zmainwindow.h"
#include "zdoc.h"
#include "zview.h"
#ifdef _WITH_TESTS_
#include "zunittest.h"
#endif
#include "zcustomcommand.h"
#include "zviewsettingwidget.h"
#include "z3dmainwindow.h"
#include "z3dview.h"
#include "zobjwidget.h"
#include "zobjeditwidget.h"

#include "zimgdoc.h"
#include "zimgview.h"
#include "zpunctadoc.h"
#include "zpunctaview.h"
#include "zswcdoc.h"
#include "zswcview.h"
#include "zmeshdoc.h"
//#include "zmeshview.h"
#include "z2danimationdoc.h"
#include "z3danimationdoc.h"
#include "zroiview.h"
#include "zregionannotationdoc.h"
#include "zregionannotationview.h"

namespace nim {

ZMainWindow::ZMainWindow()
  : QMainWindow()
  , m_3dWindow(nullptr)
  , m_isClosed(false)
{
  init();
  setCurrentFile("");
}

void ZMainWindow::initOpenglContext()
{
#ifndef _QT4_
  m_sharedContext = new Z3DCanvas("Init Canvas", 32, 32, this);
  m_sharedContext->show();

  // initialize OpenGL
  if (!ZSystemInfoInstance.initializeGL()) {
    QString msg = ZSystemInfoInstance.errorMessage();
    msg += ". 3D functions will be disabled.";
    QMessageBox::warning(this, "OpenGL Initialization", msg);
  }

  ZSystemInfoInstance.setStereoSupported(m_sharedContext->format().stereo());
  m_sharedContext->hide();
#else
  // init openGL context
  QGLFormat format = QGLFormat();
  format.setAlpha(true);
  format.setDepth(true);
  format.setDoubleBuffer(true);
  format.setRgba(true);
  format.setSampleBuffers(true);
  format.setStereo(true);
  //if (QSysInfo::MacintoshVersion >= QSysInfo::MV_LION) {
    //format.setVersion(3, 2);
    //format.setProfile(QGLFormat::CoreProfile);
  //}
  m_sharedContext = new Z3DCanvas("Init Canvas", 32, 32, format, this);

  // initGL requires a valid OpenGL context
  if (m_sharedContext) {
    // initialize OpenGL
    if (!Z3DApplicationInstance.initializeGL()) {
      QString msg = Z3DApplicationInstance.errorMessage();
      msg += ". 3D functions will be disabled.";
      QMessageBox::warning(this, "OpenGL Initialization", msg);
    }

    Z3DApplicationInstance.setStereoSupported(m_sharedContext->format().stereo());
    m_sharedContext->hide();
  }
#endif
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
  if (doc->objUndoStack(id))
    doc->objUndoStack(id)->setActive();
  else
    m_doc->activateEmptyUndoStack();
  if (!doc->typeName().contains("3D", Qt::CaseInsensitive)) {
    m_objEditWidget->showObjEditWidgetOfObj(id);
    m_editObjDockWidget->setVisible(true);
  } else {
    //m_editObjDockWidget->setVisible(false);
    if (!m_3dWindow) {
      open3DWindow();
    }
    m_3dWindow->openEditWidget(id);
  }
}

void ZMainWindow::closeEvent(QCloseEvent *event)
{
  // Qt 5.4 mac bug, use dock icon context menu -> quit will call this function twice and crash
  if (m_isClosed) {
    event->accept();
    return;
  }

  if (m_3dWindow) {
    delete m_3dWindow;
    m_3dWindow = nullptr;
  }

  if (maybeSave()) {
    writeSettings();
    event->accept();
    // otherwise it is very slow to close the application
    delete m_view;
    delete m_doc;
    m_isClosed = true;
  } else {
    event->ignore();
  }
}

void ZMainWindow::dragEnterEvent(QDragEnterEvent *event)
{
  if (event->mimeData()->hasFormat("text/uri-list")) {
    event->acceptProposedAction();
  }
}

void ZMainWindow::dropEvent(QDropEvent *event)
{
  QList<QUrl> urlList = event->mimeData()->urls();
  QStringList fileList;
  for (QList<QUrl>::const_iterator iter = urlList.begin(); iter != urlList.end(); ++iter) {
    // load files inside if is folder
    QFileInfo dirCheck(iter->toLocalFile());
    if (dirCheck.isDir()) {
      QDir dir = dirCheck.absoluteDir();
      QFileInfoList list = dir.entryInfoList(QDir::Files | QDir::NoSymLinks);
      for (int i=0; i<list.size(); i++) {
        fileList.append(list.at(i).canonicalFilePath());
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

void ZMainWindow::newWindow()
{
  ZMainWindow *other = new ZMainWindow();
  other->move(x() + 40, y() + 40);
  other->show();
}

void ZMainWindow::open()
{
  QString fileName = QFileDialog::getOpenFileName(this, tr("Open Workspace"),
                                                  "/");
  if (!fileName.isEmpty()) {
    ZMainWindow *existing = findMainWindow(fileName);
    if (existing) {
      existing->show();
      existing->raise();
      existing->activateWindow();
      return;
    }

    if (false) {   // if is empty todo
      loadWorkspace(fileName);
    } else {
      ZMainWindow *other = new ZMainWindow();
      other->move(x() + 40, y() + 40);
      other->show();
      other->loadWorkspace(fileName);
    }
  }
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
  QAction *action = qobject_cast<QAction*>(sender());
  if (action) {
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
                     QString("<p>Atlas ver. %1</p>"
                             "<p>Atlas is developed by Linqing Feng (flq@live.com).</p>"
                             "<p>Jinny Kim's Lab, Center for Functional Connectomics, Korea Institute of Science and Technology</p>"
                             "<p>All rights reserved.</p>").arg(__DATE__));
}

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

void ZMainWindow::openLogFolder()
{
  QDesktopServices::openUrl(QUrl::fromLocalFile(ZSystemInfoInstance.logDir().absolutePath()));
}

#ifdef _WITH_TESTS_
void ZMainWindow::runUnitTest()
{
  ZUnitTest::run(0, nullptr);
}
#endif

void ZMainWindow::runCustomCommand()
{
  ZCustomCommand::run();
}

void ZMainWindow::open3DWindow()
{
  if (ZSystemInfoInstance.is3DSupported()) {
    try {
      if (!m_3dWindow) {
        m_3dWindow = new Z3DMainWindow(m_doc, *this, false);
        m_3dWindow->setWindowTitle("3D View");
        connect(m_3dWindow, SIGNAL(loadScene()), this, SLOT(loadScene()));
        connect(m_3dWindow, SIGNAL(saveScene()), this, SLOT(saveScene()));
        connect(m_3dWindow, SIGNAL(loadJsonScene(QString)), this, SLOT(loadJsonScene(QString)));
        connect(m_3dWindow, SIGNAL(destroyed()), this, SLOT(detach3DWindow()));
        QApplication::processEvents();
        m_doc->animation3DDoc().bindView(m_3dWindow->view());
      }

      m_3dWindow->show();
      m_3dWindow->raise();
    }
    catch (const ZException & e) {
      LERROR() << "Failed to open 3D window:" << e.what();
      QMessageBox::critical(this, tr("Failed to open 3D window"), e.what());
      delete m_3dWindow;
      m_3dWindow = nullptr;
    }
  } else {
    QMessageBox::critical(this, tr("3D functions are disabled"),
                          ZSystemInfoInstance.errorMessage());
  }
}

void ZMainWindow::detach3DWindow()
{
  m_3dWindow = nullptr;
}

void ZMainWindow::loadScene()
{
  QString fn = QFileDialog::getOpenFileName(QApplication::activeWindow(), "Load scene", m_doc->lastOpenedFilePath(),
                                            tr("Scene file (*.scene)"));
  if (!fn.isEmpty()) {
    QString err;
    if (!loadJsonScene(fn, err)) {
      QMessageBox::critical(QApplication::activeWindow(), "Can not load scene",
                            tr("Can not load scene %1: %2").arg(fn).arg(err));
    } else {
      m_doc->setLastOpenedFilePath(fn);
      ZSystemInfoInstance.addFileToRecentFileList(fn);
    }
  }
}

void ZMainWindow::saveScene()
{
  if (!m_doc->saveAllObjs()) {
    QMessageBox::critical(QApplication::activeWindow(), "Can not save scene",
                          tr("Can not save scene because some objects have unsaved changes"));
    return;
  }

  QString fn = QFileDialog::getSaveFileName(QApplication::activeWindow(), "Save scene to file", m_doc->lastOpenedFilePath(),
                                            tr("Scene file (*.scene)"));
  if (!fn.isEmpty()) {
    QString err;
    if (!saveJsonScene(fn, err)) {
      QMessageBox::critical(QApplication::activeWindow(), "Can not save scene",
                            tr("Can not save scene %1: %2").arg(fn).arg(err));
    } else {
      m_doc->setLastOpenedFilePath(fn);
      ZSystemInfoInstance.addFileToRecentFileList(fn);
      QMessageBox::information(QApplication::activeWindow(),
                               "scene saved",
                               QString("scene saved as %1").arg(fn),
                               QMessageBox::Ok);
    }
  }
}

void ZMainWindow::loadJsonScene(const QString &fn)
{
  QString err;
  if (!loadJsonScene(fn, err)) {
    QMessageBox::critical(QApplication::activeWindow(), "Can not load scene",
                          tr("Can not load scene %1: %2").arg(fn).arg(err));
  } else {
    if (!err.isEmpty()) {
      QMessageBox::critical(QApplication::activeWindow(), "Scene loaded with error",
                            tr("Error while loading scene %1: %2").arg(fn).arg(err));
    }
    ZSystemInfoInstance.addFileToRecentFileList(fn);
  }
}

void ZMainWindow::init()
{
  setAttribute(Qt::WA_DeleteOnClose);
  setAcceptDrops(true);

  m_doc = new ZDoc(this);
  m_view = new ZView(*m_doc, this);

  //packages
  ZImgView *imgView = new ZImgView(m_doc->imgDoc(), *m_view);
  m_view->registerObjView(imgView);

  ZROIView *roiView = new ZROIView(m_doc->roiDoc(), *m_view);
  m_view->registerObjView(roiView);

  ZPunctaDoc *punctaDoc = new ZPunctaDoc(*m_doc);
  m_doc->registerObjDoc(punctaDoc);
  ZPunctaView *punctaView = new ZPunctaView(*punctaDoc, *m_view);
  m_view->registerObjView(punctaView);

  ZSwcDoc *swcDoc = new ZSwcDoc(*m_doc);
  m_doc->registerObjDoc(swcDoc);
  ZSwcView *swcView = new ZSwcView(*swcDoc, *m_view);
  m_view->registerObjView(swcView);

  ZRegionAnnotationDoc *regionAnnotationDoc = new ZRegionAnnotationDoc(*m_doc);
  m_doc->registerObjDoc(regionAnnotationDoc);
  ZRegionAnnotationView *regionAnnotationView = new ZRegionAnnotationView(*regionAnnotationDoc, *m_view);
  m_view->registerObjView(regionAnnotationView);

  // UI
  setCentralWidget(m_view);

  m_doc->animation2DDoc().bindView(m_view);

  createActions();
  createMenus();
  createToolBars();
  createStatusBar();
  createDockWindows();

  readSettings();

  //const QList<QAction*> &loadActList = m_doc->loadFileActions();
  //for (int i=0; i<loadActList.size(); ++i)
    //connect(loadActList[i], SIGNAL(triggered()), this, SLOT(activateWindowIfNot()));
}

void ZMainWindow::createActions()
{
  // file
  m_newAction = new QAction(QIcon(":/icons/file-512.png"), tr("&New"), this);
  m_newAction->setShortcuts(QKeySequence::New);
  m_newAction->setStatusTip(tr("Open a new window"));
  connect(m_newAction, SIGNAL(triggered()), this, SLOT(newWindow()));

  m_openAction = new QAction(QIcon(":/icons/folder-512.png"), tr("&Open..."), this);
  m_openAction->setShortcuts(QKeySequence::Open);
  m_openAction->setStatusTip(tr("Open an existing file"));
  connect(m_openAction, SIGNAL(triggered()), this, SLOT(open()));

  m_saveAction = new QAction(QIcon(":/icons/save-512.png"), tr("&Save"), this);
  m_saveAction->setShortcuts(QKeySequence::Save);
  m_saveAction->setStatusTip(tr("Save the document to disk"));
  connect(m_saveAction, SIGNAL(triggered()), this, SLOT(save()));

  m_saveAsAction = new QAction(QIcon(":/icons/save_as-512.png"), tr("Save &As..."), this);
  m_saveAsAction->setShortcuts(QKeySequence::SaveAs);
  m_saveAsAction->setStatusTip(tr("Save the document under a new name"));
  connect(m_saveAsAction, SIGNAL(triggered()), this, SLOT(saveAs()));

  m_loadSceneAction = new QAction(tr("Load &Scene..."), this);
  m_loadSceneAction->setStatusTip(tr("Load scene"));
  connect(m_loadSceneAction, SIGNAL(triggered()), this, SLOT(loadScene()));

  m_saveSceneAction = new QAction(tr("Save &Scene..."), this);
  m_saveSceneAction->setStatusTip(tr("Save scene"));
  connect(m_saveSceneAction, SIGNAL(triggered()), this, SLOT(saveScene()));

  m_closeAction = new QAction(tr("&Close"), this);
  m_closeAction->setShortcut(QKeySequence::Close);
  m_closeAction->setStatusTip(tr("Close this window"));
  connect(m_closeAction, SIGNAL(triggered()), this, SLOT(close()));

  for (int i = 0; i < ZSystemInfoInstance.maxNumRecentFiles(); ++i) {
    m_recentFileActions.push_back(new QAction(this));
    m_recentFileActions[i]->setVisible(false);
    connect(m_recentFileActions[i], SIGNAL(triggered()), this, SLOT(openRecentFile()));
  }

  // edit

  // view
  m_open3DViewAction = new QAction(tr("Open &3D Window"), this);
  m_open3DViewAction->setStatusTip(tr("Open 3D Window"));
  connect(m_open3DViewAction, SIGNAL(triggered()), this, SLOT(open3DWindow()));

  m_screenShotAction = new QAction(QIcon(":/icons/screenshot-512.png"), tr("&Screenshot"), this);
  m_screenShotAction->setStatusTip(tr("Screenshot"));
  connect(m_screenShotAction, SIGNAL(triggered()), this, SLOT(openScreenshotPanel()));

  m_exitAction = new QAction(tr("E&xit"), this);
  m_exitAction->setShortcuts(QKeySequence::Quit);
  m_exitAction->setStatusTip(tr("Exit the application"));
  connect(m_exitAction, SIGNAL(triggered()), qApp, SLOT(closeAllWindows()));

  m_aboutAction = new QAction(tr("&About"), this);
  m_aboutAction->setStatusTip(tr("Show the application's About box"));
  connect(m_aboutAction, SIGNAL(triggered()), this, SLOT(about()));

  m_aboutQtAction = new QAction(tr("About &Qt"), this);
  m_aboutQtAction->setStatusTip(tr("Show the Qt library's About box"));
  connect(m_aboutQtAction, SIGNAL(triggered()), qApp, SLOT(aboutQt()));

  //
  m_openLogFolderAction = new QAction(QIcon(":/icons/folder-512.png"), tr("&Open Log Folder"), this);
  m_openLogFolderAction->setStatusTip(tr("Open Log Folder"));
  connect(m_openLogFolderAction, SIGNAL(triggered()), this, SLOT(openLogFolder()));

#ifdef _WITH_TESTS_
  m_testAction = new QAction(QIcon(":/icons/test-512.png"), tr("&UnitTest"), this);
  m_testAction->setStatusTip(tr("Run Unit Test"));
  connect(m_testAction, SIGNAL(triggered()), this, SLOT(runUnitTest()));
#endif

  m_runCustomCommandAction = new QAction(QIcon(":/icons/run_command-512.png"), tr("&Run Custom Command"), this);
  m_runCustomCommandAction->setStatusTip(tr("Run Custom Command"));
  connect(m_runCustomCommandAction, SIGNAL(triggered()), this, SLOT(runCustomCommand()));
}

void ZMainWindow::createMenus()
{
  m_fileMenu = menuBar()->addMenu(tr("&File"));
  m_fileMenu->addAction(m_newAction);
  m_fileMenu->addAction(m_openAction);
  m_fileMenu->addAction(m_saveAction);
  m_fileMenu->addAction(m_saveAsAction);
  m_fileMenu->addSeparator();
  m_fileMenu->addAction(m_loadSceneAction);
  m_fileMenu->addAction(m_saveSceneAction);
  m_fileMenu->addSeparator();
  const QList<QAction*> &fileActList = m_doc->fileActions();
  for (int i=0; i<fileActList.size(); ++i)
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

  const QList<QMenu*> &menuList = m_doc->processObjMenu();
  for (int i=0; i<menuList.size(); ++i) {
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
  m_helpMenu->addSeparator();
  m_helpMenu->addAction(m_openLogFolderAction);
#ifdef _WITH_TESTS_
  m_helpMenu->addAction(m_testAction);
#endif
  m_helpMenu->addAction(m_runCustomCommandAction);
}

void ZMainWindow::createToolBars()
{
  QSize iconSize(22,22);
  m_fileToolBar = addToolBar(tr("File"));
  m_fileToolBar->addAction(m_newAction);
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

  m_helpToolBar = addToolBar(tr("Help"));
  m_helpToolBar->addAction(m_openLogFolderAction);
  //#ifdef _WITH_TESTS_
  //  m_helpToolBar->addAction(m_testAction);
  //#endif
  //  m_helpToolBar->addAction(m_runCustomCommandAction);
  m_helpToolBar->setIconSize(iconSize);
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
  ZObjWidget *objWidget = m_doc->createObjWidget(this);
  m_objectsDockWidget->setWidget(objWidget);
  connect(m_doc, SIGNAL(openEditWidget(size_t)), this, SLOT(openEditWidget(size_t)));
  addDockWidget(Qt::RightDockWidgetArea, m_objectsDockWidget);
  m_windowMenu->addAction(m_objectsDockWidget->toggleViewAction());

  m_viewSettingDockWidget = new QDockWidget(tr("Object View Setting"), this);
  m_viewSettingDockWidget->setFeatures(QDockWidget::DockWidgetClosable);
  m_viewSettingDockWidget->setAllowedAreas(Qt::RightDockWidgetArea);
  m_viewSettingDockWidget->setWidget(new ZViewSettingWidget(m_doc, m_view, this));
  addDockWidget(Qt::RightDockWidgetArea, m_viewSettingDockWidget);
  m_windowMenu->addAction(m_viewSettingDockWidget->toggleViewAction());

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
  m_objEditWidget = new ZObjEditWidget(m_doc, this);
  m_editObjDockWidget->setWidget(m_objEditWidget);
  addDockWidget(Qt::BottomDockWidgetArea, m_editObjDockWidget);
  m_windowMenu->addAction(m_editObjDockWidget->toggleViewAction());
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

void ZMainWindow::loadWorkspace(const QString &fileName)
{
  setCurrentFile(fileName);
  statusBar()->showMessage(tr("Workspace loaded"), 2000);
  activateWindowIfNot();
}

bool ZMainWindow::saveFile(const QString &fileName)
{
  setCurrentFile(fileName);
  statusBar()->showMessage(tr("Workspace saved"), 2000);
  return true;
}

void ZMainWindow::setCurrentFile(const QString &fileName)
{
  static int sequenceNumber = 1;

  if (fileName.isEmpty()) {
    setWindowFilePath(tr("Workspace%1").arg(sequenceNumber++));
  } else {
    setWindowFilePath(QFileInfo(fileName).canonicalFilePath());
  }

  setWindowModified(false);
}

QString ZMainWindow::strippedName(const QString &fullFileName)
{
  return QFileInfo(fullFileName).fileName();
}

ZMainWindow *ZMainWindow::findMainWindow(const QString &)
{
  return 0;
}

bool ZMainWindow::loadJsonScene(const QString &fn, QString &err)
{
  QFile file(fn);
  if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
    err = tr("Can not open file");
    return false;
  }

  QByteArray saveData = file.readAll();

  QJsonDocument loadDoc(QJsonDocument::fromJson(saveData));
  if (loadDoc.isNull() || loadDoc.isEmpty() || !loadDoc.isObject()) {
    err = tr("File format is incorrect");
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
    LWARN() << "Scene" << fn << "contains zero objects";
  }

  for (QJsonObject::const_iterator it = sceneObj.begin();
       it != sceneObj.end(); ++it) {
    if (it.key() == "View2DGeneral") {
      m_view->read(it.value().toObject());
    } else if (it.key() == "View3DGeneral") {
      m_3dWindow->view()->read(it.value().toObject());
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
            m_3dWindow->view()->read(id, viewObj["View3D"].toObject());
          }
        }
      } else {
        err += QString("Unknown scene key %1\n").arg(it.key());
      }
    }
  }
  QApplication::processEvents();
  LINFO() << "Finish loading scene";

  return true;
}

bool ZMainWindow::saveJsonScene(const QString &fn, QString &err)
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
  for (int i=0; i<objs.size(); ++i) {
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
