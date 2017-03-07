#pragma once

#include <QMainWindow>
#include <QPointer>

class QAction;

class QActionGroup;

class QMenu;

class QModelIndex;

namespace nim {

class ZDoc;

class ZView;

class Z3DMainWindow;

class ZObjEditWidget;

class Z3DCanvas;

class ZMainWindow : public QMainWindow
{
Q_OBJECT

public:
  explicit ZMainWindow(const QString& versionStr);

  void initOpenglContext();

  const QList<QAction*>& recentFileActions() const
  { return m_recentFileActions; }

  void updateRecentFileActions();

  void openEditWidget(size_t id);

  void loadUrls(const QList<QUrl>& urlList);

  void loadJsonScene(const QString& fn);

  void removeAllObjs();

  // might be nullptr
  Z3DMainWindow* get3DWindow();

protected:
  //void appAboutToQuit();

  void closeEvent(QCloseEvent* event);

  void dragEnterEvent(QDragEnterEvent* event);

  void dropEvent(QDropEvent* event);

private:
  //void newWindow();
  void open();

  bool save();

  bool saveAs();

  void openRecentFile();

  void about();

  void activateWindowIfNot();  //mac bug?

  void openScreenshotPanel();

  void viewLog();

  void openLogFolder();

#ifdef ATLAS_WITH_TESTS

  void runBenchmark();

  void runUnitTest();

#endif

  void runCustomCommand();

  void open3DWindow();

  void loadScene();

  void saveScene();

  void openNewInstance();

  void init();

  void createActions();

  void createMenus();

  void createToolBars();

  void createStatusBar();

  void createDockWindows();

  void readSettings();

  void writeSettings();

  bool maybeSave();

  //void loadWorkspace(const QString &fileName);
  //bool saveFile(const QString &fileName);
  //void setCurrentFile(const QString &fileName);
  QString strippedName(const QString& fullFileName);

  ZMainWindow* findMainWindow(const QString& fileName);

  bool loadJsonSceneImpl(const QString& fn, QString& err);

  bool saveJsonSceneImpl(const QString& fn, QString& err);

private:
  QMenu* m_fileMenu;
  QMenu* m_editMenu;
  QMenu* m_viewMenu;
  QMenu* m_animationMenu;
  QMenu* m_windowMenu;
  QMenu* m_helpMenu;
  QMenu* m_dockMenu;
  QToolBar* m_fileToolBar;
  QToolBar* m_editToolBar;
  QToolBar* m_viewToolBar;
  QToolBar* m_dragModeToolBar;
  QToolBar* m_roiToolBar;
  QToolBar* m_helpToolBar;

  //QAction *m_newAction;
  QAction* m_openAction;
  QAction* m_saveAction;
  QAction* m_saveAsAction;
  QAction* m_loadSceneAction;
  QAction* m_saveSceneAction;
  QAction* m_closeAction;

  QAction* m_exitAction;
  QAction* m_aboutAction;
  QAction* m_aboutQtAction;

  QAction* m_viewLogAction;
  QAction* m_openLogFolderAction;
#ifdef ATLAS_WITH_TESTS
  QAction* m_runBenchmarkAction;
  QAction* m_testAction;
#endif
  QAction* m_runCustomCommandAction;

  QAction* m_separatorAction;
  QList<QAction*> m_recentFileActions;

  QAction* m_open3DViewAction;
  QAction* m_screenShotAction;

  QAction* m_openNewInstanceAction;

  QDockWidget* m_objectsDockWidget;
  QDockWidget* m_viewSettingDockWidget;
  QDockWidget* m_objectDetailedInfoDockWidget;
  QDockWidget* m_captureDockWidget;
  QDockWidget* m_editObjDockWidget;
  ZObjEditWidget* m_objEditWidget;

  //
  std::unique_ptr<ZDoc> m_doc;
  std::unique_ptr<ZView> m_view;

  //
  Z3DCanvas* m_sharedContext;

  QPointer<Z3DMainWindow> m_3dWindow;

  bool m_isClosed = false;

  QString m_versionString;
};

} // namespace nim

