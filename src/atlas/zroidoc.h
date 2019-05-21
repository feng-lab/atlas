#pragma once

#include "zobjdoc.h"
#include "zroi.h"

namespace nim {

class ZROIDoc : public ZObjDoc
{
Q_OBJECT
public:
  explicit ZROIDoc(ZDoc& doc);

  inline ZROI& roi(size_t id)
  { return *m_idToROIPacks.at(id)->roi; }

  inline const ZROI& roi(size_t id) const
  { return *m_idToROIPacks.at(id)->roi; }

  ZROI& currentROI();

  void askToSave(const ZROI& roi, const QString& title = "");

  // ZObjDoc interface
public:
  bool save(size_t id) override;

  bool saveAs(size_t id) override;

  QString typeName() const override
  { return "ROI"; }

  QString typePluralName() const override
  { return "ROI"; }

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
  void loadROI();

  void setModified();

  void importMaskImage();

  void createMaskImage();

  // append another ROI into this doc
  size_t addROI(ZROI* roi, const QString& path, bool unsaved = false);

private:
  struct ROIPack
  { // ROI and its associated data
    ROIPack(ZROI* roi_, const QString& path_);

    void updateDerivedData();

    const QString& info() const;

    inline const QString& name() const
    { return m_name; }

    inline const QString& tooltip() const
    { return m_tooltip; }

    std::unique_ptr<ZROI> roi;
    QString path;
    bool hasUnsavedChange = false;

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

  bool saveROI(ROIPack* pack, const QString& fileName, QString& errorMsg);

  // notify obj manager about the update
  void packInfoUpdated(ROIPack* pack);

private:
  std::map<size_t, std::shared_ptr<ROIPack>> m_idToROIPacks;

  QAction* m_loadROIAction;
  QAction* m_importMaskImageAction;
  QAction* m_createMaskImageAction;
};

} // namespace nim

