#include "zloadneuroglancerprecomputeddialog.h"

#include "zlog.h"

#include <QApplication>
#include <QDialogButtonBox>
#include <QFormLayout>
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

// Small utility model: make every cell show its full DisplayRole text as a tooltip by default.
// This keeps tooltips correct even after in-place edits (history table).
class ZToolTipStandardItemModel final : public QStandardItemModel
{
public:
  using QStandardItemModel::QStandardItemModel;

  QVariant data(const QModelIndex& index, int role) const override
  {
    if (role == Qt::ToolTipRole) {
      const QVariant explicitTip = QStandardItemModel::data(index, Qt::ToolTipRole);
      if (explicitTip.isValid() && !explicitTip.toString().isEmpty()) {
        return explicitTip;
      }
      return QStandardItemModel::data(index, Qt::DisplayRole);
    }
    return QStandardItemModel::data(index, role);
  }
};

constexpr int kHistoryColName = 0;
constexpr int kHistoryColUrl = 1;

constexpr int kExamplesColName = 0;
constexpr int kExamplesColKind = 1;
constexpr int kExamplesColUrl = 2;

constexpr int kRoleHistoryKind = Qt::UserRole + 1;
constexpr int kRoleHistoryMeshSourceOverrideUrl = Qt::UserRole + 2;
constexpr int kRoleHistorySkeletonSourceOverrideUrl = Qt::UserRole + 3;
constexpr int kRoleHistoryAnnotationsSourceOverrideUrl = Qt::UserRole + 4;

[[nodiscard]] QString modelTextOrEmpty(const QStandardItemModel* model, const QModelIndex& index)
{
  if (!model || !index.isValid()) {
    return {};
  }
  const QVariant v = model->data(index, Qt::DisplayRole);
  return v.toString();
}

