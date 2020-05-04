#pragma once

#include <QAbstractItemModel>
#include <QModelIndex>
#include <QVariant>
#include <QIcon>
#include <memory>

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

  [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;

  [[nodiscard]] Qt::ItemFlags flags(const QModelIndex& index) const override;

  bool setData(const QModelIndex& index, const QVariant& value, int role) override;

  [[nodiscard]] QVariant headerData(int section, Qt::Orientation orientation,
                                    int role) const override;

  [[nodiscard]] QModelIndex index(int row, int column,
                                  const QModelIndex& parent) const override;

  [[nodiscard]] QModelIndex parent(const QModelIndex& index) const override;

  [[nodiscard]] int rowCount(const QModelIndex& parent) const override;

  [[nodiscard]] int columnCount(const QModelIndex& parent) const override;

  [[nodiscard]] inline bool hasObj() const
  { return !m_rootItem->children.empty(); }

  [[nodiscard]] size_t numObjs() const;

  [[nodiscard]] QList<size_t> objs() const;

  QList<size_t> objsOfDoc(const ZObjDoc* doc) const;

  [[nodiscard]] bool isObjVisible(size_t id) const;

  void setObjVisible(size_t id, bool v);

  [[nodiscard]] ZObjDoc* idToDoc(size_t id) const;

  void updateObj(size_t id);

  void addObj(size_t id, ZObjDoc* doc);

  void removeObj(size_t id);

  void removeObjsOfDoc(ZObjDoc* doc);

  void removeAllObjs();

  size_t indexToId(const QModelIndex& index);

  QModelIndex idToIndex(size_t id, int col = 0);

  ZObjDoc* indexToDoc(const QModelIndex& index);

  void clicked(const QModelIndex& idxIn);

  void doubleClicked(const QModelIndex& index);

  void activated(const QModelIndex& idxIn);

protected:
  void updateChildCheckState(const QModelIndex& parent, Qt::CheckState cs);

  void updateParentCheckState(const QModelIndex& child);

  // These two functions assume the input modelindex is valid
  virtual void setModelIndexCheckState(const QModelIndex& index, Qt::CheckState cs);

  [[nodiscard]] Qt::CheckState getModelIndexCheckState(const QModelIndex& index) const;

  [[nodiscard]] bool needCheckbox(const QModelIndex& index) const;

protected:
  ZDoc* m_doc;

  struct ObjItem
  {
    ObjItem(size_t id_, ZObjDoc* doc_, ObjItem* parent_);

    [[nodiscard]] int row() const;

    std::vector<std::unique_ptr<ObjItem>> children;
    ObjItem* parent;

    size_t id;
    ZObjDoc* doc;
    Qt::CheckState checkState = Qt::Checked;
    bool locked = false;
    bool show = true;
  };

  std::unique_ptr<ObjItem> m_rootItem;
  ObjItem* m_viewSettingCurrentItem;
};

} // namespace nim

