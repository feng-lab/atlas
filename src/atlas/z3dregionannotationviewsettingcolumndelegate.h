#pragma once

#include "z3dmeshfilter.h"
#include <QStyledItemDelegate>

namespace nim {

class Z3DRegionAnnotationViewSettingColumnDelegate : public QStyledItemDelegate
{
Q_OBJECT
public:
  explicit Z3DRegionAnnotationViewSettingColumnDelegate(
    std::map<int, std::unique_ptr<Z3DMeshFilter>>& idToMeshFilters,
    QObject* parent = nullptr);

  void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override;

  [[nodiscard]] QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override;

private:
  std::map<int, std::unique_ptr<Z3DMeshFilter>>& m_idToMeshFilters;
};

} // namespace nim