[[nodiscard]] QString modelRoleTextOrEmpty(const QStandardItemModel* model, const QModelIndex& index, int role)
{
  if (!model || !index.isValid()) {
    return {};
  }
  const QVariant v = model->data(index, role);
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
    m_urlEdit->setPlaceholderText(tr("precomputed://gs://bucket/path, precomputed://s3://bucket/path, or https://host/path"));
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

    m_historyModel = new ZToolTipStandardItemModel(0, 2, this);
    m_historyModel->setHorizontalHeaderLabels({tr("Name"), tr("URL")});

    QString historyErr;
    const auto history = ZNeuroglancerPrecomputedDatasetList::loadUserHistory(&historyErr);
    if (!historyErr.isEmpty()) {
      LOG(WARNING) << "Failed to load Neuroglancer history: " << historyErr.toStdString();
    }
    for (const auto& e : history) {
      addHistoryRow(e);
    }

    m_historyView = new QTableView();
    m_historyView->setModel(m_historyModel);
    m_historyView->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_historyView->setSelectionMode(QAbstractItemView::SingleSelection);
    m_historyView->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed);
    m_historyView->horizontalHeader()->setStretchLastSection(true);
    // Allow the user to reclaim space from long names (URL is last + stretched, so it will take
    // whatever remains). `ResizeToContents` would lock the column width and can starve the URL.
    m_historyView->horizontalHeader()->setSectionResizeMode(kHistoryColName, QHeaderView::Interactive);
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

    auto* sourcesBox = new QGroupBox(tr("Sources (optional)"));
    auto* sourcesLayout = new QVBoxLayout(sourcesBox);
    {
      auto* meshRow = new QHBoxLayout();
      meshRow->addWidget(new QLabel(tr("Mesh source override:")));
      m_historyMeshSourceLabel = new QLabel(tr("<none>"));
      m_historyMeshSourceLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
      m_historyMeshSourceLabel->setWordWrap(true);
      meshRow->addWidget(m_historyMeshSourceLabel, /*stretch=*/1);
      sourcesLayout->addLayout(meshRow);
    }
    {
      auto* skelRow = new QHBoxLayout();
      skelRow->addWidget(new QLabel(tr("Skeleton source override:")));
      m_historySkeletonSourceLabel = new QLabel(tr("<none>"));
      m_historySkeletonSourceLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
      m_historySkeletonSourceLabel->setWordWrap(true);
      skelRow->addWidget(m_historySkeletonSourceLabel, /*stretch=*/1);
      sourcesLayout->addLayout(skelRow);
    }
    {
      auto* annRow = new QHBoxLayout();
      annRow->addWidget(new QLabel(tr("Annotations source override:")));
      m_historyAnnotationsSourceLabel = new QLabel(tr("<none>"));
      m_historyAnnotationsSourceLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
      m_historyAnnotationsSourceLabel->setWordWrap(true);
      annRow->addWidget(m_historyAnnotationsSourceLabel, /*stretch=*/1);
      sourcesLayout->addLayout(annRow);
    }
    {
      auto* row = new QHBoxLayout();
      m_historyEditSourcesBtn = new QPushButton(tr("Edit Sources..."));
      connect(m_historyEditSourcesBtn,
              &QPushButton::clicked,
              this,
              &ZLoadNeuroglancerPrecomputedDialog::editSelectedHistorySources);
      row->addWidget(m_historyEditSourcesBtn);

      m_historyClearSourcesBtn = new QPushButton(tr("Clear Sources"));
      connect(m_historyClearSourcesBtn,
              &QPushButton::clicked,
              this,
              &ZLoadNeuroglancerPrecomputedDialog::clearSelectedHistorySources);
      row->addWidget(m_historyClearSourcesBtn);

      row->addStretch(1);
      sourcesLayout->addLayout(row);
    }
    layout->addWidget(sourcesBox);

    wireSelectionToEdits(m_historyView, m_historyModel, kHistoryColName, kHistoryColUrl);
    connect(m_historyView->selectionModel(),
            &QItemSelectionModel::selectionChanged,
            this,
            [this](const QItemSelection&, const QItemSelection&) { updateHistorySourceUi(); });
    updateHistorySourceUi();

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

    m_examplesModel = new ZToolTipStandardItemModel(0, 3, this);
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
    // Make columns user-resizable; keep URL as the stretched last column.
    m_examplesView->horizontalHeader()->setSectionResizeMode(kExamplesColName, QHeaderView::Interactive);
    m_examplesView->horizontalHeader()->setSectionResizeMode(kExamplesColKind, QHeaderView::Interactive);
    m_examplesView->horizontalHeader()->setSectionResizeMode(kExamplesColUrl, QHeaderView::Stretch);
    m_examplesView->verticalHeader()->setVisible(false);
    m_examplesView->setAlternatingRowColors(true);
    layout->addWidget(m_examplesView);

    // The "Kind" values are short; size the column once to avoid wasting URL space while still
    // keeping it interactively resizable.
    m_examplesView->resizeColumnToContents(kExamplesColKind);

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

  // Favor a taller default so the history table shows more entries (the Sources panel is useful
  // but should not crowd out the list).
  resize(900, 650);
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
    e.kind = modelRoleTextOrEmpty(m_historyModel, m_historyModel->index(r, kHistoryColUrl), kRoleHistoryKind).trimmed();
    e.meshSourceOverrideUrl =
      modelRoleTextOrEmpty(m_historyModel, m_historyModel->index(r, kHistoryColUrl), kRoleHistoryMeshSourceOverrideUrl)
        .trimmed();
    e.skeletonSourceOverrideUrl =
      modelRoleTextOrEmpty(m_historyModel,
                           m_historyModel->index(r, kHistoryColUrl),
                           kRoleHistorySkeletonSourceOverrideUrl)
        .trimmed();
    e.annotationsSourceOverrideUrl =
      modelRoleTextOrEmpty(m_historyModel,
                           m_historyModel->index(r, kHistoryColUrl),
                           kRoleHistoryAnnotationsSourceOverrideUrl)
        .trimmed();
    out.push_back(std::move(e));
  }
  return out;
}

void ZLoadNeuroglancerPrecomputedDialog::addHistoryRow(const ZNeuroglancerPrecomputedDatasetList::Entry& entry)
{
  CHECK(m_historyModel);

  QList<QStandardItem*> row;
  row << new QStandardItem(entry.name);
  auto* urlItem = new QStandardItem(entry.url);
  urlItem->setData(entry.kind, kRoleHistoryKind);
  urlItem->setData(entry.meshSourceOverrideUrl, kRoleHistoryMeshSourceOverrideUrl);
  urlItem->setData(entry.skeletonSourceOverrideUrl, kRoleHistorySkeletonSourceOverrideUrl);
  urlItem->setData(entry.annotationsSourceOverrideUrl, kRoleHistoryAnnotationsSourceOverrideUrl);
  row << urlItem;
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
  updateHistorySourceUi();
}

