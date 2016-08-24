#pragma once

#include <QObject>
#include <QMap>
#include <QList>
#include <QSet>
#include <QUndoGroup>
#include <deque>

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

class ZMeshDoc;

class ZDoc : public QObject
{
Q_OBJECT
public:
  using ConstObjIdIterator = QList<size_t>::const_iterator;

  explicit ZDoc(QObject* parent = 0);

  bool hasObj() const;

  size_t numObjs() const;

  QList<size_t> objs() const;

  size_t numSelectedObjs() const;

  QList<size_t> selectedObjs() const;

  QList<ZObjDoc*> objDocs();

  ZObjDoc* idToDoc(size_t id) const;

  bool isObjVisible(size_t id) const;

  bool isObjSelected(size_t id) const;

  void setObjVisible(size_t id, bool v);

  void setObjSelected(size_t id, bool v);

  void deselectObj(size_t id);

  void clearAndSelectObj(size_t id);

  void appendSelectObj(size_t id);

  size_t indexToId(const QModelIndex& index);

  QString objName(size_t id) const;

  QString objNameWithModifiedMarker(size_t id) const;

  QString objNameWithModifiedMarkerAndID(size_t id) const;

  QString objDetailedInfo(size_t id) const;

  QList<size_t> objsOfDoc(const ZObjDoc* objD) const;

  QList<size_t> selectedObjsOfDoc(const ZObjDoc* objD) const;

  inline QUndoGroup* undoGroup()
  { return m_undoGroup; }

  void activateEmptyUndoStack();

  inline QAction* undoAction()
  { return m_undoAction; }

  inline QAction* redoAction()
  { return m_redoAction; }

  inline QAction* make2DAnimationAction()
  { return m_make2DAnimationAction; }

  inline QAction* make3DAnimationAction()
  { return m_make3DAnimationAction; }

  inline QAction* changeAnimationSettingAction()
  { return m_changeAnimationSettingAction; }

  QList<QAction*> fileActions() const;

  QList<QAction*> loadFileActions() const;

  QList<QMenu*> processObjMenu() const;

  ZObjWidget* createObjWidget(QWidget* parent);

  QWidget* createObjEditWidget(size_t id);

  // notify model to update some info of obj id
  void updateObjInfo(size_t id);

  void registerObjDoc(ZObjDoc* objD);

  // get a unique id for new object
  inline size_t getNewObjId()
  { return m_nextObjId++; }

  // let doc know objD has a new obj with id
  void registerNewObj(size_t id, ZObjDoc* objD);

  void removeObj(size_t id);

  void removeAllObjsOfDoc(ZObjDoc* doc);

  // ask user whether or not save change of obj id, if obj id has no change, return true
  // if user choose discard, return true.
  // if user choose save and save succeed, return true.
  // otherwise return false
  bool saveOrDiscard(size_t id);

  bool saveOrDiscard(const QList<size_t>& objs);

  void loadFile(const QString& fileName);

  void loadFileList(const QStringList& fileList);

  size_t viewSettingId();

  std::map<size_t, size_t> read(const QJsonObject& json, QString& err);

  void write(QJsonObject& json, bool includeAnimation) const;

  QString lastOpenedFilePath();

  void setLastOpenedFilePath(const QString& path);

  ZImgDoc& imgDoc()
  { return *m_imgDoc; }

  Z3DAnimationDoc& animation3DDoc()
  { return *m_animation3DDoc; }

  Z2DAnimationDoc& animation2DDoc()
  { return *m_animation2DDoc; }

  ZROIDoc& roiDoc()
  { return *m_roiDoc; }

  ZMeshDoc& meshDoc()
  { return *m_meshDoc; }

  void hideAnimation3DView();

  void removeAllObjs();

  void showSelectedObjs();

  void hideSelectedObjs();

  void makeAliasOfSelectedObjs();

  void removeSelectedObjs();

  bool saveSelectedObjs();

  bool saveSelectedObjsAs();

  bool saveAllObjs();

  void showSelectedObjsInGraphicalShell();

  void copySelectedObjsPathToClipboard();

signals:

  void showViewSetting(size_t id);

  void hideViewSetting();

  void openEditWidget(size_t id);

  void objAboutToBeRemoved(size_t id, ZObjDoc* doc);

  void objAdded(size_t id, ZObjDoc* doc);

  void objRemoved(size_t id, ZObjDoc* doc);

  void objInfoChanged(size_t id);

protected:
  friend class ZObjModel;

  void sendShowViewSettingSignal(size_t id)
  {
    emit showViewSetting(id);
    m_viewSettingId = id;
  }

  void sendHideViewSettingSignal()
  {
    emit hideViewSetting();
    m_viewSettingId = 0;
  }

  void sendOpenEditWidgetSignal(size_t id)
  { emit openEditWidget(id); }

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
  QUndoGroup* m_undoGroup;
  QUndoStack* m_emptyUndoStack;
  QAction* m_undoAction;
  QAction* m_redoAction;
  QAction* m_removeAllAction;
  QAction* m_make2DAnimationAction;
  QAction* m_make3DAnimationAction;
  QAction* m_changeAnimationSettingAction;

  ZObjModel* m_objModel;
  ZItemSelectionModel* m_objSelectionModel;
  QList<DocPack> m_docPacks;
  size_t m_nextObjId;

  size_t m_viewSettingId;

  ZImgDoc* m_imgDoc;
  Z2DAnimationDoc* m_animation2DDoc;
  Z3DAnimationDoc* m_animation3DDoc;
  ZROIDoc* m_roiDoc;
  ZMeshDoc* m_meshDoc;
};

} // namespace nim

