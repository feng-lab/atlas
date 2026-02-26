#pragma once

#include "zjson.h"
#include <QObject>
#include <QUndoGroup>
#include <memory>

class QUndoStack;

class QAction;

class QWidget;

class QMenu;

class QModelIndex;

namespace nim {

class ZObjDoc;

class ZObjModel;

class ZItemSelectionModel;

class ZObjWidget;

class ZImgDoc;

class Z2DAnimationDoc;

class Z3DAnimationDoc;

class ZROIDoc;

class ZPunctaDoc;

class ZSvgDoc;

class ZSwcDoc;

class ZRegionAnnotationDoc;

class ZMeshDoc;

class ZSkeletonDoc;

class ZObjPack;

class ZTraceSettings;

class ZDoc : public QObject
{
  Q_OBJECT

public:
  explicit ZDoc(QObject* parent = nullptr);

  std::vector<size_t> chooseObjsWithWidget(const QString& title, QWidget* parent) const;

  [[nodiscard]] bool hasObj() const;

  [[nodiscard]] size_t numObjs() const;

  [[nodiscard]] std::vector<size_t> objs() const;

  [[nodiscard]] size_t numSelectedObjs() const;

  [[nodiscard]] std::vector<size_t> selectedObjs() const;

  std::vector<ZObjDoc*> objDocs();

  [[nodiscard]] ZObjDoc* idToDoc(size_t id) const;

  [[nodiscard]] bool isObjVisible(size_t id) const;

  [[nodiscard]] bool isObjSelected(size_t id) const;

  void setObjVisible(size_t id, bool v);

  void setObjLocked(size_t id, bool v);

  void setObjSelected(size_t id, bool v);

  void deselectObj(size_t id);

  void clearAndSelectObj(size_t id);

  void appendSelectObj(size_t id);

  size_t indexToId(const QModelIndex& index);

  [[nodiscard]] QString objName(size_t id) const;

  [[nodiscard]] QString objNameWithModifiedMarker(size_t id) const;

  [[nodiscard]] QString objNameWithModifiedMarkerAndID(size_t id) const;

  [[nodiscard]] QString objDetailedInfo(size_t id) const;

  std::vector<size_t> objsOfDoc(const ZObjDoc* objD) const;

  std::vector<size_t> selectedObjsOfDoc(const ZObjDoc* objD) const;

  QUndoGroup* undoGroup()
  {
    return m_undoGroup;
  }

  void activateEmptyUndoStack();

  QAction* undoAction()
  {
    return m_undoAction;
  }

  QAction* redoAction()
  {
    return m_redoAction;
  }

  QAction* make2DAnimationAction()
  {
    return m_make2DAnimationAction;
  }

  QAction* make3DAnimationAction()
  {
    return m_make3DAnimationAction;
  }

  QAction* changeAnimationSettingAction()
  {
    return m_changeAnimationSettingAction;
  }

  [[nodiscard]] std::vector<QAction*> fileActions() const;

  [[nodiscard]] std::vector<QAction*> loadFileActions() const;

  [[nodiscard]] std::vector<QMenu*> processObjMenu() const;

  ZObjWidget* createObjWidget(QWidget* parent);

  QWidget* createObjEditWidget(size_t id) const;

  // notify model to update some info of obj id
  void updateObjInfo(size_t id);

  void registerObjDoc(ZObjDoc* objD);

  // get a unique id for new object
  size_t getNewObjId()
  {
    return m_nextObjId++;
  }

  // let doc know objD has a new obj with id
  void registerNewObj(size_t id, ZObjDoc& objD);

  void registerNewObj(const std::shared_ptr<ZObjPack>& op);

  void removeObj(size_t id);

  // Remove objects without showing Save/Discard UI dialogs (intended for RPC automation).
  // Callers must explicitly decide how to handle unsaved changes (e.g., block the operation
  // unless a "force discard" flag is set) before calling this method.
  //
  // Preconditions:
  // - ids must be non-empty
  // - ids must all exist in this document
  void removeObjsNoPrompt(const std::vector<size_t>& ids);

  void removeAllObjsOfDoc(ZObjDoc* doc);

  // ask user whether or not save change of obj id, if obj id has no change, return true
  // if user choose discard, return true.
  // if user choose save and save succeed, return true.
  // otherwise return false
  bool saveOrDiscard(size_t id);

  bool saveOrDiscard(const std::vector<size_t>& objs);

  void loadFile(const QString& fileName);

  void loadFileList(const QStringList& fileList);

  size_t viewSettingId();

  std::map<size_t, size_t> read(const json::object& jo, QString& err);

