#pragma once

#include "zobjdoc.h"
#include "zregionannotation.h"

namespace nim {

class ZRegionAnnotationDoc : public ZObjDoc
{
Q_OBJECT
public:
  explicit ZRegionAnnotationDoc(ZDoc& doc);

  // return info of RegionAnnotation with id, assume RegionAnnotation exist, otherwise crash
  ZRegionAnnotation& regionAnnotation(size_t id)
  { return *m_idToRegionAnnotationPacks.at(id)->regionAnnotation; }

  ZRegionAnnotation& currentRegionAnnotation();

  // ZObjDoc interface
public:
  bool save(size_t id) override;

  bool saveAs(size_t id) override;

  QString typeName() const override
  { return "RegionAnnotation"; }

  QString typePluralName() const override
  { return "RegionAnnotations"; }

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

  QUndoStack* objUndoStack(size_t id) override;

  QJsonValue jsonValue(size_t id) const override;

  bool isSameObj(const QJsonValue& v1, const QJsonValue& v2) const override;

  size_t makeAlias(size_t id) override;

  bool isAlias(size_t id) const override;

  QWidget* createObjEditWidget(size_t id) override;

protected:
  void loadRegionAnnotation();

  void importLabelImage();

  void exportLabelImage();

  void setModified();

  void setModified(bool clean);

  // append another RegionAnnotation into this doc
  size_t addRegionAnnotation(ZRegionAnnotation* regionAnnotation, const QString& path, bool unsaved = false);

private:
  struct RegionAnnotationPack
  { // RegionAnnotation and its associated data
    RegionAnnotationPack(ZRegionAnnotation* regionAnnotationIn, const QString& pathIn, bool unsaved = false);

    void updateDerivedData();

    const QString& info() const;

    inline const QString& name() const
    { return m_name; }

    inline const QString& tooltip() const
    { return m_tooltip; }

    ZRegionAnnotation* regionAnnotation;
    QString path;
    bool hasUnsavedChange;

    // derived data
  private:
    mutable QString m_info;
    QString m_name;
    QString m_tooltip;
  };

  void createActions();

  bool saveRegionAnnotation(RegionAnnotationPack* pack, const QString& fileName, QString& errorMsg);

  // notify obj manager about the update
  void packInfoUpdated(RegionAnnotationPack* pack);

private:
  std::map<size_t, std::shared_ptr<RegionAnnotationPack>> m_idToRegionAnnotationPacks;

  QAction* m_loadRegionAnnotationAction;
  QAction* m_importLabelImageAction;
  QAction* m_exportLabelImageAction;
};

} // namespace nim

