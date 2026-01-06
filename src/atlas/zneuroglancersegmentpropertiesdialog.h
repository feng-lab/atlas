#pragma once

#include "zneuroglancerprecomputed.h"

#include <QDialog>

#include <cstdint>
#include <functional>
#include <memory>

class QFutureWatcherBase;
class QLabel;
class QLineEdit;
class QPushButton;
class QSortFilterProxyModel;
class QTableView;
class QTimer;

namespace nim {

class ZNeuroglancerSegmentPropertiesModel;

class ZNeuroglancerSegmentPropertiesDialog : public QDialog
{
  Q_OBJECT

public:
  explicit ZNeuroglancerSegmentPropertiesDialog(std::shared_ptr<ZNeuroglancerPrecomputedVolume> vol,
                                               QWidget* parent = nullptr);

  void setMeshLoadCallback(std::function<void(uint64_t)> cb);
  void setAfterPropertiesLoadedCallback(
    std::function<void(std::shared_ptr<const ZNeuroglancerPrecomputedSegmentProperties>)> cb);

private:
  void beginLoad();

  void finishLoad(std::shared_ptr<const ZNeuroglancerPrecomputedSegmentProperties> props, QString error);

  void applyFilterNow();

  void updateCounts();

  void copySelectedIdsToClipboard();

  void loadMeshesForSelectedIds();

private:
  std::shared_ptr<ZNeuroglancerPrecomputedVolume> m_vol;

  std::shared_ptr<const ZNeuroglancerPrecomputedSegmentProperties> m_props;

  std::function<void(uint64_t)> m_meshLoadCallback;
  std::function<void(std::shared_ptr<const ZNeuroglancerPrecomputedSegmentProperties>)> m_afterPropertiesLoadedCallback;

  QLabel* m_titleLabel = nullptr;
  QLabel* m_statusLabel = nullptr;
  QLabel* m_countLabel = nullptr;
  QLineEdit* m_searchEdit = nullptr;
  QTableView* m_table = nullptr;
  QPushButton* m_copyButton = nullptr;
  QPushButton* m_loadMeshButton = nullptr;

  ZNeuroglancerSegmentPropertiesModel* m_model = nullptr;
  QSortFilterProxyModel* m_proxy = nullptr;

  QFutureWatcherBase* m_loadWatcher = nullptr;
  QTimer* m_filterTimer = nullptr;
};

} // namespace nim
