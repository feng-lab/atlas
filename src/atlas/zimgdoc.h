#pragma once

#include "zobjdoc.h"
#include "zimgpack.h"
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

  //
  void setImgOffset(size_t id, int offx, int offy, int offz, int offt);

  void setImgChannelColor(size_t id, size_t c, col4 col);

  void showImg(ZImg* img, const QString& path);

  // ZObjDoc interface
public:
  virtual bool save(size_t id) override;

  virtual bool saveAs(size_t id) override;

  virtual QString typeName() const override
  { return "Image"; }

  virtual QString typePluralName() const override
  { return "Images"; }

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

  virtual QString objDetailedInfo(size_t id) const override;

  virtual QString objTooltip(size_t id) const override;

  virtual QJsonValue jsonValue(size_t id) const override;

  virtual bool isSameObj(const QJsonValue& v1, const QJsonValue& v2) const override;

  virtual size_t makeAlias(size_t id) override;

  virtual bool isAlias(size_t id) const override;

signals:

  void imgChanged(size_t id);

protected:
  void loadImg();

  void importImgZSequence();

  void importImgTimeSequence();

  void stitchImgs();

  void alignSections();

  // append another img into this doc
  size_t addImgPack(ZImgPack* imgPack);

  //
  size_t loadImg(const QString& fileName, FileFormat format, QString& errorMsg);

  size_t loadImg(const QString& fileName, size_t scene, FileFormat format, QString& errorMsg, size_t numScene = 0,
                 const ZImgInfo* info = nullptr,
                 const std::vector<std::shared_ptr<ZImgSubBlock>>* subBlock = nullptr);

  size_t loadImg(const QStringList& files, Dimension catDim, FileFormat format, QString& errorMsg);

  size_t loadImg(const QStringList& files, Dimension catDim, size_t scene, FileFormat format, QString& errorMsg,
                 size_t numScene = 0, const ZImgInfo* info = nullptr,
                 const std::vector<std::shared_ptr<ZImgSubBlock>>* subBlock = nullptr);

  void sendChangedSignal(size_t id);

private:
  void createActions();

  bool saveImg(ZImgPack* pack, QString fileName, FileFormat format, Compression comp, QString& errorMsg);

  // notify obj manager about the update
  void packInfoUpdated(ZImgPack* pack);

private:
  std::map<size_t, std::shared_ptr<ZImgPack>> m_idToImgPacks;

  QAction* m_loadImgAction;
  QAction* m_importImgZSequenceAction;
  QAction* m_importImgTimeSequenceAction;

  QAction* m_stitchImageAction;
  QAction* m_alignSectionsAction;
};

} // namespace nim

