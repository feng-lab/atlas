#pragma once

#include <QMainWindow>

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
  explicit Z3DMainWindow(ZDoc& doc, ZMainWindow& win2d, bool stereoView = false, QWidget* parent = nullptr);

  Z3DView* view()
  {
    return m_view;
  }

  void openEditWidget(size_t id);

Q_SIGNALS:

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

  void activateWindowIfNot(); // mac bug?

  void changeBackground();

  void changeAxis();

  void openScreenshotPanel();

  void openHelpPanel();

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

  void onViewReady();

private:
  QMenu* m_fileMenu = nullptr;
  QMenu* m_editMenu = nullptr;
  QMenu* m_viewMenu = nullptr;
  QMenu* m_animationMenu = nullptr;
  QMenu* m_windowMenu = nullptr;
  QMenu* m_helpMenu = nullptr;
  QToolBar* m_fileToolBar = nullptr;
  QToolBar* m_editToolBar = nullptr;
  QToolBar* m_viewToolBar = nullptr;
  QToolBar* m_helpToolBar = nullptr;

  QAction* m_openAction = nullptr;
  QAction* m_saveAction = nullptr;
  QAction* m_saveAsAction = nullptr;
  QAction* m_loadSceneAction = nullptr;
  QAction* m_saveSceneAction = nullptr;
  QAction* m_closeAction = nullptr;

  QAction* m_separatorAction = nullptr;

  QDockWidget* m_objectsDockWidget = nullptr;
  QDockWidget* m_viewSettingDockWidget = nullptr;
  QDockWidget* m_objectDetailedInfoDockWidget = nullptr;
  QDockWidget* m_globalSettingDockWidget = nullptr;
  QDockWidget* m_captureDockWidget = nullptr;
  QDockWidget* m_helpDockWidget = nullptr;
  QDockWidget* m_backgroundDockWidget = nullptr;
  QDockWidget* m_axisDockWidget = nullptr;
  ZViewSettingWidget* m_viewSettingWidget = nullptr;
  ZObjDetailedInfoWidget* m_objDetailedInfoWidget = nullptr;
  QDockWidget* m_editObjDockWidget = nullptr;
  ZObjEditWidget* m_objEditWidget = nullptr;

  QAction* m_changeBackgroundAction = nullptr;
  QAction* m_changeAxisAction = nullptr;
  QAction* m_screenShotAction = nullptr;
  QAction* m_helpAction = nullptr;

  //
  ZDoc& m_doc;
  Z3DView* m_view = nullptr;

  bool m_isStereoView = false;

  ZMainWindow& m_2dWindow;
};

} // namespace nim
