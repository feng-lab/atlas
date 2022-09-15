#pragma once

#include <QMainWindow>
#include <QPointer>
#include <memory>

class QAction;

class QActionGroup;

class QMenu;

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
  explicit ZMainWindow(QString versionStr);

  void initOpenglContext();

  [[nodiscard]] const std::vector<QAction*>& recentFileActions() const
  {
    return m_recentFileActions;
  }

  void updateRecentFileActions();

  void openEditWidget(size_t id);

  void loadUrls(const QList<QUrl>& urlList);

  void loadFileList(const QStringList& fileList);

  void loadJsonScene(const QString& fn);

  void removeAllObjs();

  // might be nullptr
  Z3DMainWindow* get3DWindow();

  ZView* view();

  QAction* exitAction()
  {
    return m_exitAction;
  }

  QAction* aboutAction()
  {
    return m_aboutAction;
  }

  QAction* aboutQtAction()
  {
    return m_aboutQtAction;
  }

  QAction* checkForUpdatesAction()
  {
    return m_checkForUpdatesAction;
  }

  QAction* viewLogAction()
  {
    return m_viewLogAction;
  }

  QAction* openLogFolderAction()
  {
    return m_openLogFolderAction;
  }

  QAction* openConfigFolderAction()
  {
    return m_openConfigFolderAction;
  }

protected:
  // void appAboutToQuit();

  void closeEvent(QCloseEvent* event) override;

  void dragEnterEvent(QDragEnterEvent* event) override;

  void dropEvent(QDropEvent* event) override;

private:
  // void newWindow();
  void open();

  bool save();

  bool saveAs();

  void openRecentFile();

  void about();

#ifdef Q_OS_LINUX
  void createDesktopEntry();
#endif

  void activateWindowIfNot(); // mac bug?

  void openScreenshotPanel();

  void openHelpPanel();

  void raiseViewSettingDockWidget();

  void raiseGlobalSettingDockWidget();

  static void viewLog();

  static void openLogFolder();

  static void openConfigFolder();

  static void generateConfigFile();

  static void runBenchmark();

  static void runCustomCommand();

  void open3DWindow();

  void loadScene();

  void saveScene();

  static void openNewInstance();

  void init();

  void createActions();

  void createMenus();

  void createToolBars();

  void createStatusBar();

  void createDockWindows();

  void readSettings();

  void writeSettings();

  bool maybeSave();

  // void loadWorkspace(const QString &fileName);
  // bool saveFile(const QString &fileName);
  // void setCurrentFile(const QString &fileName);
  static QString strippedName(const QString& fullFileName);

  static ZMainWindow* findMainWindow(const QString& fileName);

  bool loadJsonSceneImpl(const QString& fn, QString& err);

  bool saveJsonSceneImpl(const QString& fn, QString& err);

  static void checkForUpdates();

private:
  QMenu* m_fileMenu = nullptr;
  QMenu* m_editMenu = nullptr;
  QMenu* m_viewMenu = nullptr;
  QMenu* m_animationMenu = nullptr;
  QMenu* m_windowMenu = nullptr;
  QMenu* m_helpMenu = nullptr;
  QMenu* m_dockMenu = nullptr;
  QToolBar* m_fileToolBar = nullptr;
  QToolBar* m_editToolBar = nullptr;
  QToolBar* m_viewToolBar = nullptr;
  QToolBar* m_dragModeToolBar = nullptr;
  QToolBar* m_roiToolBar = nullptr;
  QToolBar* m_helpToolBar = nullptr;

  // QAction *m_newAction = nullptr;
  QAction* m_openAction = nullptr;
  QAction* m_saveAction = nullptr;
  QAction* m_saveAsAction = nullptr;
  QAction* m_loadSceneAction = nullptr;
  QAction* m_saveSceneAction = nullptr;
  QAction* m_closeAction = nullptr;

  QAction* m_exitAction = nullptr;
  QAction* m_aboutAction = nullptr;
  QAction* m_aboutQtAction = nullptr;
  QAction* m_checkForUpdatesAction = nullptr;
#ifdef Q_OS_LINUX
  QAction* m_createDesktopEntryAction = nullptr;
#endif

  QAction* m_viewLogAction = nullptr;
  QAction* m_openLogFolderAction = nullptr;
  QAction* m_openConfigFolderAction = nullptr;
  QAction* m_generateConfigFileAction = nullptr;
  QAction* m_runBenchmarkAction = nullptr;
  QAction* m_runCustomCommandAction = nullptr;

  QAction* m_separatorAction = nullptr;
  std::vector<QAction*> m_recentFileActions;

  QAction* m_open3DViewAction = nullptr;
  QAction* m_screenShotAction = nullptr;
  QAction* m_helpAction = nullptr;

  QAction* m_openNewInstanceAction = nullptr;

  QDockWidget* m_objectsDockWidget = nullptr;
  QDockWidget* m_viewSettingDockWidget = nullptr;
  QDockWidget* m_objectDetailedInfoDockWidget = nullptr;
  QDockWidget* m_globalSettingDockWidget = nullptr;
  QDockWidget* m_captureDockWidget = nullptr;
  QDockWidget* m_helpDockWidget = nullptr;
  QDockWidget* m_editObjDockWidget = nullptr;
  ZObjEditWidget* m_objEditWidget = nullptr;

  //
  std::unique_ptr<ZDoc> m_doc;
  std::unique_ptr<ZView> m_view;

  //
  Z3DCanvas* m_sharedContext = nullptr;

  QPointer<Z3DMainWindow> m_3dWindow;

  bool m_isClosed = false;

  QString m_versionString;
};

} // namespace nim
