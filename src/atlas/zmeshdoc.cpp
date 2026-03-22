#include "zmeshdoc.h"

#include "zexception.h"
#include "zimgdoc.h"
#include "zlog.h"
#include "zneuroglancerexternalsource.h"
#include "zneuroglancerprecomputed.h"
#include "zneuroglancerprecomputedmesh.h"
#include "zneuroglancerremotecontext.h"
#include "ztheme.h"
#include "zmessageboxhelpers.h"
#include <QFileDialog>
#include <QSettings>
#include <QApplication>
#include <set>

namespace nim {

namespace {

struct ResolvedNeuroglancerMeshSource
{
  QString normalizedRootUrl;
  QString meshSourceDirUrlForJson;
  uint64_t segmentId = 0;
  std::array<double, 3> baseResolutionNm{};
  std::array<int64_t, 3> baseVoxelOffset{};
  std::shared_ptr<const ZNeuroglancerRemoteContext> remoteContext;
  std::shared_ptr<const ZNeuroglancerPrecomputedMeshSource> source;
};

[[nodiscard]] ResolvedNeuroglancerMeshSource
resolveNeuroglancerMeshSource(ZDoc& doc,
                              const ZNeuroglancerMeshExternalSourceKey& key,
                              std::shared_ptr<const ZNeuroglancerRemoteContext> preferredRemoteContext = nullptr)
{
  ResolvedNeuroglancerMeshSource out;
  out.normalizedRootUrl = key.rootUrl;
  out.segmentId = key.segmentId;
  out.remoteContext = std::move(preferredRemoteContext);

  std::shared_ptr<ZNeuroglancerPrecomputedVolume> vol;
  for (const size_t imgId : doc.objsOfDoc(&doc.imgDoc())) {
    const ZImgPack& pack = doc.imgDoc().imgPack(imgId);
    if (!pack.isNeuroglancerPrecomputed()) {
      continue;
    }
    if (pack.neuroglancerRootUrl() == out.normalizedRootUrl && pack.neuroglancerVolumeShared()->isSegmentation()) {
      vol = pack.neuroglancerVolumeShared();
      break;
    }
  }
  if (!vol) {
    const std::chrono::milliseconds timeout =
      out.remoteContext ? out.remoteContext->timeout() : std::chrono::milliseconds{30000};
    const auto objectStore = out.remoteContext ? out.remoteContext->sharedObjectStore() : nullptr;
    vol = ZNeuroglancerPrecomputedVolume::open(out.normalizedRootUrl, timeout, objectStore);
  }
  CHECK(vol);
  if (!out.remoteContext) {
    out.remoteContext = vol->sharedRemoteContext();
  }
  CHECK(out.remoteContext);

  if (!key.meshSourceDirUrl.isEmpty()) {
    out.meshSourceDirUrlForJson = key.meshSourceDirUrl;
  } else {
    if (!vol->hasMeshDirectory()) {
      throw ZException("Neuroglancer volume does not specify a mesh directory");
    }
    out.meshSourceDirUrlForJson = vol->meshDirUrl().toString(QUrl::StripTrailingSlash) + "/";
  }

  out.baseResolutionNm = key.baseResolutionNm.value_or(
    std::array<double, 3>{vol->baseImgInfo().voxelSizeX, vol->baseImgInfo().voxelSizeY, vol->baseImgInfo().voxelSizeZ});
  out.baseVoxelOffset = key.baseVoxelOffset.value_or(vol->baseVoxelOffset());

  out.source = ZNeuroglancerPrecomputedMeshSource::open(QUrl(out.meshSourceDirUrlForJson),
                                                        out.baseResolutionNm,
                                                        out.baseVoxelOffset,
                                                        out.remoteContext);
  CHECK(out.source);
  return out;
}

} // namespace

ZMeshDoc::ZMeshDoc(ZDoc& doc)
  : ZObjDoc(doc)
{
  createActions();
}

size_t ZMeshDoc::addMeshFromExternalSource(ZMesh& mesh,
                                           QString displayName,
                                           QString tooltip,
                                           json::value sourceJson,
                                           std::shared_ptr<const ZNeuroglancerRemoteContext> remoteContext)
{
  for (const auto& idPack : m_idToMeshPacks) {
    if (isSameObj(sourceJson, jsonValue(idPack.first))) {
      if (remoteContext && idPack.second->runtimeRemoteContext != remoteContext) {
        idPack.second->runtimeRemoteContext = std::move(remoteContext);
        packGeometryUpdated(idPack.second.get());
      }
      return idPack.first;
    }
  }

  size_t id = m_doc.getNewObjId();
  m_idToMeshPacks[id] = std::make_shared<MeshPack>(mesh,
                                                   std::move(displayName),
                                                   std::move(tooltip),
                                                   std::move(sourceJson),
                                                   std::move(remoteContext));
  m_doc.registerNewObj(id, *this);

  Q_EMIT objAdded(id, this);
  return id;
}

std::optional<size_t> ZMeshDoc::findMeshByExternalSource(const json::value& sourceJson) const
{
  for (const auto& idPack : m_idToMeshPacks) {
    if (isSameObj(sourceJson, jsonValue(idPack.first))) {
      return idPack.first;
    }
  }
  return std::nullopt;
}

std::shared_ptr<const ZNeuroglancerRemoteContext> ZMeshDoc::externalRemoteContext(size_t id) const
{
  CHECK(m_idToMeshPacks.contains(id));
  return m_idToMeshPacks.at(id)->runtimeRemoteContext;
}

void ZMeshDoc::replaceMeshGeometry(size_t id, ZMesh& mesh)
{
  CHECK(m_idToMeshPacks.contains(id));
  auto& pack = m_idToMeshPacks.at(id);
  pack->mesh.swap(mesh);
  pack->updateDerivedData();
  packInfoUpdated(pack.get());
  packGeometryUpdated(pack.get());
}

void ZMeshDoc::updateExternalMeshMetadata(size_t id, QString displayName, QString tooltip)
{
  CHECK(m_idToMeshPacks.contains(id));
  auto& pack = m_idToMeshPacks.at(id);
  CHECK(!pack->sourceJson.is_null()) << "updateExternalMeshMetadata is only valid for external-source meshes";

  pack->displayNameOverride = std::move(displayName);
  pack->tooltipOverride = std::move(tooltip);
  pack->updateDerivedData();
  packInfoUpdated(pack.get());
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
    size_t id = addMesh(std::move(mesh), fileName);
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

      const auto keyOpt = parseNeuroglancerMeshExternalSourceKey(jValue);
      if (!keyOpt) {
        errorMsg = QString("Invalid neuroglancer mesh JSON");
        return 0;
      }
      const auto& key = *keyOpt;
      const uint64_t segId = key.segmentId;

      // Deduplicate before performing any network work.
      for (const auto& idPack : m_idToMeshPacks) {
        if (isSameObj(jValue, jsonValue(idPack.first))) {
          return idPack.first;
        }
      }

      const ResolvedNeuroglancerMeshSource resolved = resolveNeuroglancerMeshSource(m_doc, key);

      // Normalize persisted JSON to keep it stable across save/load cycles.
      const json::value normalized = makeNeuroglancerMeshExternalSourceJson(resolved.normalizedRootUrl,
                                                                            resolved.meshSourceDirUrlForJson,
                                                                            segId,
                                                                            resolved.baseResolutionNm,
                                                                            resolved.baseVoxelOffset);

      if (resolved.source->supportsRuntimeLod()) {
        // Multi-LOD datasets can stream mesh chunks at runtime. Avoid blocking
        // scene load on coarse mesh materialization; instead register an
        // external-source placeholder and let Z3DMeshFilter drive progressive
        // refinement once the 3D view is up.
        if (VLOG_IS_ON(1)) {
          VLOG(1) << fmt::format("Scene load: deferring Neuroglancer mesh materialization: segment={} mesh='{}'",
                                 segId,
                                 resolved.meshSourceDirUrlForJson);
        }
        ZMesh placeholderMesh;
        return addMeshFromExternalSource(
          placeholderMesh,
          QString("NG Mesh %1").arg(segId),
          QString("Neuroglancer precomputed mesh (deferred)\nSegmentation: %1\nSegment: %2")
            .arg(resolved.normalizedRootUrl)
            .arg(segId),
          normalized,
          resolved.remoteContext);
      }

      // Legacy (non-multi-LOD) precomputed meshes have no runtime chunking path,
      // so we must materialize geometry at load time.
      std::shared_ptr<ZMesh> mesh =
        resolved.source->loadMeshBlocking(segId, ZNeuroglancerPrecomputedMeshSource::LodPolicy::Finest);
      if (!mesh || mesh->empty()) {
        errorMsg = QString("Loaded neuroglancer mesh is empty");
        return 0;
      }

      return addMeshFromExternalSource(*mesh,
                                       QString("NG Mesh %1").arg(segId),
                                       QString("Neuroglancer precomputed mesh\nSegmentation: %1\nSegment: %2")
                                         .arg(resolved.normalizedRootUrl)
                                         .arg(segId),
                                       normalized,
                                       resolved.remoteContext);
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
    size_t id = addMesh(std::move(mesh), fileName);
    ZSystemInfo::instance().addFileToRecentFileList(fileName);
    setLastOpenedObjPath(fileName);
    return id;
  }
  catch (const ZException& e) {
    errorMsg = e.what();
    return 0;
  }
}

