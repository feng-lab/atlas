#pragma once

#include "zroifilter.h"
#include <QStyledItemDelegate>

namespace nim {

class ZRegionAnnotationViewSettingColumnDelegate : public QStyledItemDelegate
{
Q_OBJECT
public:
  explicit ZRegionAnnotationViewSettingColumnDelegate(
    std::map<int, std::unique_ptr<ZROIFilter>>& idToROIFilters,
    QObject* parent = nullptr);

  void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override;

  [[nodiscard]] QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override;

private:
  std::map<int, std::unique_ptr<ZROIFilter>>& m_idToROIFilters;
};

} // namespace nim

