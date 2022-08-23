#pragma once

#include "zitemeditorfactory.h"
#include <QStyledItemDelegate>

class QPushButton;

namespace nim {

class ZStyledItemDelegate : public QStyledItemDelegate
{
  Q_OBJECT

public:
  explicit ZStyledItemDelegate(QObject* parent = nullptr);

  inline QString displayText(const QVariant& value, const QLocale& locale) const
  {
    if (value.metaType() == QMetaType(QMetaType::Double)) {
      return locale.toString(value.toDouble(), 'g', 16);
    }
    return QStyledItemDelegate::displayText(value, locale);
  }

#if 0
  void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const;
  QWidget *createEditor(QWidget *parent, const QStyleOptionViewItem &option, const QModelIndex &index) const;
  void setEditorData(QWidget *editor, const QModelIndex &index) const;
  void setModelData(QWidget *editor, QAbstractItemModel *model, const QModelIndex &index) const;
  void updateEditorGeometry(QWidget *editor, const QStyleOptionViewItem &option, const QModelIndex &index) const;

  void cellEntered(const QModelIndex &index);
#endif

protected:
  ZItemEditorFactory m_factory;
#if 0
  QAbstractItemView *m_widget;
  QIcon m_settingIcon;
  QPushButton *m_btn;
  bool m_isOneCellInEditMode;
  QPersistentModelIndex m_currentEditedCellIndex;
#endif
};

} // namespace nim
