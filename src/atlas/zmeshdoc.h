#ifndef ZMESHDOC_H
#define ZMESHDOC_H

#include "zobjdoc.h"
#include "zmesh.h"

namespace nim {

class ZMeshDoc : public ZObjDoc
{
  Q_OBJECT
public:
  explicit ZMeshDoc(ZDoc &doc);

  // return info of mesh with id, assume mesh exist, otherwise crash
  QList<ZMesh*>* meshList(size_t id) { return &(m_idToMeshPacks.at(id)->meshList); }

  void askToSave(const ZMesh &msh, const QString &title = "");

  // ZObjDoc interface
public:
  virtual bool save(size_t id) override;
  virtual bool saveAs(size_t id) override;
  virtual QString typeName() const override { return "Mesh"; }
  virtual QString typePluralName() const override { return "Mesh"; }
  virtual bool canReadFile(const QString &fileName) override;
  virtual size_t loadFile(const QString &fileName, QString &errorMsg) override;
  virtual size_t loadFile(const QJsonValue &jValue, QString &errorMsg) override;
  virtual QList<QAction*> loadFileActions() const override;
  virtual void removeObj(size_t id) override;
  virtual QString objName(size_t id) const override;
  virtual QString objPath(size_t id) const override;
  virtual bool objHasUnsavedChange(size_t id) const override;
  virtual QString objInfo(size_t id) const override;
  virtual QString objDetailedInfo(size_t id) const override;
  virtual QString objTooltip(size_t id) const override;
  virtual QJsonValue jsonValue(size_t id) const override;
  virtual bool isSameObj(const QJsonValue& v1, const QJsonValue& v2) const override;
  virtual size_t makeAlias(size_t id) override;
  virtual bool isAlias(size_t id) const override;

protected:
  void loadMesh();

  // append another mesh into this doc
  size_t addMesh(ZMesh &mesh, const QString &path);

private:
  struct MeshPack { // mesh and its associated data
    MeshPack(ZMesh &mesh, const QString &path);
    ~MeshPack();

    void updateDerivedData();
    const QString& info() const;
    const QString& detailedInfo() const;
    inline const QString& name() const { return m_name; }
    inline const QString& tooltip() const { return m_tooltip; }

    ZMesh mesh;
    QList<ZMesh*> meshList;
    QString path;
    bool hasUnsavedChange;

    // derived data
  private:
    mutable QString m_info;
    mutable QString m_detailedInfo;
    QString m_name;
    QString m_tooltip;
  };

  void createActions();
  bool saveMesh(MeshPack *pack, const QString &fileName, QString &errorMsg, const std::string& format = "");
  // notify obj manager about the update
  void packInfoUpdated(MeshPack* pack);

private:
  std::map<size_t, std::shared_ptr<MeshPack>> m_idToMeshPacks;

  QAction *m_loadMeshAction;
};

} // namespace nim

#endif // ZMESHDOC_H
