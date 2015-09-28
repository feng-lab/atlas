#ifndef ZREGIONANNOTATIONDOC_H
#define ZREGIONANNOTATIONDOC_H

#include "zobjdoc.h"
#include "zregionannotation.h"

namespace nim {

class ZRegionAnnotationDoc : public ZObjDoc
{
  Q_OBJECT
public:
  explicit ZRegionAnnotationDoc(ZDoc &doc);

  // return info of RegionAnnotation with id, assume RegionAnnotation exist, otherwise crash
  ZRegionAnnotation& regionAnnotation(size_t id) { return *m_idToRegionAnnotationPacks.at(id)->regionAnnotation; }

  // ZObjDoc interface
public:
  virtual bool save(size_t id) override;
  virtual bool saveAs(size_t id) override;
  virtual QString typeName() const override { return "RegionAnnotation"; }
  virtual QString typePluralName() const override { return "RegionAnnotations"; }
  virtual bool canReadFile(const QString &fileName) override;
  virtual size_t loadFile(const QString &fileName, QString &errorMsg) override;
  virtual size_t loadFile(const QJsonValue &jValue, QString &errorMsg) override;
  virtual QList<QAction*> loadFileActions() const override;
  virtual QMenu* processObjMenu() const override;
  virtual void removeObj(size_t id) override;
  virtual const QString& objName(size_t id) const override;
  virtual QString objPath(size_t id) const override;
  virtual bool objHasUnsavedChange(size_t id) const override;
  virtual const QString& objInfo(size_t id) const override;
  virtual const QString& objTooltip(size_t id) const override;
  virtual QUndoStack* objUndoStack(size_t id) override;
  virtual QJsonValue jsonValue(size_t id) const override;
  virtual bool isSameObj(const QJsonValue& v1, const QJsonValue& v2) const override;
  virtual size_t makeAlias(size_t id) override;
  virtual bool isAlias(size_t id) const override;
  virtual QWidget *createObjEditWidget(size_t id) override;

protected slots:
  void loadRegionAnnotation();
  void importLabelImage();
  void exportLabelImage();
  void setModified();
  void setModified(bool clean);

protected:
  // append another RegionAnnotation into this doc
  size_t addRegionAnnotation(ZRegionAnnotation *regionAnnotation, const QString &path, bool unsaved = false);

signals:

private:
  struct RegionAnnotationPack { // RegionAnnotation and its associated data
    RegionAnnotationPack(ZRegionAnnotation *regionAnnotation, const QString &path, bool unsaved = false);
    ~RegionAnnotationPack();

    void updateDerivedData();
    const QString& info() const;
    inline const QString& name() const { return m_name; }
    inline const QString& tooltip() const { return m_tooltip; }

    ZRegionAnnotation *regionAnnotation;
    QString path;
    bool hasUnsavedChange;

    // derived data
  private:
    mutable QString m_info;
    QString m_name;
    QString m_tooltip;
  };

  void createActions();
  bool saveRegionAnnotation(RegionAnnotationPack *pack, const QString &fileName, QString &errorMsg);
  // notify obj manager about the update
  void packInfoUpdated(RegionAnnotationPack* pack);

private:
  std::map<size_t, std::shared_ptr<RegionAnnotationPack>> m_idToRegionAnnotationPacks;

  QAction *m_loadRegionAnnotationAction;
  QAction *m_importLabelImageAction;
  QAction *m_exportLabelImageAction;
};

} // namespace nim

#endif // ZREGIONANNOTATIONDOC_H
