#include "zneuroglancersegmentpropertiesdialog.h"

#include "zexception.h"
#include "zmessageboxhelpers.h"
#include "zneuroglancersegmentpropertiesmodel.h"

#include <QApplication>
#include <QClipboard>
#include <QDialogButtonBox>
#include <QFutureWatcher>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSortFilterProxyModel>
#include <QTableView>
#include <QTimer>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>

namespace nim {

namespace {

struct LoadResult
{
  std::shared_ptr<const ZNeuroglancerPrecomputedSegmentProperties> props;
  QString error;
};

} // namespace

ZNeuroglancerSegmentPropertiesDialog::ZNeuroglancerSegmentPropertiesDialog(std::shared_ptr<ZNeuroglancerPrecomputedVolume> vol,
                                                                           QWidget* parent)
  : QDialog(parent)
  , m_vol(std::move(vol))
{
  CHECK(m_vol);

  setWindowTitle(QString("Segment Properties - %1").arg(m_vol->rootUrl()));
  setModal(false);
  resize(900, 600);

  m_titleLabel = new QLabel(QString("<b>Neuroglancer Segment Properties</b><br>%1").arg(m_vol->rootUrl()), this);
  m_titleLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);

  m_statusLabel = new QLabel(this);
  m_statusLabel->setWordWrap(true);

  m_searchEdit = new QLineEdit(this);
  m_searchEdit->setPlaceholderText(QStringLiteral("Search (any field)…"));
  m_searchEdit->setEnabled(false);

  m_countLabel = new QLabel(this);

  m_table = new QTableView(this);
  m_table->setEnabled(false);
  m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
  m_table->setSelectionMode(QAbstractItemView::ExtendedSelection);
  m_table->setSortingEnabled(true);

  m_copyButton = new QPushButton(QStringLiteral("Copy ID(s)"), this);
  m_copyButton->setEnabled(false);
  connect(m_copyButton, &QPushButton::clicked, this, &ZNeuroglancerSegmentPropertiesDialog::copySelectedIdsToClipboard);

  m_loadMeshButton = new QPushButton(QStringLiteral("Load Mesh(es)"), this);
  m_loadMeshButton->setEnabled(false);
  connect(m_loadMeshButton, &QPushButton::clicked, this, &ZNeuroglancerSegmentPropertiesDialog::loadMeshesForSelectedIds);

  auto* closeButton = new QPushButton(QStringLiteral("Close"), this);
  connect(closeButton, &QPushButton::clicked, this, &QDialog::close);

  auto* buttonRow = new QDialogButtonBox(this);
  buttonRow->addButton(m_copyButton, QDialogButtonBox::ActionRole);
  buttonRow->addButton(m_loadMeshButton, QDialogButtonBox::ActionRole);
  buttonRow->addButton(closeButton, QDialogButtonBox::RejectRole);

  auto* layout = new QVBoxLayout(this);
  layout->addWidget(m_titleLabel);
  layout->addWidget(m_statusLabel);
  layout->addWidget(m_searchEdit);
  layout->addWidget(m_countLabel);
  layout->addWidget(m_table, /*stretch=*/1);
  layout->addWidget(buttonRow);
  setLayout(layout);

  m_filterTimer = new QTimer(this);
  m_filterTimer->setSingleShot(true);
  m_filterTimer->setInterval(200);
  connect(m_filterTimer, &QTimer::timeout, this, &ZNeuroglancerSegmentPropertiesDialog::applyFilterNow);

  connect(m_searchEdit, &QLineEdit::textChanged, this, [this]() {
    if (!m_props) {
      return;
    }
    m_filterTimer->start();
  });

  beginLoad();
}

void ZNeuroglancerSegmentPropertiesDialog::setMeshLoadCallback(std::function<void(uint64_t)> cb)
{
  m_meshLoadCallback = std::move(cb);
  m_loadMeshButton->setEnabled(static_cast<bool>(m_meshLoadCallback) && m_props);
}

void ZNeuroglancerSegmentPropertiesDialog::setAfterPropertiesLoadedCallback(
  std::function<void(std::shared_ptr<const ZNeuroglancerPrecomputedSegmentProperties>)> cb)
{
  m_afterPropertiesLoadedCallback = std::move(cb);
  if (m_props && m_afterPropertiesLoadedCallback) {
    m_afterPropertiesLoadedCallback(m_props);
  }
}

void ZNeuroglancerSegmentPropertiesDialog::beginLoad()
{
  CHECK(m_vol);
  if (!m_vol->hasSegmentPropertiesDirectory()) {
    finishLoad({}, QStringLiteral("This dataset does not specify segment_properties."));
    return;
  }

  if (auto cached = m_vol->segmentPropertiesShared()) {
    finishLoad(std::move(cached), {});
    return;
  }

  m_statusLabel->setText(QStringLiteral("Loading segment_properties…"));

  auto* watcher = new QFutureWatcher<LoadResult>(this);
  m_loadWatcher = watcher;
  connect(watcher, &QFutureWatcher<LoadResult>::finished, this, [this, watcher]() {
    const LoadResult r = watcher->result();
    watcher->deleteLater();
    if (m_loadWatcher == watcher) {
      m_loadWatcher = nullptr;
    }
    finishLoad(r.props, r.error);
  });

  watcher->setFuture(QtConcurrent::run([vol = m_vol]() -> LoadResult {
    LoadResult out;
    try {
      out.props = vol->loadSegmentPropertiesBlocking();
    }
    catch (const ZException& e) {
      out.error = QString::fromUtf8(e.what());
    }
    catch (const std::exception& e) {
      out.error = QString::fromUtf8(e.what());
    }
    return out;
  }));
}