bool ZMeshDoc::canPrepareLoadAsync(const json::value& jValue) const
{
  // Only async-prepare local file loads. External-source meshes (e.g. Neuroglancer)
  // may involve network IO and cross-doc coordination (image doc reuse), so keep them synchronous.
  if (!jValue.is_string()) {
    return false;
  }
  return !asQString(jValue).trimmed().isEmpty();
}

folly::coro::Task<ZObjDoc::PreparedLoadResult> ZMeshDoc::prepareLoadAsync(const json::value& jValue,
                                                                          const ZObjDoc::AsyncLoadContext&) const
{
  PreparedLoadResult out;
  const QString fileName = asQString(jValue);
  if (fileName.trimmed().isEmpty()) {
    out.errorMsg = QString("File path is not string or is empty");
    co_return out;
  }

  try {
    ZMesh mesh(fileName);
    ZMeshDoc* self = const_cast<ZMeshDoc*>(this);
    out.commit = [self, this, fileName, mesh = std::move(mesh)](QString& errorMsg) mutable -> size_t {
      try {
        const size_t id = self->addMesh(std::move(mesh), fileName);
        ZSystemInfo::instance().addFileToRecentFileList(fileName);
        setLastOpenedObjPath(fileName);
        return id;
      }
      catch (const ZException& e) {
        errorMsg = e.what();
        return 0;
      }
      catch (const std::exception& e) {
        errorMsg = e.what();
        return 0;
      }
    };
  }
  catch (const ZException& e) {
    out.errorMsg = e.what();
  }
  catch (const std::exception& e) {
    out.errorMsg = e.what();
  }

  co_return out;
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
    const auto k1 = parseNeuroglancerMeshExternalSourceKey(v1);
    const auto k2 = parseNeuroglancerMeshExternalSourceKey(v2);
    return k1 && k2 && sameNeuroglancerMeshSourceCompat(*k1, *k2);
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

size_t ZMeshDoc::addMesh(ZMesh mesh, const QString& path)
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

ZMeshDoc::MeshPack::MeshPack(ZMesh& imesh,
                             QString displayName,
                             QString tooltip,
                             json::value sourceJson_,
                             std::shared_ptr<const ZNeuroglancerRemoteContext> remoteContext)
  : sourceJson(std::move(sourceJson_))
  , runtimeRemoteContext(std::move(remoteContext))
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
    if (!sourceJson.is_null() && mesh.empty()) {
      // External-source meshes (e.g. runtime Neuroglancer LOD) may be registered
      // as placeholders during scene load. Avoid showing misleading "0 vertices"
      // in the object manager when the geometry has simply been deferred.
      m_info = QStringLiteral("Deferred external mesh (runtime LOD)");
    } else {
      m_info = QString("%1 vertices, %2 triangles").arg(mesh.numVertices()).arg(mesh.numTriangles());
    }
  }
  return m_info;
}

