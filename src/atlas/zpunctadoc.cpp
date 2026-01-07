#include "zpunctadoc.h"

#include "zneuroglancerprecomputed.h"
#include "zneuroglancerprecomputedannotations.h"
#include "zpunctadetectiondialog.h"
#include "zimgdoc.h"
#include "zanalysisworklistdialog.h"
#include "ztheme.h"
#include "zpunctawidget.h"
#include "zmessageboxhelpers.h"
#include <QFileDialog>
#include <QSettings>
#include <QApplication>

#include <string>

namespace nim {

ZPunctaDoc::ZPunctaDoc(ZDoc& doc)
  : ZObjDoc(doc)
{
  createActions();
}

bool ZPunctaDoc::save(size_t id)
{
  if (!objHasUnsavedChange(id)) {
    return true;
  }

  auto& pack = m_idToPunctaPacks.at(id);
  if (ZPuncta::canWriteFile(pack->path())) {
    QString err;
    if (savePuncta(pack.get(), pack->path(), err)) {
      m_doc.updateObjInfo(id);
      return true;
    }
    showCriticalWithDetails(QApplication::activeWindow(), tr("Can not save puncta %1").arg(pack->path()), err);
    return false;
  }
  return saveAs(id);
}

bool ZPunctaDoc::saveAs(size_t id)
{
  QStringList filters;
  QStringList formats;
  ZPuncta::getQtWriteNameFilter(filters, formats);

  QFileDialog dialog(QApplication::activeWindow());
  dialog.setAcceptMode(QFileDialog::AcceptSave);
  dialog.setFileMode(QFileDialog::AnyFile);
  dialog.setNameFilters(filters);
  dialog.setDirectory(lastOpenedObjPath());
  dialog.setWindowTitle(tr("Save Puncta %1 As").arg(objName(id)));
  if (dialog.exec()) {
    QString err;
    auto& pack = m_idToPunctaPacks.at(id);
    const QString targetPath = dialog.selectedFiles().at(0);
    if (auto fmtIdx = filters.indexOf(dialog.selectedNameFilter());
        savePuncta(pack.get(), targetPath, err, formats[fmtIdx])) {
      m_doc.updateObjInfo(id);
      return true;
    }
    showCriticalWithDetails(QApplication::activeWindow(), tr("Can not save puncta %1").arg(targetPath), err);
  }
  return false;
}

bool ZPunctaDoc::canReadFile(const QString& fileName) const
{
  return ZPuncta::canReadFile(fileName);
}

size_t ZPunctaDoc::loadFile(const QString& fileName, QString& errorMsg)
{
  for (const auto& [id, pack] : m_idToPunctaPacks) {
    if (pack->path() == fileName) {
      return id;
    }
  }
  try {
    size_t id = addPuncta(ZPuncta(fileName), fileName);
    ZSystemInfo::instance().addFileToRecentFileList(fileName);
    setLastOpenedObjPath(fileName);
    return id;
  }
  catch (const ZException& e) {
    errorMsg = e.what();
    return 0;
  }
}

size_t ZPunctaDoc::loadFile(const json::value& jValue, QString& errorMsg)
{
  try {
    if (jValue.is_object()) {
      constexpr std::chrono::milliseconds kDefaultTimeout{30000};

      const auto& jo = jValue.as_object();
      auto typeIt = jo.find("type");
      if (typeIt == jo.end() || !typeIt->value().is_string()) {
        errorMsg = QString("Invalid puncta JSON: missing string field 'type'");
        return 0;
      }
      const QString type = json::value_to<QString>(typeIt->value()).trimmed();
      if (type != "neuroglancer_precomputed_annotations") {
        errorMsg = QString("Unsupported puncta JSON type '%1'").arg(type);
        return 0;
      }

      auto rootIt = jo.find("segmentation_root_url");
      if (rootIt == jo.end() || !rootIt->value().is_string()) {
        errorMsg = QString("Invalid neuroglancer annotations JSON: missing string field 'segmentation_root_url'");
        return 0;
      }
      const QString normalizedSegRootUrl =
        ZNeuroglancerPrecomputedVolume::normalizeRootUrl(json::value_to<QString>(rootIt->value()));

      auto annIt = jo.find("annotation_root_url");
      if (annIt == jo.end() || !annIt->value().is_string()) {
        errorMsg = QString("Invalid neuroglancer annotations JSON: missing string field 'annotation_root_url'");
        return 0;
      }
      const QString normalizedAnnRootUrl =
        ZNeuroglancerPrecomputedVolume::normalizeRootUrl(json::value_to<QString>(annIt->value()));

      auto relIt = jo.find("relationship_id");
      if (relIt == jo.end() || !relIt->value().is_string()) {
        errorMsg = QString("Invalid neuroglancer annotations JSON: missing string field 'relationship_id'");
        return 0;
      }
      const QString relationshipId = json::value_to<QString>(relIt->value()).trimmed();
      if (relationshipId.isEmpty()) {
        errorMsg = QString("Invalid neuroglancer annotations JSON: relationship_id must be non-empty");
        return 0;
      }

      auto objIt = jo.find("object_id");
      if (objIt == jo.end() || !objIt->value().is_string()) {
        errorMsg = QString("Invalid neuroglancer annotations JSON: missing string field 'object_id'");
        return 0;
      }
      const QString objStr = json::value_to<QString>(objIt->value()).trimmed();
      bool ok = false;
      const qulonglong objIdQt = objStr.toULongLong(&ok, 10);
      if (!ok) {
        errorMsg = QString("Invalid neuroglancer annotations JSON: object_id must be base-10 uint64");
        return 0;
      }
      const uint64_t objectId = static_cast<uint64_t>(objIdQt);

      // Normalize persisted JSON to keep it stable across save/load cycles.
      json::object normalized;
      normalized["type"] = "neuroglancer_precomputed_annotations";
      normalized["segmentation_root_url"] = json::value_from(normalizedSegRootUrl);
      normalized["annotation_root_url"] = json::value_from(normalizedAnnRootUrl);
      normalized["relationship_id"] = json::value_from(relationshipId);
      normalized["object_id"] = json::value_from(QString::number(static_cast<qulonglong>(objectId)));
      const json::value sourceJson = normalized;

      if (const auto existing = findPunctaByExternalSource(sourceJson)) {
        return *existing;
      }

      std::shared_ptr<ZNeuroglancerPrecomputedVolume> segVol =
        ZNeuroglancerPrecomputedVolume::open(normalizedSegRootUrl, kDefaultTimeout);
      CHECK(segVol);

      std::array<double, 3> baseResNm{segVol->baseImgInfo().voxelSizeX, segVol->baseImgInfo().voxelSizeY, segVol->baseImgInfo().voxelSizeZ};
      auto source =
        ZNeuroglancerPrecomputedAnnotationsSource::open(QUrl(normalizedAnnRootUrl), baseResNm, segVol->baseVoxelOffset(), kDefaultTimeout);
      CHECK(source);

      if (source->annotationType() != ZNeuroglancerPrecomputedAnnotationsSource::AnnotationType::Point) {
        errorMsg = QString("Unsupported neuroglancer annotations type for puncta: expected POINT");
        return 0;
      }

      const auto anns = source->loadAnnotationsForRelatedObjectBlocking(relationshipId, objectId);
      if (anns.empty()) {
        errorMsg = QString("No annotations found for object %1 (relationship '%2')")
                     .arg(static_cast<qulonglong>(objectId))
                     .arg(relationshipId);
        return 0;
      }

      std::list<ZPunctum> pts;
      for (const auto& a : anns) {
        if (a.points.size() != 1) {
          continue;
        }
        const auto& p = a.points[0];
        ZPunctum punctum(p.x, p.y, p.z, /*r=*/2.0);
        punctum.name = std::to_string(static_cast<unsigned long long>(a.id));
        punctum.setMaxIntensity(255.0);
        punctum.setMeanIntensity(255.0);
        if (a.rgba8) {
          const auto& c = *a.rgba8;
          punctum.setColor(col4(c[0], c[1], c[2], c[3]));
        }
        pts.push_back(std::move(punctum));
      }
      if (pts.empty()) {
        errorMsg = QString("No POINT annotations decoded for object %1 (relationship '%2')")
                     .arg(static_cast<qulonglong>(objectId))
                     .arg(relationshipId);
        return 0;
      }

      const QString displayName = QString("NG Annotations %1 (%2)").arg(static_cast<qulonglong>(objectId)).arg(relationshipId);
      const QString tooltip = QString("Neuroglancer precomputed annotations\nSegmentation: %1\nAnnotations: %2\nRelationship: %3\nObject: %4\nCount: %5")
                                .arg(normalizedSegRootUrl)
                                .arg(normalizedAnnRootUrl)
                                .arg(relationshipId)
                                .arg(static_cast<qulonglong>(objectId))
                                .arg(static_cast<qulonglong>(pts.size()));

      ZPuncta puncta;
      puncta.data = std::move(pts);
      return addPunctaFromExternalSource(std::move(puncta), displayName, tooltip, sourceJson);
    }

    if (!jValue.is_string() || asQString(jValue).trimmed().isEmpty()) {
      errorMsg = QString("File path is not string or is empty");
      return 0;
    }

    for (const auto& [id, pack] : m_idToPunctaPacks) {
      if (isSameObj(jValue, jsonValue(id))) {
        return id;
      }
    }

    const QString fileName = asQString(jValue);
    size_t id = addPuncta(ZPuncta(fileName), fileName);
    ZSystemInfo::instance().addFileToRecentFileList(fileName);
    setLastOpenedObjPath(fileName);
    return id;
  }
  catch (const ZException& e) {
    errorMsg = e.what();
    return 0;
  }
}

std::vector<QAction*> ZPunctaDoc::loadFileActions() const
{
  std::vector<QAction*> res;
  res.push_back(m_loadPunctaAction);
  return res;
}

QMenu* ZPunctaDoc::processObjMenu() const
{
  auto res = new QMenu(typeName());
  res->addAction(m_detectPunctaAction);
  res->addAction(m_generateAnalysisTextFilesAction);
  return res;
}

void ZPunctaDoc::removeObj(size_t id)
{
  auto it = m_idToPunctaPacks.find(id);
  Q_EMIT objAboutToBeRemoved(it->first, this);
  m_idToPunctaPacks.erase(it);
  Q_EMIT objRemoved(id, this);
}

QString ZPunctaDoc::objName(size_t id) const
{
  return m_idToPunctaPacks.at(id)->name();
}

QString ZPunctaDoc::objPath(size_t id) const
{
  return m_idToPunctaPacks.at(id)->path();
}

bool ZPunctaDoc::objHasUnsavedChange(size_t id) const
{
  return m_idToPunctaPacks.at(id)->hasUnsavedChange;
}

QString ZPunctaDoc::objInfo(size_t id) const
{
  return m_idToPunctaPacks.at(id)->info();
}

QString ZPunctaDoc::objTooltip(size_t id) const
{
  return m_idToPunctaPacks.at(id)->tooltip();
}

const QUndoStack* ZPunctaDoc::objUndoStack(size_t id) const
{
  return m_idToPunctaPacks.at(id)->undoStack();
}

json::value ZPunctaDoc::jsonValue(size_t id) const
{
  const auto& pack = m_idToPunctaPacks.at(id);
  if (!pack->sourceJson.is_null()) {
    return pack->sourceJson;
  }
  return json::value_from(pack->path());
}

bool ZPunctaDoc::isSameObj(const json::value& v1, const json::value& v2) const
{
  if (v1.is_object() && v2.is_object()) {
    const auto& o1 = v1.as_object();
    const auto& o2 = v2.as_object();

    auto keyFor = [](const json::object& o) -> std::optional<QString> {
      auto itType = o.find("type");
      if (itType == o.end() || !itType->value().is_string()) {
        return std::nullopt;
      }
      const QString type = json::value_to<QString>(itType->value()).trimmed();
      if (type != "neuroglancer_precomputed_annotations") {
        return std::nullopt;
      }

      auto itSegRoot = o.find("segmentation_root_url");
      auto itAnnRoot = o.find("annotation_root_url");
      auto itRel = o.find("relationship_id");
      auto itObj = o.find("object_id");
      if (itSegRoot == o.end() || !itSegRoot->value().is_string() || itAnnRoot == o.end() || !itAnnRoot->value().is_string() ||
          itRel == o.end() || !itRel->value().is_string() || itObj == o.end() || !itObj->value().is_string()) {
        return std::nullopt;
      }

      QString segRoot = json::value_to<QString>(itSegRoot->value());
      QString annRoot = json::value_to<QString>(itAnnRoot->value());
      try {
        segRoot = ZNeuroglancerPrecomputedVolume::normalizeRootUrl(std::move(segRoot));
        annRoot = ZNeuroglancerPrecomputedVolume::normalizeRootUrl(std::move(annRoot));
      }
      catch (...) {
        return std::nullopt;
      }

      const QString rel = json::value_to<QString>(itRel->value()).trimmed();
      const QString obj = json::value_to<QString>(itObj->value()).trimmed();
      return QString("%1|%2|%3|%4|%5").arg(type, segRoot, annRoot, rel, obj);
    };

    const auto k1 = keyFor(o1);
    const auto k2 = keyFor(o2);
    return k1 && k2 && *k1 == *k2;
  }

  if (v1.is_string() && v2.is_string()) {
    if (v1 == v2) {
      return true;
    }
    QString f1 = asQString(v1);
    QString f2 = asQString(v2);
    if (!QFile::exists(f1) || !QFile::exists(f2)) {
      return false;
    }
    return QFileInfo(f1).canonicalFilePath() == QFileInfo(f2).canonicalFilePath();
  }

  return false;
}

size_t ZPunctaDoc::makeAlias(size_t id)
{
  CHECK(m_idToPunctaPacks.contains(id));

  size_t aliasId = m_doc.getNewObjId();
  m_idToPunctaPacks[aliasId] = m_idToPunctaPacks[id];
  m_doc.registerNewObj(aliasId, *this);

  Q_EMIT objAdded(aliasId, this);
  return aliasId;
}

bool ZPunctaDoc::isAlias(size_t id) const
{
  CHECK(m_idToPunctaPacks.contains(id));

  return std::ranges::any_of(m_idToPunctaPacks, [&, this](const auto& idPack) {
    return idPack.first != id && idPack.second == m_idToPunctaPacks.at(id);
  });
}

QWidget* ZPunctaDoc::createObjEditWidget(size_t id)
{
  CHECK(m_idToPunctaPacks.contains(id));

  return new ZPunctaWidget(punctaPack(id), m_doc);
}

void ZPunctaDoc::loadPuncta()
{
  QFileDialog dialog(QApplication::activeWindow());
  dialog.setFileMode(QFileDialog::ExistingFiles);
  dialog.setNameFilter(ZPuncta::getQtReadNameFilter());
  dialog.setDirectory(lastOpenedObjPath());
  dialog.setWindowTitle("Load Puncta File");
  if (dialog.exec()) {
    QString errorMsg;
    // auto fmtIdx = filters.indexOf(dialog.selectedNameFilter());
    for (index_t i = 0; i < dialog.selectedFiles().size(); ++i) {
      const QString filePath = dialog.selectedFiles().at(i);
      if (!loadFile(filePath, errorMsg)) {
        showCriticalWithDetails(QApplication::activeWindow(), tr("Can not load puncta %1").arg(filePath), errorMsg);
      }
    }
  }
}

void ZPunctaDoc::detectPuncta()
{
  ZPunctaDetectionDialog dlg(QApplication::activeWindow());
  dlg.exec();
}

void ZPunctaDoc::generateAnalysisTextFiles()
{
  ZAnalysisWorklistDialog dia(QApplication::activeWindow());
  dia.exec();
}

size_t ZPunctaDoc::addPuncta(ZPuncta puncta, const QString& path)
{
  size_t id = m_doc.getNewObjId();
  m_idToPunctaPacks[id] = std::make_shared<ZPunctaPack>(puncta, path, id, *this);
  m_idToPunctaPacks[id]->hasUnsavedChange = false;
  m_doc.registerNewObj(m_idToPunctaPacks[id]);

  Q_EMIT objAdded(id, this);
  connect(m_idToPunctaPacks[id].get(), &ZPunctaPack::undoStackCleanChanged, this, &ZPunctaDoc::setModified);
  return id;
}

size_t ZPunctaDoc::addPunctaFromExternalSource(ZPuncta puncta, QString displayName, QString tooltip, json::value sourceJson)
{
  size_t id = m_doc.getNewObjId();
  m_idToPunctaPacks[id] = std::make_shared<ZPunctaPack>(std::move(puncta), /*path=*/QString{}, id, *this);
  auto& pack = *m_idToPunctaPacks.at(id);
  pack.sourceJson = std::move(sourceJson);
  pack.displayNameOverride = std::move(displayName);
  pack.tooltipOverride = std::move(tooltip);
  pack.updateDerivedData();
  pack.hasUnsavedChange = false;

  m_doc.registerNewObj(m_idToPunctaPacks[id]);

  Q_EMIT objAdded(id, this);
  connect(m_idToPunctaPacks[id].get(), &ZPunctaPack::undoStackCleanChanged, this, &ZPunctaDoc::setModified);
  return id;
}

std::optional<size_t> ZPunctaDoc::findPunctaByExternalSource(const json::value& sourceJson) const
{
  for (const auto& [id, pack] : m_idToPunctaPacks) {
    if (pack && isSameObj(pack->sourceJson, sourceJson)) {
      return id;
    }
  }
  return std::nullopt;
}

void ZPunctaDoc::updateExternalPunctaMetadata(size_t id, QString displayName, QString tooltip)
{
  CHECK(m_idToPunctaPacks.contains(id));
  auto& pack = m_idToPunctaPacks.at(id);
  CHECK(!pack->sourceJson.is_null()) << "updateExternalPunctaMetadata is only valid for external-source puncta";

  pack->displayNameOverride = std::move(displayName);
  pack->tooltipOverride = std::move(tooltip);
  pack->updateDerivedData();
  packInfoUpdated(pack.get());
}

void ZPunctaDoc::setModified(bool)
{
  if (auto ra = qobject_cast<ZPunctaPack*>(sender())) {
    for (const auto& [id, pack] : m_idToPunctaPacks) {
      if (pack.get() == ra) {
        pack->updateDerivedData();
        pack->hasUnsavedChange = !pack->undoStack()->isClean();
        m_doc.updateObjInfo(id);
        return;
      }
    }
  }
}

void ZPunctaDoc::createActions()
{
  m_loadPunctaAction = new QAction(ZTheme::instance().icon(ZTheme::LoadObjectIcon), tr("&Load Puncta..."), this);
  m_loadPunctaAction->setStatusTip(tr("Load one or more existing puncta files"));
  connect(m_loadPunctaAction, &QAction::triggered, this, &ZPunctaDoc::loadPuncta);

  m_detectPunctaAction = new QAction(tr("&Detect Puncta..."), this);
  m_detectPunctaAction->setStatusTip(tr("Auto Detect Puncta"));
  connect(m_detectPunctaAction, &QAction::triggered, this, &ZPunctaDoc::detectPuncta);

  m_generateAnalysisTextFilesAction = new QAction(tr("&Generate Analysis Text Files..."), this);
  m_generateAnalysisTextFilesAction->setStatusTip(tr("Generate Analysis Text Files from input list"));
  connect(m_generateAnalysisTextFilesAction, &QAction::triggered, this, &ZPunctaDoc::generateAnalysisTextFiles);
}

bool ZPunctaDoc::savePuncta(ZPunctaPack* pack, const QString& fileName, QString& errorMsg, const QString& format)
{
  try {
    pack->save(fileName, format);

    ZSystemInfo::instance().addFileToRecentFileList(fileName);
    setLastOpenedObjPath(fileName);
    return true;
  }
  catch (const ZException& e) {
    errorMsg = e.what();
    return false;
  }
}

void ZPunctaDoc::packInfoUpdated(ZPunctaPack* pack)
{
  for (const auto& [id, ppack] : m_idToPunctaPacks) {
    if (ppack.get() == pack) {
      m_doc.updateObjInfo(id);
    }
  }
}

} // namespace nim
