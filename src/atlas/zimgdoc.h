#pragma once

#include "zimgpack.h"
#include "zobjdoc.h"
#include <QStringList>

namespace nim {

class ZImgDoc : public ZObjDoc
{
Q_OBJECT
public:
  explicit ZImgDoc(ZDoc& doc);

  inline ZImgPack& imgPack(size_t id)
  { return *m_idToImgPacks.at(id); }

  inline const ZImgPack& imgPack(size_t id) const
  { return *m_idToImgPacks.at(id); }

  void setImgChannelColor(size_t id, size_t c, col4 col);

  // ZObjDoc interface
public:
  bool save(size_t id) override;

  bool saveAs(size_t id) override;

  QString typeName() const override
  { return "Image"; }

  QString typePluralName() const override
  { return "Images"; }

  bool canReadFile(const QString& fileName) const override;

  size_t loadFile(const QString& fileName, QString& errorMsg) override;

  size_t loadFile(const QJsonValue& jValue, QString& errorMsg) override;

  QList<QAction*> loadFileActions() const override;

  QMenu* processObjMenu() const override;

  void removeObj(size_t id) override;

  QString objName(size_t id) const override;

  QString objPath(size_t id) const override;

  bool objHasUnsavedChange(size_t id) const override;

  QString objInfo(size_t id) const override;

  QString objDetailedInfo(size_t id) const override;

  QString objTooltip(size_t id) const override;

  QJsonValue jsonValue(size_t id) const override;

  bool isSameObj(const QJsonValue& v1, const QJsonValue& v2) const override;

  size_t makeAlias(size_t id) override;

  bool isAlias(size_t id) const override;

signals:

  void imgChanged(size_t id);

protected:
  void loadImg();

  void importImgSequence();

  void stitchImgs();

  void alignSections();

  void correctChromaticShift();

  // append another img into this doc
  size_t addImgPack(ZImgPack* imgPack);

  //
  size_t loadImg(const QString& fileName, FileFormat format, QString& errorMsg);

  size_t loadImg(const QString& fileName, size_t scene, FileFormat format, QString& errorMsg,
                 const ZImgInfo* info = nullptr,
                 const std::vector<std::shared_ptr<ZImgSubBlock>>* subBlock = nullptr);

  size_t loadImg(const QStringList& files, Dimension catDim, bool catScenes, FileFormat format, QString& errorMsg);

  size_t loadImg(const QStringList& files, Dimension catDim, bool catScenes, size_t scene, FileFormat format, QString& errorMsg,
                 const ZImgInfo* info = nullptr,
                 const std::vector<std::shared_ptr<ZImgSubBlock>>* subBlock = nullptr);

  size_t loadImg(const ZImgSource& imgSource, QString& errorMsg);

  void sendChangedSignal(size_t id);

private:
  void createActions();

  bool saveImg(ZImgPack* pack, const QString& fileName, FileFormat format,
               const ZImgWriteParameters& paras, QString& errorMsg);

  // notify obj manager about the update
  void packInfoUpdated(ZImgPack* pack);

private:
  std::map<size_t, std::shared_ptr<ZImgPack>> m_idToImgPacks;

  QAction* m_loadImgAction;
  QAction* m_importImgSequenceAction;

  QAction* m_stitchImageAction;
  QAction* m_alignSectionsAction;
  QAction* m_correctChromaticShiftAction;
};

} // namespace nim