void ZLoadNeuroglancerPrecomputedDialog::updateHistorySourceUi()
{
  if (!m_historyView || !m_historyModel || !m_historyMeshSourceLabel || !m_historySkeletonSourceLabel ||
      !m_historyAnnotationsSourceLabel || !m_historyEditSourcesBtn || !m_historyClearSourcesBtn) {
    return;
  }

  const QModelIndexList rows = m_historyView->selectionModel()->selectedRows();
  const bool hasSel = !rows.isEmpty();
  m_historyEditSourcesBtn->setEnabled(hasSel);
  m_historyClearSourcesBtn->setEnabled(hasSel);

  QString mesh;
  QString skel;
  QString ann;
  if (hasSel) {
    const int r = rows.front().row();
    const QModelIndex urlIdx = m_historyModel->index(r, kHistoryColUrl);
    mesh = modelRoleTextOrEmpty(m_historyModel, urlIdx, kRoleHistoryMeshSourceOverrideUrl).trimmed();
    skel = modelRoleTextOrEmpty(m_historyModel, urlIdx, kRoleHistorySkeletonSourceOverrideUrl).trimmed();
    ann = modelRoleTextOrEmpty(m_historyModel, urlIdx, kRoleHistoryAnnotationsSourceOverrideUrl).trimmed();
  }

  m_historyMeshSourceLabel->setText(mesh.isEmpty() ? tr("<none>") : mesh);
  m_historySkeletonSourceLabel->setText(skel.isEmpty() ? tr("<none>") : skel);
  m_historyAnnotationsSourceLabel->setText(ann.isEmpty() ? tr("<none>") : ann);
}

void ZLoadNeuroglancerPrecomputedDialog::editSelectedHistorySources()
{
  if (!m_historyView || !m_historyModel) {
    return;
  }

  const QModelIndexList rows = m_historyView->selectionModel()->selectedRows();
  if (rows.isEmpty()) {
    return;
  }

  const int r = rows.front().row();
  const QModelIndex urlIdx = m_historyModel->index(r, kHistoryColUrl);
  const QString currentMesh = modelRoleTextOrEmpty(m_historyModel, urlIdx, kRoleHistoryMeshSourceOverrideUrl).trimmed();
  const QString currentSkel =
    modelRoleTextOrEmpty(m_historyModel, urlIdx, kRoleHistorySkeletonSourceOverrideUrl).trimmed();
  const QString currentAnn =
    modelRoleTextOrEmpty(m_historyModel, urlIdx, kRoleHistoryAnnotationsSourceOverrideUrl).trimmed();

  QDialog dlg(this);
  dlg.setWindowTitle(tr("Edit Neuroglancer Sources"));
  auto* layout = new QVBoxLayout(&dlg);

  auto* help = new QLabel(tr("Optional per-dataset overrides for Neuroglancer mesh/skeleton/annotations sources.\n"
                             "Leave blank to clear. Values may be absolute URLs or dataset-relative paths."));
  help->setWordWrap(true);
  layout->addWidget(help);

  auto* form = new QFormLayout();
  auto* meshEdit = new QLineEdit(currentMesh);
  meshEdit->setPlaceholderText(tr("e.g. precomputed://... or relative path"));
  form->addRow(tr("Mesh source override:"), meshEdit);

  auto* skelEdit = new QLineEdit(currentSkel);
  skelEdit->setPlaceholderText(tr("e.g. precomputed://... or relative path"));
  form->addRow(tr("Skeleton source override:"), skelEdit);

  auto* annEdit = new QLineEdit(currentAnn);
  annEdit->setPlaceholderText(tr("e.g. precomputed://... or relative path"));
  form->addRow(tr("Annotations source override:"), annEdit);
  layout->addLayout(form);

  auto* buttonBox = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
  connect(buttonBox, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
  connect(buttonBox, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
  layout->addWidget(buttonBox);

  if (dlg.exec() != QDialog::Accepted) {
    return;
  }

  const QString newMesh = meshEdit->text().trimmed();
  const QString newSkel = skelEdit->text().trimmed();
  const QString newAnn = annEdit->text().trimmed();

  m_historyModel->setData(urlIdx, newMesh, kRoleHistoryMeshSourceOverrideUrl);
  m_historyModel->setData(urlIdx, newSkel, kRoleHistorySkeletonSourceOverrideUrl);
  m_historyModel->setData(urlIdx, newAnn, kRoleHistoryAnnotationsSourceOverrideUrl);
  updateHistorySourceUi();
}

void ZLoadNeuroglancerPrecomputedDialog::clearSelectedHistorySources()
{
  if (!m_historyView || !m_historyModel) {
    return;
  }

  const QModelIndexList rows = m_historyView->selectionModel()->selectedRows();
  if (rows.isEmpty()) {
    return;
  }

  const int r = rows.front().row();
  const QModelIndex urlIdx = m_historyModel->index(r, kHistoryColUrl);
  m_historyModel->setData(urlIdx, QString(), kRoleHistoryMeshSourceOverrideUrl);
  m_historyModel->setData(urlIdx, QString(), kRoleHistorySkeletonSourceOverrideUrl);
  m_historyModel->setData(urlIdx, QString(), kRoleHistoryAnnotationsSourceOverrideUrl);
  updateHistorySourceUi();
}

} // namespace nim
