#include "zloadneuroglancerprecomputeddialog.h"

#include "zlog.h"

#include <QApplication>
#include <QDialogButtonBox>
#include <QGroupBox>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QStandardItemModel>
#include <QTableView>
#include <QTabWidget>
#include <QVBoxLayout>

namespace nim {

namespace {

constexpr int kHistoryColName = 0;
constexpr int kHistoryColUrl = 1;

constexpr int kExamplesColName = 0;
constexpr int kExamplesColKind = 1;
constexpr int kExamplesColUrl = 2;

[[nodiscard]] QString modelTextOrEmpty(const QStandardItemModel* model, const QModelIndex& index)
{
  if (!model || !index.isValid()) {
    return {};
  }
  const QVariant v = model->data(index, Qt::DisplayRole);
  return v.toString();
}

} // namespace

ZLoadNeuroglancerPrecomputedDialog::ZLoadNeuroglancerPrecomputedDialog(QWidget* parent)
  : QDialog(parent)
{
  setWindowTitle(tr("Load Neuroglancer (Precomputed)"));
  setModal(true);

  auto* rootLayout = new QVBoxLayout(this);

  auto* instructions = new QLabel(
    tr("Enter a Neuroglancer precomputed dataset root URL (or a direct `.../info` URL).\n"
       "Viewer state `.json` URLs are not dataset roots."));
  instructions->setWordWrap(true);
  rootLayout->addWidget(instructions);

  auto* formBox = new QGroupBox(tr("Dataset"));
  auto* formLayout = new QVBoxLayout(formBox);
  {
    auto* row = new QHBoxLayout();
    row->addWidget(new QLabel(tr("URL:")));
    m_urlEdit = new QLineEdit();
    m_urlEdit->setPlaceholderText(tr("precomputed://gs://bucket/path or https://host/path"));
    row->addWidget(m_urlEdit, /*stretch=*/1);
    formLayout->addLayout(row);
  }
  {
    auto* row = new QHBoxLayout();
    row->addWidget(new QLabel(tr("Name (optional):")));
    m_nameEdit = new QLineEdit();
    m_nameEdit->setPlaceholderText(tr("Friendly name shown in history"));
    row->addWidget(m_nameEdit, /*stretch=*/1);
    formLayout->addLayout(row);
  }
  rootLayout->addWidget(formBox);

  // Tabs: History + Examples
  auto* tabs = new QTabWidget();

  // History tab
  {
    auto* tab = new QWidget();
    auto* layout = new QVBoxLayout(tab);

    m_historyModel = new QStandardItemModel(0, 2, this);
    m_historyModel->setHorizontalHeaderLabels({tr("Name"), tr("URL")});

    QString historyErr;
    const auto history = ZNeuroglancerPrecomputedDatasetList::loadUserHistory(&historyErr);
    if (!historyErr.isEmpty()) {
      LOG(WARNING) << "Failed to load Neuroglancer history: " << historyErr.toStdString();
    }
    for (const auto& e : history) {
      addHistoryRow(e.name, e.url);
    }

    m_historyView = new QTableView();
    m_historyView->setModel(m_historyModel);
    m_historyView->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_historyView->setSelectionMode(QAbstractItemView::SingleSelection);
    m_historyView->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed);
    m_historyView->horizontalHeader()->setStretchLastSection(true);
    m_historyView->horizontalHeader()->setSectionResizeMode(kHistoryColName, QHeaderView::ResizeToContents);
    m_historyView->horizontalHeader()->setSectionResizeMode(kHistoryColUrl, QHeaderView::Stretch);
    m_historyView->verticalHeader()->setVisible(false);
    m_historyView->setAlternatingRowColors(true);
    layout->addWidget(m_historyView);

    auto* buttonsRow = new QHBoxLayout();
    auto* removeBtn = new QPushButton(tr("Remove Selected"));
    connect(removeBtn, &QPushButton::clicked, this, &ZLoadNeuroglancerPrecomputedDialog::removeSelectedHistory);
    buttonsRow->addWidget(removeBtn);
    buttonsRow->addStretch(1);
    layout->addLayout(buttonsRow);

    wireSelectionToEdits(m_historyView, m_historyModel, kHistoryColName, kHistoryColUrl);

    tabs->addTab(tab, tr("History"));
  }

