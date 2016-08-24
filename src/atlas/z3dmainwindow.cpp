#include "z3dgl.h"
#include "z3dmainwindow.h"

#include <QFileDialog>
#include <QMessageBox>
#include <QAction>
#include <QActionGroup>
#include <QDockWidget>
#include <QToolBar>
#include <QMenu>
#include <QMimeData>
#include <QMenuBar>
#include <QSettings>
#include <QStatusBar>
#include <QApplication>
#include <QDesktopServices>

#include "zdoc.h"
#include "z3dview.h"
#include "zviewsettingwidget.h"
#include "zobjdetailedinfowidget.h"
#include "z3dcanvas.h"
#include "zobjwidget.h"
#include "zobjeditwidget.h"
#include "zobjdoc.h"
#include "zmainwindow.h"
#include "zsysteminfo.h"

//#include "zlogdialog.h"

namespace nim {

Z3DMainWindow::Z3DMainWindow(ZDoc* doc, ZMainWindow& win2d, bool stereoView, QWidget* parent)
  : QMainWindow(parent)
  , m_doc(doc)
  , m_isStereoView(stereoView)
  , m_2dWindow(win2d)
{
  m_view = new Z3DView(m_doc, false, this);
  setCentralWidget(&m_view->canvas());
  init();
  setCurrentFile("");
}

void Z3DMainWindow::openEditWidget(size_t id)
{
  ZObjDoc* doc = m_doc->idToDoc(id);
  if (doc->objUndoStack(id))
    doc->objUndoStack(id)->setActive();
  else
    m_doc->activateEmptyUndoStack();
  if (!doc->typeName().contains("2D", Qt::CaseInsensitive)) {
    if (m_objEditWidget->showObjEditWidgetOfObj(id)) {
      m_editObjDockWidget->setVisible(true);
    }
  } else {
    //m_editObjDockWidget->setVisible(false);
    m_2dWindow.openEditWidget(id);
  }
}

void Z3DMainWindow::closeEvent(QCloseEvent* event)
{
  if (maybeSave()) {
    writeSettings();
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
  for (QList<QUrl>::const_iterator iter = urlList.begin(); iter != urlList.end(); ++iter) {
    // load files inside if is folder
    QFileInfo dirCheck(iter->toLocalFile());
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

void Z3DMainWindow::open()
{
}

bool Z3DMainWindow::save()
{
  return m_doc->saveAllObjs();
}

bool Z3DMainWindow::saveAs()
{
  return m_doc->saveSelectedObjsAs();
}

void Z3DMainWindow::openRecentFile()
{
  QAction* action = qobject_cast<QAction*>(sender());
  if (action) {
    QString fn = action->data().toString();
    if (fn.endsWith(".scene", Qt::CaseInsensitive)) {
      emit loadJsonScene(fn);
    } else {
      m_doc->loadFile(action->data().toString());
    }
  }
}

void Z3DMainWindow::about()
{
}

void Z3DMainWindow::activateWindowIfNot()
{
  if (!isActiveWindow())
    this->activateWindow();
}

void Z3DMainWindow::viewLog()
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

void Z3DMainWindow::openLogFolder()
{
  QDesktopServices::openUrl(QUrl::fromLocalFile(ZSystemInfo::instance().logDir().absolutePath()));
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

  readSettings();

  //const QList<QAction*> &loadActList = m_doc->loadFileActions();
  //for (int i=0; i<loadActList.size(); ++i)
  //connect(loadActList[i], &QAction::triggered, this, &Z3DMainWindow::activateWindowIfNot);
}

void Z3DMainWindow::createActions()
{
  // file
  m_openAction = new QAction(QIcon(":/icons/folder-512.png"), tr("&Open..."), this);
  m_openAction->setShortcuts(QKeySequence::Open);
  m_openAction->setStatusTip(tr("Open an existing scene file"));
  connect(m_openAction, &QAction::triggered, this, &Z3DMainWindow::loadScene);

  m_saveAction = new QAction(QIcon(":/icons/save-512.png"), tr("&Save"), this);
  m_saveAction->setShortcuts(QKeySequence::Save);
  m_saveAction->setStatusTip(tr("Save unsaved objects to disk"));
  connect(m_saveAction, &QAction::triggered, this, &Z3DMainWindow::save);

  m_saveAsAction = new QAction(QIcon(":/icons/save_as-512.png"), tr("Save &As..."), this);
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
  m_changeBackgroundAction = new QAction(QIcon(":/icons/background_color-512.png"), tr("&Change Background"), this);
  m_changeBackgroundAction->setStatusTip(tr("Change background of 3d view"));
  connect(m_changeBackgroundAction, &QAction::triggered, this, &Z3DMainWindow::changeBackground);

  m_changeAxisAction = new QAction(QIcon(":/icons/axis-512.png"), tr("&Change Axis"), this);
  m_changeAxisAction->setStatusTip(tr("Change axis of 3d view"));
  connect(m_changeAxisAction, &QAction::triggered, this, &Z3DMainWindow::changeAxis);

  m_screenShotAction = new QAction(QIcon(":/icons/screenshot-512.png"), tr("&Screenshot"), this);
  m_screenShotAction->setStatusTip(tr("Screenshot"));
  connect(m_screenShotAction, &QAction::triggered, this, &Z3DMainWindow::openScreenshotPanel);

  //
  m_exitAction = new QAction(tr("E&xit"), this);
  m_exitAction->setShortcuts(QKeySequence::Quit);
  m_exitAction->setStatusTip(tr("Exit the application"));
  connect(m_exitAction, &QAction::triggered, qApp, &QApplication::closeAllWindows);

  m_aboutAction = new QAction(tr("&About"), this);
  m_aboutAction->setStatusTip(tr("Show the application's About box"));
  connect(m_aboutAction, &QAction::triggered, this, &Z3DMainWindow::about);

  m_aboutQtAction = new QAction(tr("About &Qt"), this);
  m_aboutQtAction->setStatusTip(tr("Show the Qt library's About box"));
  connect(m_aboutQtAction, &QAction::triggered, qApp, &QApplication::aboutQt);

  //
  m_viewLogAction = new QAction(tr("&View Log"), this);
  m_viewLogAction->setStatusTip(tr("View Log"));
  connect(m_viewLogAction, &QAction::triggered, this, &Z3DMainWindow::viewLog);

  m_openLogFolderAction = new QAction(QIcon(":/icons/folder-512.png"), tr("&Open Log Folder"), this);
  m_openLogFolderAction->setStatusTip(tr("Open Log Folder"));
  connect(m_openLogFolderAction, &QAction::triggered, this, &Z3DMainWindow::openLogFolder);
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
  const QList<QAction*>& fileActList = m_doc->fileActions();
  for (int i = 0; i < fileActList.size(); ++i)
    m_fileMenu->addAction(fileActList[i]);
  m_separatorAction = m_fileMenu->addSeparator();
  const QList<QAction*>& recentFileActions = m_2dWindow.recentFileActions();
  for (int i = 0; i < recentFileActions.size(); ++i)
    m_fileMenu->addAction(recentFileActions[i]);
  m_fileMenu->addSeparator();
  m_fileMenu->addAction(m_closeAction);
  m_fileMenu->addAction(m_exitAction);

  m_editMenu = menuBar()->addMenu(tr("&Edit"));
  m_editMenu->addAction(m_doc->undoAction());
  m_editMenu->addAction(m_doc->redoAction());

  m_viewMenu = menuBar()->addMenu(tr("&View"));
  m_viewMenu->addAction(m_view->zoomInAction());
  m_viewMenu->addAction(m_view->zoomOutAction());
  m_viewMenu->addAction(m_view->resetCameraAction());
  m_viewMenu->addSeparator();
  m_viewMenu->addAction(m_changeBackgroundAction);
  m_viewMenu->addAction(m_changeAxisAction);
  m_viewMenu->addSeparator();
  m_viewMenu->addAction(m_screenShotAction);

  const QList<QMenu*>& menuList = m_doc->processObjMenu();
  for (int i = 0; i < menuList.size(); ++i) {
    menuBar()->addMenu(menuList[i]);
  }

  m_animationMenu = menuBar()->addMenu(tr("&Animation"));
  m_animationMenu->addAction(m_doc->make3DAnimationAction());
  m_animationMenu->addAction(m_doc->changeAnimationSettingAction());

  m_windowMenu = menuBar()->addMenu(tr("&Window"));

  menuBar()->addSeparator();

  m_helpMenu = menuBar()->addMenu(tr("&Help"));
  m_helpMenu->addAction(m_aboutAction);
  m_helpMenu->addAction(m_aboutQtAction);
  m_helpMenu->addSeparator();
  m_helpMenu->addAction(m_viewLogAction);
  m_helpMenu->addAction(m_openLogFolderAction);
}

void Z3DMainWindow::createToolBars()
{
  QSize iconSize(22, 22);
  m_fileToolBar = addToolBar(tr("File"));
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
  m_viewToolBar->addAction(m_view->resetCameraAction());
  m_viewToolBar->addAction(m_changeBackgroundAction);
  m_viewToolBar->addAction(m_changeAxisAction);
  m_viewToolBar->addAction(m_screenShotAction);
  m_viewToolBar->setIconSize(iconSize);

  //m_helpToolBar = addToolBar(tr("Help"));
  //m_helpToolBar->addAction(m_openLogFolderAction);
  //m_helpToolBar->setIconSize(iconSize);
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
  ZObjWidget* objWidget = m_doc->createObjWidget(this);
  m_objectsDockWidget->setWidget(objWidget);
  connect(m_doc, &ZDoc::openEditWidget, this, &Z3DMainWindow::openEditWidget);
  addDockWidget(Qt::RightDockWidgetArea, m_objectsDockWidget);
  m_windowMenu->addAction(m_objectsDockWidget->toggleViewAction());

  m_viewSettingDockWidget = new QDockWidget(tr("Object View Setting"), this);
  m_viewSettingDockWidget->setFeatures(QDockWidget::DockWidgetClosable);
  m_viewSettingDockWidget->setAllowedAreas(Qt::RightDockWidgetArea);
  m_viewSettingWidget = new ZViewSettingWidget(m_doc, m_view, this);
  if (m_doc->viewSettingId() > 0)
    m_viewSettingWidget->showViewSettingWidgetOfObj(m_doc->viewSettingId());
  m_viewSettingDockWidget->setWidget(m_viewSettingWidget);
  connect(m_doc, &ZDoc::showViewSetting, this, &Z3DMainWindow::raiseViewSettingDockWidget);
  addDockWidget(Qt::RightDockWidgetArea, m_viewSettingDockWidget);
  m_windowMenu->addAction(m_viewSettingDockWidget->toggleViewAction());

  m_objectDetailedInfoDockWidget = new QDockWidget(tr("Object Detailed Info"), this);
  m_objectDetailedInfoDockWidget->setFeatures(QDockWidget::DockWidgetClosable |
                                              QDockWidget::DockWidgetMovable |
                                              QDockWidget::DockWidgetFloatable);
  m_objectDetailedInfoDockWidget->setAllowedAreas(Qt::RightDockWidgetArea);
  m_objDetailedInfoWidget = new ZObjDetailedInfoWidget(m_doc, this);
  if (m_doc->viewSettingId() > 0)
    m_objDetailedInfoWidget->showWidgetOfObj(m_doc->viewSettingId());
  m_objectDetailedInfoDockWidget->setWidget(m_objDetailedInfoWidget);
  addDockWidget(Qt::RightDockWidgetArea, m_objectDetailedInfoDockWidget);
  m_windowMenu->addAction(m_objectDetailedInfoDockWidget->toggleViewAction());
  m_objectDetailedInfoDockWidget->setVisible(false);

  m_globalSettingDockWidget = new QDockWidget(tr("Global View Setting"), this);
  m_globalSettingDockWidget->setFeatures(QDockWidget::DockWidgetClosable |
                                         QDockWidget::DockWidgetMovable |
                                         QDockWidget::DockWidgetFloatable);
  m_globalSettingDockWidget->setAllowedAreas(Qt::RightDockWidgetArea);
  m_globalSettingDockWidget->setWidget(m_view->globalParasWidget());
  addDockWidget(Qt::RightDockWidgetArea, m_globalSettingDockWidget);
  m_windowMenu->addAction(m_globalSettingDockWidget->toggleViewAction());
  tabifyDockWidget(m_viewSettingDockWidget, m_globalSettingDockWidget);

  m_captureDockWidget = new QDockWidget(tr("Capture"), this);
  m_captureDockWidget->setFeatures(QDockWidget::DockWidgetClosable |
                                   QDockWidget::DockWidgetMovable |
                                   QDockWidget::DockWidgetFloatable);
  m_captureDockWidget->setAllowedAreas(Qt::RightDockWidgetArea);
  m_captureDockWidget->setWidget(m_view->captureWidget());
  addDockWidget(Qt::RightDockWidgetArea, m_captureDockWidget);
  m_windowMenu->addAction(m_captureDockWidget->toggleViewAction());
  m_captureDockWidget->setVisible(false);

  m_backgroundDockWidget = new QDockWidget(tr("Background"), this);
  m_backgroundDockWidget->setFeatures(QDockWidget::DockWidgetClosable |
                                      QDockWidget::DockWidgetMovable |
                                      QDockWidget::DockWidgetFloatable);
  m_backgroundDockWidget->setAllowedAreas(Qt::RightDockWidgetArea);
  m_backgroundDockWidget->setWidget(m_view->backgroundWidget());
  addDockWidget(Qt::RightDockWidgetArea, m_backgroundDockWidget);
  m_windowMenu->addAction(m_backgroundDockWidget->toggleViewAction());
  m_backgroundDockWidget->setVisible(false);

  m_axisDockWidget = new QDockWidget(tr("Axis"), this);
  m_axisDockWidget->setFeatures(QDockWidget::DockWidgetClosable |
                                QDockWidget::DockWidgetMovable |
                                QDockWidget::DockWidgetFloatable);
  m_axisDockWidget->setAllowedAreas(Qt::RightDockWidgetArea);
  m_axisDockWidget->setWidget(m_view->axisWidget());
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

void Z3DMainWindow::readSettings()
{
  QSettings settings;
  QPoint pos = settings.value("pos3d", QPoint(200, 200)).toPoint();
  QSize size = settings.value("size3d", QSize(400, 400)).toSize();
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
    ret = QMessageBox::warning(this, qApp->applicationName(),
                               tr("This workspace has been modified.\n"
                                    "Do you want to save your changes?"),
                               QMessageBox::Save | QMessageBox::Discard
                               | QMessageBox::Cancel);
    if (ret == QMessageBox::Save)
      return save();
    else if (ret == QMessageBox::Cancel)
      return false;
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

QString Z3DMainWindow::strippedName(const QString& fullFileName)
{
  return QFileInfo(fullFileName).fileName();
}

Z3DMainWindow* Z3DMainWindow::findMainWindow(const QString&)
{
  return 0;
}

} // namespace nim
