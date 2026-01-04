#include "zmeshdoc.h"

#include "zexception.h"
#include "zimgdoc.h"
#include "zlog.h"
#include "zneuroglancerprecomputed.h"
#include "zneuroglancerprecomputedmesh.h"
#include "ztheme.h"
#include "zmessageboxhelpers.h"
#include <QFileDialog>
#include <QSettings>
#include <QApplication>
#include <set>

namespace nim {

ZMeshDoc::ZMeshDoc(ZDoc& doc)
  : ZObjDoc(doc)
{
  createActions();
}

size_t ZMeshDoc::addMeshFromExternalSource(ZMesh& mesh, QString displayName, QString tooltip, json::value sourceJson)
{
  for (const auto& idPack : m_idToMeshPacks) {
    if (isSameObj(sourceJson, jsonValue(idPack.first))) {
      return idPack.first;
    }
  }

  size_t id = m_doc.getNewObjId();
  m_idToMeshPacks[id] = std::make_shared<MeshPack>(mesh, std::move(displayName), std::move(tooltip), std::move(sourceJson));
  m_doc.registerNewObj(id, *this);

  Q_EMIT objAdded(id, this);
  return id;
}

void ZMeshDoc::askToSave(const ZMesh& msh, const QString& title)
{
  QStringList filters;
  std::vector<std::string> formats;
  ZMesh::getQtWriteNameFilter(filters, formats);

  QFileDialog dialog(QApplication::activeWindow());
  dialog.setAcceptMode(QFileDialog::AcceptSave);
  dialog.setFileMode(QFileDialog::AnyFile);
  dialog.setNameFilters(filters);
  dialog.setDirectory(lastOpenedObjPath());
  if (title.isEmpty()) {
    dialog.setWindowTitle(tr("Save Mesh As"));
  } else {
    dialog.setWindowTitle(title);
  }

  if (dialog.exec()) {
    auto fmtIdx = filters.indexOf(dialog.selectedNameFilter());

    try {
      msh.save(dialog.selectedFiles().at(0), formats[fmtIdx]);

      ZSystemInfo::instance().addFileToRecentFileList(dialog.selectedFiles().at(0));
      setLastOpenedObjPath(dialog.selectedFiles().at(0));
    }
    catch (const ZException& e) {
      showCriticalWithDetails(QApplication::activeWindow(),
                              tr("Can not save mesh %1").arg(dialog.selectedFiles().at(0)),
                              e.what());
    }
  }
}

bool ZMeshDoc::save(size_t id)
{
  if (!objHasUnsavedChange(id)) {
    return true;
  }

  auto& pack = m_idToMeshPacks.at(id);
  if (ZMesh::canWriteFile(pack->path)) {
    QString err;
    if (saveMesh(pack.get(), pack->path, err)) {
      m_doc.updateObjInfo(id);
      return true;
    }
    showCriticalWithDetails(QApplication::activeWindow(), tr("Can not save mesh %1").arg(pack->path), err);
    return false;
  }
  return saveAs(id);
}

bool ZMeshDoc::saveAs(size_t id)
{
  QStringList filters;
  std::vector<std::string> formats;
  ZMesh::getQtWriteNameFilter(filters, formats);

  QFileDialog dialog(QApplication::activeWindow());
  dialog.setAcceptMode(QFileDialog::AcceptSave);
  dialog.setFileMode(QFileDialog::AnyFile);
  dialog.setNameFilters(filters);
  dialog.setDirectory(lastOpenedObjPath());
  dialog.setWindowTitle(tr("Save Mesh %1 As").arg(objName(id)));
  if (dialog.exec()) {
    QString err;
    auto& pack = m_idToMeshPacks.at(id);
    auto fmtIdx = filters.indexOf(dialog.selectedNameFilter());
    const QString targetPath = dialog.selectedFiles().at(0);
    if (saveMesh(pack.get(), targetPath, err, formats[fmtIdx])) {
      m_doc.updateObjInfo(id);
      return true;
    }
    showCriticalWithDetails(QApplication::activeWindow(), tr("Can not save mesh %1").arg(targetPath), err);
  }
  return false;
}

bool ZMeshDoc::canReadFile(const QString& fileName) const
{
  return ZMesh::canReadFile(fileName);
}

size_t ZMeshDoc::loadFile(const QString& fileName, QString& errorMsg)
{
  for (const auto& idPack : m_idToMeshPacks) {
    if (idPack.second->path == fileName) {
      return idPack.first;
    }
  }
  try {
    ZMesh mesh(fileName);
    size_t id = addMesh(mesh, fileName);
    ZSystemInfo::instance().addFileToRecentFileList(fileName);
    setLastOpenedObjPath(fileName);
    return id;
  }
  catch (const ZException& e) {
    errorMsg = e.what();
    return 0;
  }
}

size_t ZMeshDoc::loadFile(const json::value& jValue, QString& errorMsg)
{
  try {
    if (jValue.is_object()) {
      const auto& jo = jValue.as_object();
      auto typeIt = jo.find("type");
      if (typeIt == jo.end() || !typeIt->value().is_string()) {
        errorMsg = QString("Invalid mesh JSON: missing string field 'type'");
        return 0;
      }
      const QString type = json::value_to<QString>(typeIt->value()).trimmed();
      if (type != "neuroglancer_precomputed_mesh") {
        errorMsg = QString("Unsupported mesh JSON type '%1'").arg(type);
        return 0;
      }

      auto rootIt = jo.find("segmentation_root_url");
      if (rootIt == jo.end() || !rootIt->value().is_string()) {
        errorMsg = QString("Invalid neuroglancer mesh JSON: missing string field 'segmentation_root_url'");
        return 0;
      }
      const QString normalizedRootUrl =
        ZNeuroglancerPrecomputedVolume::normalizeRootUrl(json::value_to<QString>(rootIt->value()));

      auto segIt = jo.find("segment_id");
      if (segIt == jo.end() || !segIt->value().is_string()) {
        errorMsg = QString("Invalid neuroglancer mesh JSON: missing string field 'segment_id'");
        return 0;
      }
      const QString segStr = json::value_to<QString>(segIt->value()).trimmed();
      bool ok = false;
      const qulonglong segId = segStr.toULongLong(&ok, 10);
      if (!ok) {
        errorMsg = QString("Invalid neuroglancer mesh JSON: segment_id must be base-10 uint64");
        return 0;
      }

      QString lod = "coarsest";
      if (auto lodIt = jo.find("lod_policy"); lodIt != jo.end() && !lodIt->value().is_null()) {
        if (!lodIt->value().is_string()) {
          errorMsg = QString("Invalid neuroglancer mesh JSON: lod_policy must be a string");
          return 0;
        }
        lod = json::value_to<QString>(lodIt->value()).trimmed().toLower();
      }
      const auto lodPolicy = (lod == "finest") ? ZNeuroglancerPrecomputedMeshSource::LodPolicy::Finest
                                               : ZNeuroglancerPrecomputedMeshSource::LodPolicy::Coarsest;

      // Deduplicate before performing any network work.
      for (const auto& idPack : m_idToMeshPacks) {
        if (isSameObj(jValue, jsonValue(idPack.first))) {
          return idPack.first;
        }
      }

      // Try to reuse an already-opened segmentation volume.
      std::shared_ptr<ZNeuroglancerPrecomputedVolume> vol;
      for (const size_t imgId : m_doc.objsOfDoc(&m_doc.imgDoc())) {
        const ZImgPack& pack = m_doc.imgDoc().imgPack(imgId);
        if (!pack.isNeuroglancerPrecomputed()) {
          continue;
        }
        if (pack.neuroglancerRootUrl() == normalizedRootUrl && pack.neuroglancerVolumeShared()->isSegmentation()) {
          vol = pack.neuroglancerVolumeShared();
          break;
        }
      }
      if (!vol) {
        constexpr std::chrono::milliseconds timeout{30000};
        vol = ZNeuroglancerPrecomputedVolume::open(normalizedRootUrl, timeout);
      }

      std::shared_ptr<const ZNeuroglancerPrecomputedMeshSource> source;
      if (vol->hasMeshDirectory()) {
        source = vol->loadMeshSourceBlocking();
      } else if (auto meshKeyIt = jo.find("mesh_key"); meshKeyIt != jo.end() && meshKeyIt->value().is_string()) {
        constexpr std::chrono::milliseconds timeout{30000};
        QString meshKey = json::value_to<QString>(meshKeyIt->value());
        if (!meshKey.endsWith('/')) {
          meshKey += '/';
        }
        const QUrl meshDirUrl = QUrl(vol->rootUrl()).resolved(QUrl(meshKey));
        std::array<double, 3> baseRes{vol->baseImgInfo().voxelSizeX, vol->baseImgInfo().voxelSizeY, vol->baseImgInfo().voxelSizeZ};
        source = ZNeuroglancerPrecomputedMeshSource::open(meshDirUrl, baseRes, vol->baseVoxelOffset(), timeout);
      } else {
        errorMsg = QString("Neuroglancer volume does not specify a mesh directory");
        return 0;
      }

      CHECK(source);
      std::shared_ptr<ZMesh> mesh = source->loadMeshBlocking(static_cast<uint64_t>(segId), lodPolicy);
      if (!mesh || mesh->empty()) {
        errorMsg = QString("Loaded neuroglancer mesh is empty");
        return 0;
      }

      // Normalize persisted JSON to keep it stable across save/load cycles.
      json::object normalized;
      normalized["type"] = "neuroglancer_precomputed_mesh";
      normalized["segmentation_root_url"] = json::value_from(normalizedRootUrl);
      normalized["segment_id"] = json::value_from(QString::number(segId));
      normalized["lod_policy"] = json::value_from(lod);
      if (!vol->meshKey().isEmpty()) {
        normalized["mesh_key"] = json::value_from(vol->meshKey());
      }

      return addMeshFromExternalSource(*mesh,
                                      QString("NG Mesh %1").arg(segId),
                                      QString("Neuroglancer precomputed mesh\nSegmentation: %1\nSegment: %2")
                                        .arg(normalizedRootUrl)
                                        .arg(segId),
                                      normalized);
    }

    if (asQString(jValue).trimmed().isEmpty()) {
      errorMsg = QString("File path is not string or is empty");
      return 0;
    }
    for (const auto& idPack : m_idToMeshPacks) {
      if (isSameObj(jValue, jsonValue(idPack.first))) {
        return idPack.first;
      }
    }
    QString fileName = asQString(jValue);

    ZMesh mesh(fileName);
    size_t id = addMesh(mesh, fileName);
    ZSystemInfo::instance().addFileToRecentFileList(fileName);
    setLastOpenedObjPath(fileName);
    return id;
  }
  catch (const ZException& e) {
    errorMsg = e.what();
    return 0;
  }
}

std::vector<QAction*> ZMeshDoc::loadFileActions() const
{
  std::vector<QAction*> res;
  res.push_back(m_loadMeshAction);
  return res;
}

void ZMeshDoc::removeObj(size_t id)
{
  auto it = m_idToMeshPacks.find(id);
  Q_EMIT objAboutToBeRemoved(it->first, this);
  m_idToMeshPacks.erase(it);
  Q_EMIT objRemoved(id, this);
}

QString ZMeshDoc::objName(size_t id) const
{
  return m_idToMeshPacks.at(id)->name();
}

QString ZMeshDoc::objPath(size_t id) const
{
  return m_idToMeshPacks.at(id)->path;
}

bool ZMeshDoc::objHasUnsavedChange(size_t id) const
{
  return m_idToMeshPacks.at(id)->hasUnsavedChange;
}

QString ZMeshDoc::objInfo(size_t id) const
{
  return m_idToMeshPacks.at(id)->info();
}

QString ZMeshDoc::objDetailedInfo(size_t id) const
{
  return m_idToMeshPacks.at(id)->detailedInfo();
}

QString ZMeshDoc::objTooltip(size_t id) const
{
  return m_idToMeshPacks.at(id)->tooltip();
}

json::value ZMeshDoc::jsonValue(size_t id) const
{
  const auto& pack = m_idToMeshPacks.at(id);
  if (!pack->sourceJson.is_null()) {
    return pack->sourceJson;
  }
  return json::value_from(pack->path);
}

bool ZMeshDoc::isSameObj(const json::value& v1, const json::value& v2) const
{
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

  if (v1.is_object() && v2.is_object()) {
    auto keyFor = [](const json::object& o) -> std::optional<QString> {
      auto itType = o.find("type");
      if (itType == o.end() || !itType->value().is_string()) {
        return std::nullopt;
      }
      const QString type = json::value_to<QString>(itType->value()).trimmed();
      if (type != "neuroglancer_precomputed_mesh") {
        return std::nullopt;
      }

      auto itRoot = o.find("segmentation_root_url");
      auto itSeg = o.find("segment_id");
      if (itRoot == o.end() || !itRoot->value().is_string() || itSeg == o.end() || !itSeg->value().is_string()) {
        return std::nullopt;
      }

      QString root = json::value_to<QString>(itRoot->value());
      try {
        root = ZNeuroglancerPrecomputedVolume::normalizeRootUrl(std::move(root));
      }
      catch (...) {
        return std::nullopt;
      }

      const QString seg = json::value_to<QString>(itSeg->value()).trimmed();

      QString lod = "coarsest";
      if (auto itLod = o.find("lod_policy"); itLod != o.end() && itLod->value().is_string()) {
        lod = json::value_to<QString>(itLod->value()).trimmed().toLower();
      }

      return QString("%1|%2|%3|%4").arg(type, root, seg, lod);
    };

    const auto k1 = keyFor(v1.as_object());
    const auto k2 = keyFor(v2.as_object());
    return k1 && k2 && *k1 == *k2;
  }

  return false;
}

size_t ZMeshDoc::makeAlias(size_t id)
{
  CHECK(m_idToMeshPacks.contains(id));

  size_t aliasId = m_doc.getNewObjId();
  m_idToMeshPacks[aliasId] = m_idToMeshPacks[id];
  m_doc.registerNewObj(aliasId, *this);

  Q_EMIT objAdded(aliasId, this);
  return aliasId;
}

bool ZMeshDoc::isAlias(size_t id) const
{
  CHECK(m_idToMeshPacks.contains(id));

  return std::ranges::any_of(m_idToMeshPacks, [&, this](const auto& idPack) {
    return idPack.first != id && idPack.second == m_idToMeshPacks.at(id);
  });
}

void ZMeshDoc::loadMesh()
{
  QFileDialog dialog(QApplication::activeWindow());
  dialog.setFileMode(QFileDialog::ExistingFiles);
  dialog.setNameFilter(ZMesh::getQtReadNameFilter());
  dialog.setDirectory(lastOpenedObjPath());
  dialog.setWindowTitle("Load Mesh File");
  if (dialog.exec()) {
    QString errorMsg;
    // auto fmtIdx = filters.indexOf(dialog.selectedNameFilter());
    for (index_t i = 0; i < dialog.selectedFiles().size(); ++i) {
      const QString filePath = dialog.selectedFiles().at(i);
      if (!loadFile(filePath, errorMsg)) {
        showCriticalWithDetails(QApplication::activeWindow(), tr("Can not load mesh %1").arg(filePath), errorMsg);
      }
    }
  }
}

size_t ZMeshDoc::addMesh(ZMesh& mesh, const QString& path)
{
  size_t id = m_doc.getNewObjId();
  m_idToMeshPacks[id] = std::make_shared<MeshPack>(mesh, path);
  m_doc.registerNewObj(id, *this);

  Q_EMIT objAdded(id, this);
  return id;
}

ZMeshDoc::MeshPack::MeshPack(ZMesh& imesh, const QString& path_)
  : path(QFileInfo(path_).canonicalFilePath())
{
  mesh.swap(imesh);
  meshList.push_back(&mesh);
  updateDerivedData();
}

ZMeshDoc::MeshPack::MeshPack(ZMesh& imesh, QString displayName, QString tooltip, json::value sourceJson_)
  : sourceJson(std::move(sourceJson_))
  , displayNameOverride(std::move(displayName))
  , tooltipOverride(std::move(tooltip))
{
  mesh.swap(imesh);
  meshList.push_back(&mesh);
  updateDerivedData();
}

ZMeshDoc::MeshPack::~MeshPack()
{
  meshList.clear();
}

void ZMeshDoc::MeshPack::updateDerivedData()
{
  m_info.clear();
  m_detailedInfo.clear();

  if (!displayNameOverride.isEmpty()) {
    m_name = displayNameOverride;
  } else if (!path.isEmpty()) {
    m_name = QFileInfo(path).fileName();
  } else {
    m_name = QStringLiteral("Mesh");
  }

  if (!tooltipOverride.isEmpty()) {
    m_tooltip = tooltipOverride;
  } else if (!path.isEmpty()) {
    m_tooltip = path;
  } else {
    m_tooltip.clear();
  }
}

const QString& ZMeshDoc::MeshPack::info() const
{
  if (m_info.isEmpty()) {
    m_info = QString("%1 vertices, %2 triangles").arg(mesh.numVertices()).arg(mesh.numTriangles());
  }
  return m_info;
}

const QString& ZMeshDoc::MeshPack::detailedInfo() const
{
  if (m_detailedInfo.isEmpty()) {
    QStringList info;
    ZMeshProperties prop = mesh.properties();
    info << QString("Number of Vertices: %1").arg(prop.numVertices);
    info << QString("Number of Triangles: %1").arg(prop.numTriangles);
    info << QString("Surface Area: %1").arg(prop.surfaceArea);
    info << QString("Min Triangle Area: %1").arg(prop.minTriangleArea);
    info << QString("Max Triangle Area: %1").arg(prop.maxTriangleArea);
    info << QString("Volume: %1").arg(prop.volume);
    info << QString("Volume Projected: %1").arg(prop.volumeProjected);
    info << QString("Volume X: %1").arg(prop.volumeX);
    info << QString("Volume Y: %1").arg(prop.volumeY);
    info << QString("Volume Z: %1").arg(prop.volumeZ);
    info << QString("Kx: %1").arg(prop.kx);
    info << QString("Ky: %1").arg(prop.ky);
    info << QString("Kz: %1").arg(prop.kz);
    info << QString("Normalized Shape Index: %1").arg(prop.normalizedShapeIndex);
    m_detailedInfo = info.join("\n");
  }
  return m_detailedInfo;
}

void ZMeshDoc::createActions()
{
  m_loadMeshAction = new QAction(ZTheme::instance().icon(ZTheme::LoadObjectIcon), tr("&Load Mesh..."), this);
  m_loadMeshAction->setStatusTip(tr("Load an existing mesh file"));
  connect(m_loadMeshAction, &QAction::triggered, this, &ZMeshDoc::loadMesh);
}

bool ZMeshDoc::saveMesh(MeshPack* pack, const QString& fileName, QString& errorMsg, const std::string& format)
{
  try {
    pack->mesh.save(fileName, format);
    pack->path = QFileInfo(fileName).canonicalFilePath();
    pack->sourceJson = {};
    pack->displayNameOverride.clear();
    pack->tooltipOverride.clear();
    pack->hasUnsavedChange = false;
    pack->updateDerivedData();

    ZSystemInfo::instance().addFileToRecentFileList(fileName);
    setLastOpenedObjPath(fileName);
    return true;
  }
  catch (const ZException& e) {
    errorMsg = e.what();
    return false;
  }
}

void ZMeshDoc::packInfoUpdated(MeshPack* pack)
{
  for (const auto& idPack : m_idToMeshPacks) {
    if (idPack.second.get() == pack) {
      m_doc.updateObjInfo(idPack.first);
    }
  }
}

} // namespace nim