  void write(json::object& json, bool includeAnimation) const;

  static QString lastOpenedFilePath();

  static void setLastOpenedFilePath(const QString& path);

  ZImgDoc& imgDoc()
  {
    return *m_imgDoc;
  }

  Z3DAnimationDoc& animation3DDoc()
  {
    return *m_animation3DDoc;
  }

  Z2DAnimationDoc& animation2DDoc()
  {
    return *m_animation2DDoc;
  }

  ZROIDoc& roiDoc()
  {
    return *m_roiDoc;
  }

  ZPunctaDoc& punctaDoc()
  {
    return *m_punctaDoc;
  }

  ZSwcDoc& swcDoc()
  {
    return *m_swcDoc;
  }

  ZSvgDoc& svgDoc()
  {
    return *m_svgDoc;
  }

  ZMeshDoc& meshDoc()
  {
    return *m_meshDoc;
  }

  ZSkeletonDoc& skeletonDoc()
  {
    return *m_skeletonDoc;
  }

  ZRegionAnnotationDoc& regionAnnotationDoc()
  {
    return *m_regionAnnotationDoc;
  }

  ZTraceSettings& traceSettings()
  {
    return *m_traceSettings;
  }

  [[nodiscard]] const ZTraceSettings& traceSettings() const
  {
    return *m_traceSettings;
  }

  void hideAnimation3DView();

  void deselectAllObjs();

  void removeAllObjs();

  void showSelectedObjs();

  void hideSelectedObjs();

  void lockSelectedObjs();

  void unlockSelectedObjs();

  void makeAliasOfSelectedObjs();

  void removeSelectedObjs();

  bool saveSelectedObjs();

  bool saveSelectedObjsAs();

  bool saveAllObjs() const;

  void showSelectedObjsInGraphicalShell();

  void copySelectedObjsPathToClipboard();

Q_SIGNALS:
  void showViewSetting(size_t id);

  void hideViewSetting();

  void openEditWidget(size_t id);

  void objAboutToBeRemoved(size_t id, ZObjDoc* doc);

  void objAdded(size_t id, ZObjDoc* doc);

  void objRemoved(size_t id, ZObjDoc* doc);

  void objInfoChanged(size_t id);

  void requestToAdjustViewToPosition(double x, double y, double z, double radius = 128.);

protected:
  friend class ZObjModel;

  void sendShowViewSettingSignal(size_t id)
  {
    Q_EMIT showViewSetting(id);
    m_viewSettingId = id;
  }

  void sendHideViewSettingSignal()
  {
    Q_EMIT hideViewSetting();
    m_viewSettingId = 0;
  }

  void sendOpenEditWidgetSignal(size_t id)
  {
    Q_EMIT openEditWidget(id);
  }

  void onObjAboutToBeRemoved(size_t id, ZObjDoc* doc);

  void onObjAdded(size_t id, ZObjDoc* doc);

private:
  void create2DAnimation();

  void create3DAnimation();

private:
  struct DocPack
  {
    ZObjDoc* doc;
    QAction* removeAllAction;
  };

  void createActions();

  bool loadFile(const QString& fileName, QString& errMsg);

private:
  QUndoGroup* m_undoGroup = nullptr;
  QUndoStack* m_emptyUndoStack = nullptr;
  QAction* m_undoAction = nullptr;
  QAction* m_redoAction = nullptr;
  QAction* m_removeAllAction = nullptr;
  QAction* m_make2DAnimationAction = nullptr;
  QAction* m_make3DAnimationAction = nullptr;
  QAction* m_changeAnimationSettingAction = nullptr;

  ZObjModel* m_objModel = nullptr;
  ZItemSelectionModel* m_objSelectionModel = nullptr;
  std::vector<DocPack> m_docPacks;
  size_t m_nextObjId;

  size_t m_viewSettingId;

  ZImgDoc* m_imgDoc = nullptr;
  Z2DAnimationDoc* m_animation2DDoc = nullptr;
  Z3DAnimationDoc* m_animation3DDoc = nullptr;
  ZROIDoc* m_roiDoc = nullptr;
  ZPunctaDoc* m_punctaDoc = nullptr;
  ZSwcDoc* m_swcDoc = nullptr;
  ZSvgDoc* m_svgDoc = nullptr;
  ZMeshDoc* m_meshDoc = nullptr;
  ZSkeletonDoc* m_skeletonDoc = nullptr;
  ZRegionAnnotationDoc* m_regionAnnotationDoc = nullptr;

  ZTraceSettings* m_traceSettings = nullptr;
};

} // namespace nim
