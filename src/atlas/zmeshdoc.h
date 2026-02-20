#pragma once

#include "zmesh.h"
#include "zobjdoc.h"

#include <optional>

namespace nim {

class ZMeshDoc : public ZObjDoc
{
  Q_OBJECT

public:
  explicit ZMeshDoc(ZDoc& doc);

  // return info of mesh with id, assume mesh exist, otherwise crash
  std::vector<ZMesh*>* meshList(size_t id)
  {
    return &(m_idToMeshPacks.at(id)->meshList);
  }

  void askToSave(const ZMesh& msh, const QString& title = "");

  // Add a mesh that does not come from a local file path (e.g. network-backed Neuroglancer mesh).
  // The `sourceJson` is stored in the scene file so the mesh can be reloaded on restore.
  size_t addMeshFromExternalSource(ZMesh& mesh, QString displayName, QString tooltip, json::value sourceJson);

  [[nodiscard]] std::optional<size_t> findMeshByExternalSource(const json::value& sourceJson) const;

  // Replace the geometry of an existing mesh object and notify 3D views.
  // Intended for progressive refinement (e.g. Neuroglancer mesh LOD refinement).
  void replaceMeshGeometry(size_t id, ZMesh& mesh);

  // Update the display name / tooltip of an external-source mesh (e.g. Neuroglancer),
  // without modifying its geometry. This is useful when optional metadata (like
  // segment_properties) becomes available after the mesh has already been loaded.
  void updateExternalMeshMetadata(size_t id, QString displayName, QString tooltip);

Q_SIGNALS:
  void meshChanged(size_t id);

  // ZObjDoc interface

public:
  bool save(size_t id) override;

  bool saveAs(size_t id) override;

  [[nodiscard]] QString typeName() const override
  {
    return "Mesh";
  }

  [[nodiscard]] QString typePluralName() const override
  {
    return "Mesh";
  }

  [[nodiscard]] bool canReadFile(const QString& fileName) const override;

  size_t loadFile(const QString& fileName, QString& errorMsg) override;

  size_t loadFile(const json::value& jValue, QString& errorMsg) override;

  [[nodiscard]] std::vector<QAction*> loadFileActions() const override;

  [[nodiscard]] QString objName(size_t id) const override;

  [[nodiscard]] QString objPath(size_t id) const override;

  [[nodiscard]] bool objHasUnsavedChange(size_t id) const override;

  [[nodiscard]] QString objInfo(size_t id) const override;

  [[nodiscard]] QString objDetailedInfo(size_t id) const override;

  [[nodiscard]] QString objTooltip(size_t id) const override;

  [[nodiscard]] json::value jsonValue(size_t id) const override;

  [[nodiscard]] bool isSameObj(const json::value& v1, const json::value& v2) const override;

  size_t makeAlias(size_t id) override;

  [[nodiscard]] bool isAlias(size_t id) const override;

protected:
  void removeObj(size_t id) override;

  void loadMesh();

  // append another mesh into this doc
  size_t addMesh(ZMesh mesh, const QString& path);

  [[nodiscard]] bool canPrepareLoadAsync(const json::value& jValue) const override;

  [[nodiscard]] folly::coro::Task<ZObjDoc::PreparedLoadResult>
  prepareLoadAsync(const json::value& jValue, const ZObjDoc::AsyncLoadContext& ctx) const override;

private:
  struct MeshPack
  { // mesh and its associated data
    MeshPack(ZMesh& imesh, const QString& path_);
    MeshPack(ZMesh& imesh, QString displayName, QString tooltip, json::value sourceJson);

    ~MeshPack();

    void updateDerivedData();

    const QString& info() const;

    const QString& detailedInfo() const;

    const QString& name() const
    {
      return m_name;
    }

    const QString& tooltip() const
    {
      return m_tooltip;
    }

    ZMesh mesh;
    std::vector<ZMesh*> meshList;
    QString path;
    json::value sourceJson;
    QString displayNameOverride;
    QString tooltipOverride;
    bool hasUnsavedChange = false;

    // derived data

  private:
    mutable QString m_info;
    mutable QString m_detailedInfo;
    QString m_name;
    QString m_tooltip;
  };

  void createActions();

  bool saveMesh(MeshPack* pack, const QString& fileName, QString& errorMsg, const std::string& format = "");

  // notify obj manager about the update
  void packInfoUpdated(MeshPack* pack);

private:
  std::map<size_t, std::shared_ptr<MeshPack>> m_idToMeshPacks;

  QAction* m_loadMeshAction = nullptr;
};

} // namespace nim
