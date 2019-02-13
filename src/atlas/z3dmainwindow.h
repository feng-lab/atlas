#pragma once

#include <QMainWindow>

class QModelIndex;

namespace nim {

class ZDoc;

class Z3DView;

class ZViewSettingWidget;

class ZObjDetailedInfoWidget;

class ZObjEditWidget;

class ZMainWindow;

class Z3DMainWindow : public QMainWindow
{
Q_OBJECT
public:
  explicit Z3DMainWindow(ZDoc* doc, ZMainWindow& win2d, bool stereoView = false,
                         QWidget* parent = nullptr);

  Z3DView* view()
  { return m_view; }

  void openEditWidget(size_t id);

signals:

  void loadScene();

  void saveScene();

  void loadJsonScene(const QString& fn);

  void viewReady(Z3DView* view);

protected:
  void closeEvent(QCloseEvent* event) override;

  void dragEnterEvent(QDragEnterEvent* event) override;

  void dropEvent(QDropEvent* event) override;

private:
  void open();

  bool save();

  bool saveAs();

  void openRecentFile();

  void about();

  void activateWindowIfNot();  //mac bug?

  void viewLog();

  void openLogFolder();

  void changeBackground();

  void changeAxis();

  void openScreenshotPanel();

  void raiseViewSettingDockWidget();

  void raiseGlobalSettingDockWidget();

  void init();

  void createActions();

  void createMenus();

  void createToolBars();

  void createStatusBar();

  void createDockWindows();

  void fillDockWindows();

  void readSettings();

  void writeSettings();

  bool maybeSave();

  void loadWorkspace(const QString& fileName);

  bool saveFile(const QString& fileName);

  void setCurrentFile(const QString& fileName);

  QString strippedName(const QString& fullFileName);

  Z3DMainWindow* findMainWindow(const QString& fileName);

  void onViewReady();

private:
  QMenu* m_fileMenu;
  QMenu* m_editMenu;
  QMenu* m_viewMenu;
  QMenu* m_animationMenu;
  QMenu* m_windowMenu;
  QMenu* m_helpMenu;
  QToolBar* m_fileToolBar;
  QToolBar* m_editToolBar;
  QToolBar* m_viewToolBar;
  QToolBar* m_helpToolBar;

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

  QAction* m_separatorAction;

  QDockWidget* m_objectsDockWidget;
  QDockWidget* m_viewSettingDockWidget;
  QDockWidget* m_objectDetailedInfoDockWidget;
  QDockWidget* m_globalSettingDockWidget;
  QDockWidget* m_captureDockWidget;
  QDockWidget* m_backgroundDockWidget;
  QDockWidget* m_axisDockWidget;
  ZViewSettingWidget* m_viewSettingWidget;
  ZObjDetailedInfoWidget* m_objDetailedInfoWidget;
  QDockWidget* m_editObjDockWidget;
  ZObjEditWidget* m_objEditWidget;

  QAction* m_changeBackgroundAction;
  QAction* m_changeAxisAction;
  QAction* m_screenShotAction;

  //
  ZDoc* m_doc;
  Z3DView* m_view;

  bool m_isStereoView;

  ZMainWindow& m_2dWindow;
};

} // namespace nim

