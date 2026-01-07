#pragma once

#include "zneuroglancerprecomputeddatasetlist.h"

#include <QDialog>

#include <vector>

class QLineEdit;
class QStandardItemModel;
class QTableView;
class QLabel;
class QPushButton;

namespace nim {

class ZLoadNeuroglancerPrecomputedDialog : public QDialog
{
  Q_OBJECT

public:
  explicit ZLoadNeuroglancerPrecomputedDialog(QWidget* parent = nullptr);

  void setInitialUrl(const QString& url);

  [[nodiscard]] QString selectedUrl() const;
  [[nodiscard]] QString selectedName() const;

  [[nodiscard]] std::vector<ZNeuroglancerPrecomputedDatasetList::Entry> userHistoryEntries() const;

private:
  void addHistoryRow(const ZNeuroglancerPrecomputedDatasetList::Entry& entry);
  void setUrlAndName(const QString& url, const QString& name);

  void wireSelectionToEdits(QTableView* view, QStandardItemModel* model, int nameCol, int urlCol);
  void removeSelectedHistory();
  void updateHistorySourceUi();
  void editSelectedHistorySources();
  void clearSelectedHistorySources();

private:
  QLineEdit* m_urlEdit = nullptr;
  QLineEdit* m_nameEdit = nullptr;

  QStandardItemModel* m_historyModel = nullptr;
  QTableView* m_historyView = nullptr;
  QLabel* m_historyMeshSourceLabel = nullptr;
  QLabel* m_historySkeletonSourceLabel = nullptr;
  QPushButton* m_historyEditSourcesBtn = nullptr;
  QPushButton* m_historyClearSourcesBtn = nullptr;

  QStandardItemModel* m_examplesModel = nullptr;
  QTableView* m_examplesView = nullptr;
};

} // namespace nim
