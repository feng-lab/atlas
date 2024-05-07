#pragma once

#include "zmesh.h"
#include "zobjdoc.h"

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

  void removeObj(size_t id) override;

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
  void loadMesh();

  // append another mesh into this doc
  size_t addMesh(ZMesh& mesh, const QString& path);

private:
  struct MeshPack
  { // mesh and its associated data
    MeshPack(ZMesh& imesh, const QString& path_);

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