void ZNeuroglancerSegmentPropertiesDialog::finishLoad(std::shared_ptr<const ZNeuroglancerPrecomputedSegmentProperties> props,
                                                     QString error)
{
  if (!error.isEmpty()) {
    m_statusLabel->setText(QString("<b>Failed to load segment_properties</b><br>%1").arg(error.toHtmlEscaped()));
    return;
  }
  if (!props) {
    m_statusLabel->setText(QStringLiteral("No segment properties available."));
    return;
  }

  m_props = std::move(props);
  m_statusLabel->setText(QStringLiteral(""));
  if (m_afterPropertiesLoadedCallback) {
    m_afterPropertiesLoadedCallback(m_props);
  }

  m_model = new ZNeuroglancerSegmentPropertiesModel(m_props, this);

  m_proxy = new QSortFilterProxyModel(this);
  m_proxy->setSourceModel(m_model);
  m_proxy->setFilterCaseSensitivity(Qt::CaseInsensitive);
  m_proxy->setFilterKeyColumn(-1);
  m_proxy->setDynamicSortFilter(true);

  m_table->setModel(m_proxy);
  m_table->horizontalHeader()->setStretchLastSection(true);
  m_table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
  m_table->setEnabled(true);
  m_searchEdit->setEnabled(true);
  m_copyButton->setEnabled(true);
  m_loadMeshButton->setEnabled(static_cast<bool>(m_meshLoadCallback));

  connect(m_proxy, &QAbstractItemModel::modelReset, this, &ZNeuroglancerSegmentPropertiesDialog::updateCounts);
  connect(m_proxy, &QAbstractItemModel::rowsInserted, this, &ZNeuroglancerSegmentPropertiesDialog::updateCounts);
  connect(m_proxy, &QAbstractItemModel::rowsRemoved, this, &ZNeuroglancerSegmentPropertiesDialog::updateCounts);
  updateCounts();

  // Double click row to load mesh.
  connect(m_table, &QTableView::doubleClicked, this, [this](const QModelIndex& idx) {
    if (!m_meshLoadCallback || !m_proxy || !m_model) {
      return;
    }
    const QModelIndex src = m_proxy->mapToSource(idx);
    const auto idOpt = m_model->segmentIdForRow(src.row());
    if (!idOpt) {
      return;
    }
    m_meshLoadCallback(*idOpt);
  });
}

void ZNeuroglancerSegmentPropertiesDialog::applyFilterNow()
{
  if (!m_proxy) {
    return;
  }
  const QString q = m_searchEdit->text().trimmed();
  if (q.isEmpty()) {
    m_proxy->setFilterRegularExpression(QRegularExpression());
  } else {
    QRegularExpression re(QRegularExpression::escape(q), QRegularExpression::CaseInsensitiveOption);
    m_proxy->setFilterRegularExpression(re);
  }
  updateCounts();
}

void ZNeuroglancerSegmentPropertiesDialog::updateCounts()
{
  if (!m_props) {
    m_countLabel->setText(QString());
    return;
  }
  const size_t total = m_props->numIds();
  const size_t shown = m_proxy ? static_cast<size_t>(m_proxy->rowCount()) : total;
  m_countLabel->setText(QString("Segments: %1 (matched: %2)").arg(total).arg(shown));
}

void ZNeuroglancerSegmentPropertiesDialog::copySelectedIdsToClipboard()
{
  if (!m_proxy || !m_model) {
    return;
  }
  const auto selected = m_table->selectionModel()->selectedRows();
  if (selected.isEmpty()) {
    return;
  }

  QStringList lines;
  lines.reserve(selected.size());
  for (const auto& idx : selected) {
    const QModelIndex src = m_proxy->mapToSource(idx);
    const auto idOpt = m_model->segmentIdForRow(src.row());
    if (!idOpt) {
      continue;
    }
    lines << QString::number(*idOpt);
  }
  QApplication::clipboard()->setText(lines.join('\n'));
}

void ZNeuroglancerSegmentPropertiesDialog::loadMeshesForSelectedIds()
{
  if (!m_meshLoadCallback || !m_proxy || !m_model) {
    return;
  }
  const auto selected = m_table->selectionModel()->selectedRows();
  if (selected.isEmpty()) {
    return;
  }

  std::set<uint64_t> ids;
  for (const auto& idx : selected) {
    const QModelIndex src = m_proxy->mapToSource(idx);
    const auto idOpt = m_model->segmentIdForRow(src.row());
    if (!idOpt) {
      continue;
    }
    ids.insert(*idOpt);
  }
  for (const uint64_t id : ids) {
    m_meshLoadCallback(id);
  }
}

} // namespace nim
