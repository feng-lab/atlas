#pragma once

#include <QStyledItemDelegate>
#include <QWidget>
#include <QPushButton>

namespace nim {

class ZButtonColumnDelegate : public QStyledItemDelegate
{
  Q_OBJECT

public:
  explicit ZButtonColumnDelegate(QObject* parent = nullptr);

  QWidget* createEditor(QWidget* parent, const QStyleOptionViewItem& option, const QModelIndex& index) const override;

  void setEditorData(QWidget* editor, const QModelIndex& index) const override;

  void setModelData(QWidget* editor, QAbstractItemModel* model, const QModelIndex& index) const override;

  void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override;

  void
  updateEditorGeometry(QWidget* editor, const QStyleOptionViewItem& option, const QModelIndex& index) const override;

  QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override;

  void cellEntered(const QModelIndex& index);

Q_SIGNALS:
  void buttonClickedForUserData(QVariant ud);

protected:
  void buttonClicked();

private:
  QAbstractItemView* m_widget = nullptr;
  mutable std::unique_ptr<QPushButton> m_button;
  bool m_isOneCellInEditMode;
  QPersistentModelIndex m_currentEditedCellIndex;
};

} // namespace nim