const QString& ZMeshDoc::MeshPack::detailedInfo() const
{
  if (m_detailedInfo.isEmpty()) {
    if (!sourceJson.is_null() && mesh.empty()) {
      m_detailedInfo = QStringLiteral("Deferred external mesh (runtime LOD)\n"
                                      "Geometry is streamed when a 3D view is active.");
      return m_detailedInfo;
    }

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
    std::optional<ZMesh> materializedMesh;
    if (!pack->sourceJson.is_null()) {
      if (const auto keyOpt = parseNeuroglancerMeshExternalSourceKey(pack->sourceJson)) {
        const ResolvedNeuroglancerMeshSource resolved =
          resolveNeuroglancerMeshSource(m_doc, *keyOpt, pack->runtimeRemoteContext);
        std::shared_ptr<ZMesh> fullMesh =
          resolved.source->loadMeshBlocking(resolved.segmentId, ZNeuroglancerPrecomputedMeshSource::LodPolicy::Finest);
        if (!fullMesh || fullMesh->empty()) {
          throw ZException("Loaded neuroglancer mesh is empty");
        }
        materializedMesh.emplace();
        materializedMesh->swap(*fullMesh);
      }
    }

    if (materializedMesh) {
      materializedMesh->save(fileName, format);
      pack->mesh.swap(*materializedMesh);
    } else {
      pack->mesh.save(fileName, format);
    }

    pack->path = QFileInfo(fileName).canonicalFilePath();
    pack->sourceJson = {};
    pack->runtimeRemoteContext.reset();
    pack->displayNameOverride.clear();
    pack->tooltipOverride.clear();
    pack->hasUnsavedChange = false;
    pack->updateDerivedData();

    ZSystemInfo::instance().addFileToRecentFileList(fileName);
    setLastOpenedObjPath(fileName);
    packInfoUpdated(pack);
    packGeometryUpdated(pack);
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

void ZMeshDoc::packGeometryUpdated(MeshPack* pack)
{
  for (const auto& idPack : m_idToMeshPacks) {
    if (idPack.second.get() == pack) {
      Q_EMIT meshChanged(idPack.first);
    }
  }
}

} // namespace nim