  // Examples tab
  {
    auto* tab = new QWidget();
    auto* layout = new QVBoxLayout(tab);

    auto* label = new QLabel(
      tr("Examples shipped with Atlas (read-only). Select one to fill the URL above."));
    label->setWordWrap(true);
    layout->addWidget(label);

    m_examplesModel = new QStandardItemModel(0, 3, this);
    m_examplesModel->setHorizontalHeaderLabels({tr("Name"), tr("Kind"), tr("URL")});

    QString examplesErr;
    const auto examples = ZNeuroglancerPrecomputedDatasetList::loadExamples(&examplesErr);
    if (!examplesErr.isEmpty()) {
      LOG(WARNING) << "Failed to load Neuroglancer examples: " << examplesErr.toStdString();
    }
    for (const auto& e : examples) {
      QList<QStandardItem*> row;
      auto* nameItem = new QStandardItem(e.name);
      nameItem->setEditable(false);
      auto* kindItem = new QStandardItem(e.kind);
      kindItem->setEditable(false);
      auto* urlItem = new QStandardItem(e.url);
      urlItem->setEditable(false);
      row << nameItem << kindItem << urlItem;
      m_examplesModel->appendRow(row);
    }

    m_examplesView = new QTableView();
    m_examplesView->setModel(m_examplesModel);
    m_examplesView->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_examplesView->setSelectionMode(QAbstractItemView::SingleSelection);
    m_examplesView->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_examplesView->horizontalHeader()->setStretchLastSection(true);
    m_examplesView->horizontalHeader()->setSectionResizeMode(kExamplesColName, QHeaderView::ResizeToContents);
    m_examplesView->horizontalHeader()->setSectionResizeMode(kExamplesColKind, QHeaderView::ResizeToContents);
    m_examplesView->horizontalHeader()->setSectionResizeMode(kExamplesColUrl, QHeaderView::Stretch);
    m_examplesView->verticalHeader()->setVisible(false);
    m_examplesView->setAlternatingRowColors(true);
    layout->addWidget(m_examplesView);

    wireSelectionToEdits(m_examplesView, m_examplesModel, kExamplesColName, kExamplesColUrl);

    tabs->addTab(tab, tr("Examples"));
  }

  rootLayout->addWidget(tabs, /*stretch=*/1);

  // Buttons
  auto* buttonBox = new QDialogButtonBox(QDialogButtonBox::Open | QDialogButtonBox::Cancel);
  connect(buttonBox, &QDialogButtonBox::accepted, this, [this]() {
    const QString url = selectedUrl().trimmed();
    if (url.isEmpty()) {
      QMessageBox::warning(this, QApplication::applicationName(), tr("Please enter a dataset URL."));
      return;
    }
    accept();
  });
  connect(buttonBox, &QDialogButtonBox::rejected, this, &QDialog::reject);
  rootLayout->addWidget(buttonBox);

  resize(900, 500);
}

void ZLoadNeuroglancerPrecomputedDialog::setInitialUrl(const QString& url)
{
  m_urlEdit->setText(url.trimmed());
  m_urlEdit->selectAll();
  m_urlEdit->setFocus();
}

QString ZLoadNeuroglancerPrecomputedDialog::selectedUrl() const
{
  CHECK(m_urlEdit);
  return m_urlEdit->text().trimmed();
}

QString ZLoadNeuroglancerPrecomputedDialog::selectedName() const
{
  CHECK(m_nameEdit);
  return m_nameEdit->text().trimmed();
}

std::vector<ZNeuroglancerPrecomputedDatasetList::Entry> ZLoadNeuroglancerPrecomputedDialog::userHistoryEntries() const
{
  CHECK(m_historyModel);

  std::vector<ZNeuroglancerPrecomputedDatasetList::Entry> out;
  out.reserve(static_cast<size_t>(m_historyModel->rowCount()));
  for (int r = 0; r < m_historyModel->rowCount(); ++r) {
    ZNeuroglancerPrecomputedDatasetList::Entry e;
    e.name = modelTextOrEmpty(m_historyModel, m_historyModel->index(r, kHistoryColName)).trimmed();
    e.url = modelTextOrEmpty(m_historyModel, m_historyModel->index(r, kHistoryColUrl)).trimmed();
    out.push_back(std::move(e));
  }
  return out;
}

void ZLoadNeuroglancerPrecomputedDialog::addHistoryRow(const QString& name, const QString& url)
{
  CHECK(m_historyModel);

  QList<QStandardItem*> row;
  row << new QStandardItem(name);
  row << new QStandardItem(url);
  m_historyModel->appendRow(row);
}

void ZLoadNeuroglancerPrecomputedDialog::setUrlAndName(const QString& url, const QString& name)
{
  CHECK(m_urlEdit);
  CHECK(m_nameEdit);

  m_urlEdit->setText(url.trimmed());
  m_nameEdit->setText(name.trimmed());
}

void ZLoadNeuroglancerPrecomputedDialog::wireSelectionToEdits(QTableView* view,
                                                              QStandardItemModel* model,
                                                              int nameCol,
                                                              int urlCol)
{
  CHECK(view);
  CHECK(model);

  connect(view->selectionModel(),
          &QItemSelectionModel::selectionChanged,
          this,
          [this, view, model, nameCol, urlCol]() {
    const QModelIndexList rows = view->selectionModel()->selectedRows();
    if (rows.isEmpty()) {
      return;
    }
    const int r = rows.front().row();
    const QString name = modelTextOrEmpty(model, model->index(r, nameCol));
    const QString url = modelTextOrEmpty(model, model->index(r, urlCol));
    setUrlAndName(url, name);
  });
}

void ZLoadNeuroglancerPrecomputedDialog::removeSelectedHistory()
{
  if (!m_historyView || !m_historyModel) {
    return;
  }

  const QModelIndexList rows = m_historyView->selectionModel()->selectedRows();
  if (rows.isEmpty()) {
    return;
  }

  const int r = rows.front().row();
  m_historyModel->removeRow(r);
}

} // namespace nim
