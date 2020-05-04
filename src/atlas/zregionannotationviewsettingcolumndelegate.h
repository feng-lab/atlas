#pragma once

#include "zroifilter.h"
#include <QStyledItemDelegate>
#include <QWidget>
#include <QPushButton>

namespace nim {

class ZRegionAnnotationViewSettingColumnDelegate : public QStyledItemDelegate
{
Q_OBJECT
public:
  ZRegionAnnotationViewSettingColumnDelegate(
    std::map<int, std::unique_ptr<ZROIFilter>>& idToROIFilters,
    QObject* parent = nullptr);

  virtual QWidget* createEditor(QWidget* parent, const QStyleOptionViewItem& option,
                                const QModelIndex& index) const override;

  void setEditorData(QWidget* editor, const QModelIndex& index) const override;

  void setModelData(QWidget* editor, QAbstractItemModel* model, const QModelIndex& index) const override;

  void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override;

  virtual void
  updateEditorGeometry(QWidget* editor, const QStyleOptionViewItem& option, const QModelIndex& index) const override;

  QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override;

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
  std::map<int, std::unique_ptr<ZROIFilter>>& m_idToROIFilters;
};

} // namespace nim

