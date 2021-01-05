#pragma once

#include "zobjdoc.h"
#include "zswcpack.h"

namespace nim {

class ZSwcDoc : public ZObjDoc
{
Q_OBJECT
public:
  explicit ZSwcDoc(ZDoc& doc);

  // return info of swc with id, assume swc exist, otherwise crash
  ZSwcPack& swcPack(size_t id)
  { return *m_idToSwcPacks.at(id); }

  // ZObjDoc interface
public:
  bool save(size_t id) override;

  bool saveAs(size_t id) override;

  [[nodiscard]] QString typeName() const override
  { return "Swc"; }

  [[nodiscard]] QString typePluralName() const override
  { return "Swcs"; }

  [[nodiscard]] bool canReadFile(const QString& fileName) const override;

  size_t loadFile(const QString& fileName, QString& errorMsg) override;

  size_t loadFile(const json::value& jValue, QString& errorMsg) override;

  [[nodiscard]] std::vector<QAction*> loadFileActions() const override;

  void removeObj(size_t id) override;

  [[nodiscard]] QString objName(size_t id) const override;

  [[nodiscard]] QString objPath(size_t id) const override;

  [[nodiscard]] bool objHasUnsavedChange(size_t id) const override;

  [[nodiscard]] QString objInfo(size_t id) const override;

  [[nodiscard]] QString objTooltip(size_t id) const override;

  [[nodiscard]] const QUndoStack* objUndoStack(size_t id) const override;

  [[nodiscard]] json::value jsonValue(size_t id) const override;

  [[nodiscard]] bool isSameObj(const json::value& v1, const json::value& v2) const override;

  size_t makeAlias(size_t id) override;

  [[nodiscard]] bool isAlias(size_t id) const override;

  QWidget* createObjEditWidget(size_t id) override;

protected:
  void loadSwc();

  // append another swc into this doc
  size_t addSwc(ZSwc& tree, const QString& path);

  void setModified(bool clean);

private:
  void createActions();

  bool saveSwc(ZSwcPack* pack, const QString& fileName, QString& errorMsg);

  // notify obj manager about the update
  void packInfoUpdated(ZSwcPack* pack);

private:
  std::map<size_t, std::shared_ptr<ZSwcPack>> m_idToSwcPacks;

  QAction* m_loadSwcAction = nullptr;
};

} // namespace nim

