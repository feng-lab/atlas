#ifndef ZROIDOC_H
#define ZROIDOC_H

#include "zobjdoc.h"
#include "zroi.h"

namespace nim {

class ZROIDoc : public ZObjDoc
{
  Q_OBJECT
public:
  explicit ZROIDoc(ZDoc &doc);
  ~ZROIDoc();

  inline ZROI& roi(size_t id) { return *m_idToROIPacks.at(id)->roi; }
  inline const ZROI& roi(size_t id) const { return *m_idToROIPacks.at(id)->roi; }

  ZROI& currentROI();

  // ZObjDoc interface
public:
  virtual bool save(size_t id) override;
  virtual bool saveAs(size_t id) override;
  virtual QString typeName() const override { return "ROI"; }
  virtual QString typePluralName() const override { return "ROI"; }
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

protected slots:
  void loadROI();

protected slots:
  void setModified();
  void createMaskImage();

protected:
  // append another ROI into this doc
  size_t addROI(ZROI* roi, const QString &path);

signals:

private:
  struct ROIPack { // ROI and its associated data
    ROIPack(ZROI *roi, const QString &path);
    ~ROIPack();

    void updateDerivedData();
    const QString& info() const;
    inline const QString& name() const { return m_name; }
    inline const QString& tooltip() const { return m_tooltip; }

    ZROI *roi;
    QString path;
    bool hasUnsavedChange;

  private:
    QString generateUniqueName();

    // derived data
  private:
    mutable QString m_info;
    QString m_name;
    QString m_tmpName;
    QString m_tooltip;
  };

  void createActions();
  bool saveROI(ROIPack *pack, const QString &fileName, QString &errorMsg);
  // notify obj manager about the update
  void packInfoUpdated(ROIPack* pack);

private:
  std::map<size_t, ROIPack*> m_idToROIPacks;

  QAction *m_loadROIAction;

  QAction *m_createMaskImageAction;
};

} // namespace nim

#endif // ZROIDOC_H
