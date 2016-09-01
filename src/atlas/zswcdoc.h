#pragma once

#include "zobjdoc.h"
#include "zswc.h"

namespace nim {

class ZSwcDoc : public ZObjDoc
{
Q_OBJECT
public:
  explicit ZSwcDoc(ZDoc& doc);

  // return info of swc with id, assume swc exist, otherwise crash
  ZSwc& swc(size_t id)
  { return m_idToSwcPacks.at(id)->swc; }

  // ZObjDoc interface
public:
  virtual bool save(size_t id) override;

  virtual bool saveAs(size_t id) override;

  virtual QString typeName() const override
  { return "Swc"; }

  virtual QString typePluralName() const override
  { return "Swcs"; }

  virtual bool canReadFile(const QString& fileName) override;

  virtual size_t loadFile(const QString& fileName, QString& errorMsg) override;

  virtual size_t loadFile(const QJsonValue& jValue, QString& errorMsg) override;

  virtual QList<QAction*> loadFileActions() const override;

  virtual void removeObj(size_t id) override;

  virtual QString objName(size_t id) const override;

  virtual QString objPath(size_t id) const override;

  virtual bool objHasUnsavedChange(size_t id) const override;

  virtual QString objInfo(size_t id) const override;

  virtual QString objTooltip(size_t id) const override;

  virtual QJsonValue jsonValue(size_t id) const override;

  virtual bool isSameObj(const QJsonValue& v1, const QJsonValue& v2) const override;

  virtual size_t makeAlias(size_t id) override;

  virtual bool isAlias(size_t id) const override;

protected:
  void loadSwc();

  // append another swc into this doc
  size_t addSwc(ZSwc& tree, const QString& path);

private:
  struct SwcPack
  { // swc and its associated data
    SwcPack(ZSwc& tree, const QString& path_);

    ~SwcPack();

    void updateDerivedData();

    const QString& info() const;

    inline const QString& name() const
    { return m_name; }

    inline const QString& tooltip() const
    { return m_tooltip; }

    ZSwc swc;
    QString path;
    bool hasUnsavedChange;

    // derived data
  private:
    mutable QString m_info;
    QString m_name;
    QString m_tooltip;
  };

  void createActions();

  bool saveSwc(SwcPack* pack, const QString& fileName, QString& errorMsg);

  // notify obj manager about the update
  void packInfoUpdated(SwcPack* pack);

private:
  std::map<size_t, std::shared_ptr<SwcPack>> m_idToSwcPacks;

  QAction* m_loadSwcAction;
};

} // namespace nim

