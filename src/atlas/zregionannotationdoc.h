#pragma once

#include "zobjdoc.h"
#include "zregionannotationpack.h"

namespace nim {

class ZRegionAnnotationDoc : public ZObjDoc
{
Q_OBJECT
public:
  explicit ZRegionAnnotationDoc(ZDoc& doc);

  // return info of RegionAnnotation with id, assume RegionAnnotation exist, otherwise crash
  ZRegionAnnotationPack& regionAnnotationPack(size_t id)
  { return *m_idToRegionAnnotationPacks.at(id); }

  ZRegionAnnotationPack& currentRegionAnnotationPack();

  // ZObjDoc interface
public:
  bool save(size_t id) override;

  bool saveAs(size_t id) override;

  [[nodiscard]] QString typeName() const override
  { return "RegionAnnotation"; }

  [[nodiscard]] QString typePluralName() const override
  { return "RegionAnnotations"; }

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
  void loadRegionAnnotation();

  void importLabelImage();

  void exportLabelImage();

  // void setModified();

  void setModified(bool clean);

  // append another RegionAnnotation into this doc
  size_t addRegionAnnotation(ZRegionAnnotation* regionAnnotation, const QString& path);

private:
  void createActions();

  bool saveRegionAnnotation(ZRegionAnnotationPack* pack, const QString& fileName, QString& errorMsg);

  // notify obj manager about the update
  void packInfoUpdated(ZRegionAnnotationPack* pack);

private:
  std::map<size_t, std::shared_ptr<ZRegionAnnotationPack>> m_idToRegionAnnotationPacks;

  QAction* m_loadRegionAnnotationAction = nullptr;
  QAction* m_importLabelImageAction = nullptr;
  QAction* m_exportLabelImageAction = nullptr;
};

} // namespace nim

