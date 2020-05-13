#pragma once

#include "zobjdoc.h"
#include "zpunctapack.h"

namespace nim {

class ZPunctaDoc : public ZObjDoc
{
Q_OBJECT
public:
  explicit ZPunctaDoc(ZDoc& doc);

  // return info of puncta with id, assume puncta exist, otherwise crash
  ZPunctaPack& punctaPack(size_t id)
  { return *m_idToPunctaPacks.at(id); }

  // ZObjDoc interface
public:
  bool save(size_t id) override;

  bool saveAs(size_t id) override;

  QString typeName() const override
  { return "Puncta"; }

  QString typePluralName() const override
  { return "Puncta"; }

  bool canReadFile(const QString& fileName) override;

  size_t loadFile(const QString& fileName, QString& errorMsg) override;

  size_t loadFile(const QJsonValue& jValue, QString& errorMsg) override;

  QList<QAction*> loadFileActions() const override;

  QMenu* processObjMenu() const override;

  void removeObj(size_t id) override;

  QString objName(size_t id) const override;

  QString objPath(size_t id) const override;

  bool objHasUnsavedChange(size_t id) const override;

  QString objInfo(size_t id) const override;

  QString objTooltip(size_t id) const override;

  const QUndoStack* objUndoStack(size_t id) const override;

  QJsonValue jsonValue(size_t id) const override;

  bool isSameObj(const QJsonValue& v1, const QJsonValue& v2) const override;

  size_t makeAlias(size_t id) override;

  bool isAlias(size_t id) const override;

  QWidget* createObjEditWidget(size_t id) override;

protected:
  void loadPuncta();

  void detectPuncta();

  void generateAnalysisTextFiles();

  // append another puncta into this doc
  size_t addPuncta(ZPuncta puncta, const QString& path);

  void setModified(bool clean);

private:
  void createActions();

  bool savePuncta(ZPunctaPack* pack, const QString& fileName, QString& errorMsg, const QString& format = "");

  // notify obj manager about the update
  void packInfoUpdated(ZPunctaPack* pack);

private:
  std::map<size_t, std::shared_ptr<ZPunctaPack>> m_idToPunctaPacks;

  QAction* m_loadPunctaAction;

  QAction* m_detectPunctaAction;
  QAction* m_generateAnalysisTextFilesAction;
};

} // namespace nim

