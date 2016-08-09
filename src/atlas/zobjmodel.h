#pragma once

#include <QAbstractItemModel>
#include <QModelIndex>
#include <QVariant>
#include <QIcon>

#define CONFIG_OLD

namespace nim {

class ZDoc;

class ZObjDoc;

class ZObjModel : public QAbstractItemModel
{
Q_OBJECT
public:
  enum Column
  {
#ifdef CONFIG_OLD
    ShowHideNameColumn = 0,
    TypeColumn,
    InfoColumn,
    IDColumn,
    ShowHideColumn,
    LockColumn,
    NameColumn,
    ViewSettingColumn,
    ColumnCount
#else
    ShowHideColumn = 0, LockColumn, NameColumn, TypeColumn, InfoColumn, IDColumn, ViewSettingColumn, ShowHideNameColumn, ColumnCount
#endif
  };

  explicit ZObjModel(ZDoc* doc);

  QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const;

  Qt::ItemFlags flags(const QModelIndex& index) const;

  bool setData(const QModelIndex& index, const QVariant& value, int role = Qt::EditRole);

  QVariant headerData(int section, Qt::Orientation orientation,
                      int role = Qt::DisplayRole) const;

  QModelIndex index(int row, int column,
                    const QModelIndex& parent = QModelIndex()) const;

  QModelIndex parent(const QModelIndex& index) const;

  int rowCount(const QModelIndex& parent = QModelIndex()) const;

  int columnCount(const QModelIndex& parent = QModelIndex()) const;

  inline bool hasObj() const
  { return !m_rootItem->children.empty(); }

  size_t numObjs() const;

  QList<size_t> objs() const;

  QList<size_t> objsOfDoc(const ZObjDoc* doc) const;

  bool isObjVisible(size_t id) const;

  void setObjVisible(size_t id, bool v);

  ZObjDoc* idToDoc(size_t id) const;

  void updateObj(size_t id);

  void addObj(size_t id, ZObjDoc* doc);

  void removeObj(size_t id);

  void removeObjsOfDoc(ZObjDoc* doc);

  void removeAllObjs();

  size_t indexToId(const QModelIndex& index);

  QModelIndex idToIndex(size_t id, int col = 0);

  ZObjDoc* indexToDoc(const QModelIndex& index);

  void clicked(const QModelIndex& index);

  void doubleClicked(const QModelIndex& index);

  void activated(const QModelIndex& index);

protected:
  void updateChildCheckState(const QModelIndex& parent, Qt::CheckState cs);

  void updateParentCheckState(const QModelIndex& child);

  // These two functions assume the input modelindex is valid
  virtual void setModelIndexCheckState(const QModelIndex& index, Qt::CheckState cs);

  Qt::CheckState getModelIndexCheckState(const QModelIndex& index) const;

  bool needCheckbox(const QModelIndex& index) const;

protected:
  ZDoc* m_doc;

  struct ObjItem
  {
    ObjItem(size_t id, ZObjDoc* doc, ObjItem* parent);

    int row() const;

    std::vector<std::unique_ptr<ObjItem>> children;
    ObjItem* parent;

    size_t id;
    ZObjDoc* doc;
    Qt::CheckState checkState;
    bool locked;
    bool show;
  };

  QIcon m_settingIcon;
  QIcon m_visibleIcon;
  QIcon m_invisibleIcon;
  QIcon m_lockIcon;
  QIcon m_unlockIcon;
  std::unique_ptr<ObjItem> m_rootItem;
  ObjItem* m_viewSettingCurrentItem;
};

} // namespace nim

