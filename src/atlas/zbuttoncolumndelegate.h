#pragma once

#include <QStyledItemDelegate>
#include <QWidget>
#include <QPushButton>

namespace nim {

class ZButtonColumnDelegate : public QStyledItemDelegate
{
Q_OBJECT
public:
  explicit ZButtonColumnDelegate(QObject* parent = 0);

  ~ZButtonColumnDelegate();

  virtual QWidget* createEditor(QWidget* parent, const QStyleOptionViewItem& option,
                                const QModelIndex& index) const override;

  virtual void setEditorData(QWidget* editor, const QModelIndex& index) const override;

  virtual void setModelData(QWidget* editor, QAbstractItemModel* model, const QModelIndex& index) const override;

  virtual void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override;

  virtual void
  updateEditorGeometry(QWidget* editor, const QStyleOptionViewItem& option, const QModelIndex& index) const override;

  virtual QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override;

  void cellEntered(const QModelIndex& index);

signals:

  void buttonClickedForUserData(QVariant ud);

protected:
  void buttonClicked();

private:
  QAbstractItemView* m_widget;
  QPushButton* m_button;
  bool m_isOneCellInEditMode;
  QPersistentModelIndex m_currentEditedCellIndex;
};

} // namespace nim

