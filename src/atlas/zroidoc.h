#pragma once

#include "zobjdoc.h"
#include "zroipack.h"

namespace nim {

class ZROIDoc : public ZObjDoc
{
Q_OBJECT
public:
  explicit ZROIDoc(ZDoc& doc);

  inline ZROIPack& roiPack(size_t id)
  { return *m_idToROIPacks.at(id); }

  [[nodiscard]] inline const ZROIPack& roiPack(size_t id) const
  { return *m_idToROIPacks.at(id); }

  ZROIPack& currentROIPack();

  void askToSave(const ZROI& roi, const QString& title = "");

  // ZObjDoc interface
public:
  bool save(size_t id) override;

  bool saveAs(size_t id) override;

  [[nodiscard]] QString typeName() const override
  { return "ROI"; }

  [[nodiscard]] QString typePluralName() const override
  { return "ROI"; }

  [[nodiscard]] bool canReadFile(const QString& fileName) const override;

  size_t loadFile(const QString& fileName, QString& errorMsg) override;

  size_t loadFile(const QJsonValue& jValue, QString& errorMsg) override;

  [[nodiscard]] std::vector<QAction*> loadFileActions() const override;

  [[nodiscard]] QMenu* processObjMenu() const override;

  void removeObj(size_t id) override;

  [[nodiscard]] QString objName(size_t id) const override;

  [[nodiscard]] QString objPath(size_t id) const override;

  [[nodiscard]] bool objHasUnsavedChange(size_t id) const override;

  [[nodiscard]] QString objInfo(size_t id) const override;

  [[nodiscard]] QString objTooltip(size_t id) const override;

  [[nodiscard]] const QUndoStack* objUndoStack(size_t id) const override;

  [[nodiscard]] QJsonValue jsonValue(size_t id) const override;

  [[nodiscard]] bool isSameObj(const QJsonValue& v1, const QJsonValue& v2) const override;

  size_t makeAlias(size_t id) override;

  [[nodiscard]] bool isAlias(size_t id) const override;

  QWidget* createObjEditWidget(size_t id) override;

protected:
  void loadROI();

  void setModified();

  // void setModified(bool clean);

  void importMaskImage();

  void createMaskImage();

  // append another ROI into this doc
  size_t addROI(ZROI* roi, const QString& path, bool unsaved = false);

private:
  void createActions();

  bool saveROI(ZROIPack* pack, const QString& fileName, QString& errorMsg);

  // notify obj manager about the update
  void packInfoUpdated(ZROIPack* pack);

private:
  std::map<size_t, std::shared_ptr<ZROIPack>> m_idToROIPacks;

  QAction* m_loadROIAction = nullptr;
  QAction* m_importMaskImageAction = nullptr;
  QAction* m_createMaskImageAction = nullptr;
};

} // namespace nim

