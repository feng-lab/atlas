#pragma once

#include "zobjdoc.h"
#include "zpuncta.h"

namespace nim {

class ZPunctaDoc : public ZObjDoc
{
Q_OBJECT
public:
  explicit ZPunctaDoc(ZDoc& doc);

  // return info of puncta with id, assume puncta exist, otherwise crash
  ZPuncta& puncta(size_t id)
  { return m_idToPunctaPacks.at(id)->puncta; }

  // ZObjDoc interface
public:
  virtual bool save(size_t id) override;

  virtual bool saveAs(size_t id) override;

  virtual QString typeName() const override
  { return "Puncta"; }

  virtual QString typePluralName() const override
  { return "Puncta"; }

  virtual bool canReadFile(const QString& fileName) override;

  virtual size_t loadFile(const QString& fileName, QString& errorMsg) override;

  virtual size_t loadFile(const QJsonValue& jValue, QString& errorMsg) override;

  virtual QList<QAction*> loadFileActions() const override;

  virtual QMenu* processObjMenu() const override;

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
  void loadPuncta();

  void detectPuncta();

  void generateAnalysisTextFiles();

  // append another puncta into this doc
  size_t addPuncta(ZPuncta& puncta, const QString& path);

private:
  struct PunctaPack
  { // puncta and its associated data
    PunctaPack(ZPuncta& puncta, const QString& path);

    ~PunctaPack();

    void updateDerivedData();

    const QString& info() const;

    inline const QString& name() const
    { return m_name; }

    inline const QString& tooltip() const
    { return m_tooltip; }

    ZPuncta puncta;
    QString path;
    bool hasUnsavedChange;

    // derived data
  private:
    mutable QString m_info;
    QString m_name;
    QString m_tooltip;
  };

  void createActions();

  bool savePuncta(PunctaPack* pack, const QString& fileName, QString& errorMsg, const QString& format = "");

  // notify obj manager about the update
  void packInfoUpdated(PunctaPack* pack);

private:
  std::map<size_t, std::shared_ptr<PunctaPack>> m_idToPunctaPacks;

  QAction* m_loadPunctaAction;

  QAction* m_detectPunctaAction;
  QAction* m_generateAnalysisTextFilesAction;
};

} // namespace nim

