#pragma once

#include "zneuroglancerprecomputedsegmentproperties.h"

#include <QAbstractTableModel>

#include <cstdint>
#include <memory>
#include <optional>

namespace nim {

class ZNeuroglancerSegmentPropertiesModel : public QAbstractTableModel
{
  Q_OBJECT

public:
  struct ColumnSpec
  {
    enum class Kind
    {
      Id,
      String,
      Number,
      Tags,
    };

    Kind kind = Kind::Id;
    QString header;
    const ZNeuroglancerPrecomputedSegmentProperties::Property* prop = nullptr;
  };

  explicit ZNeuroglancerSegmentPropertiesModel(std::shared_ptr<const ZNeuroglancerPrecomputedSegmentProperties> props,
                                              QObject* parent = nullptr);

  [[nodiscard]] int rowCount(const QModelIndex& parent = QModelIndex()) const override;

  [[nodiscard]] int columnCount(const QModelIndex& parent = QModelIndex()) const override;

  [[nodiscard]] QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;

  [[nodiscard]] QVariant headerData(int section,
                                    Qt::Orientation orientation,
                                    int role = Qt::DisplayRole) const override;

  [[nodiscard]] std::optional<uint64_t> segmentIdForRow(int row) const;

private:
  std::shared_ptr<const ZNeuroglancerPrecomputedSegmentProperties> m_props;

  std::vector<ColumnSpec> m_columns;
};

} // namespace nim
