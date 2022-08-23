#pragma once

#include <QStyledItemDelegate>

namespace nim {

class ZROIFilter;

class Z3DMeshFilter;

class ZRegionAnnotationViewSettingColumnDelegate : public QStyledItemDelegate
{
  Q_OBJECT

public:
  explicit ZRegionAnnotationViewSettingColumnDelegate(std::map<int64_t, std::unique_ptr<ZROIFilter>>& idToROIFilters,
                                                      QObject* parent = nullptr);

  explicit ZRegionAnnotationViewSettingColumnDelegate(
    std::map<int64_t, std::unique_ptr<Z3DMeshFilter>>& idToMeshFilters,
    QObject* parent = nullptr);

  void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override;

  [[nodiscard]] QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override;

private:
  explicit ZRegionAnnotationViewSettingColumnDelegate(QObject* parent = nullptr);

private:
  std::map<int64_t, std::unique_ptr<ZROIFilter>>* m_idToROIFilters = nullptr;
  std::map<int64_t, std::unique_ptr<Z3DMeshFilter>>* m_idToMeshFilters = nullptr;
};

} // namespace nim
