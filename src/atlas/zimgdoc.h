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

  ZImgPack& imgPack(size_t id)
  {
    return *m_idToImgPacks.at(id);
  }

  [[nodiscard]] const ZImgPack& imgPack(size_t id) const
  {
    return *m_idToImgPacks.at(id);
  }

  void setImgChannelColor(size_t id, size_t c, col4 col);

  // ZObjDoc interface

public:
  bool save(size_t id) override;

  bool saveAs(size_t id) override;

  [[nodiscard]] QString typeName() const override
  {
    return "Image";
  }

  [[nodiscard]] QString typePluralName() const override
  {
    return "Images";
  }

  [[nodiscard]] bool canReadFile(const QString& fileName) const override;

  size_t loadFile(const QString& fileName, QString& errorMsg) override;

  size_t loadFile(const json::value& jValue, QString& errorMsg) override;

  [[nodiscard]] std::vector<QAction*> loadFileActions() const override;

  [[nodiscard]] QMenu* processObjMenu() const override;

  void removeObj(size_t id) override;

  [[nodiscard]] QString objName(size_t id) const override;

  [[nodiscard]] QString objPath(size_t id) const override;

  [[nodiscard]] bool objHasUnsavedChange(size_t id) const override;

  [[nodiscard]] QString objInfo(size_t id) const override;

  [[nodiscard]] QString objDetailedInfo(size_t id) const override;

  [[nodiscard]] QString objTooltip(size_t id) const override;

  [[nodiscard]] json::value jsonValue(size_t id) const override;

  [[nodiscard]] bool isSameObj(const json::value& v1, const json::value& v2) const override;

  size_t makeAlias(size_t id) override;

  [[nodiscard]] bool isAlias(size_t id) const override;

Q_SIGNALS:
  void imgChanged(size_t id);

protected:
  void loadImg();

  void loadNeuroglancerPrecomputed();

  void loadNeuroglancerState();

  void importImgSequence();

  void stitchImgs();

  void alignSections();

  void correctChromaticShift();

  // append another img into this doc
  size_t addImgPack(ZImgPack* imgPack);

  //
  size_t loadImg(const QString& fileName, FileFormat format, QString& errorMsg);

  size_t loadImg(const QString& fileName,
                 size_t scene,
                 FileFormat format,
                 QString& errorMsg,
                 ZImgInfo& info,
                 std::vector<std::shared_ptr<ZImgSubBlock>>& sceneSubBlocks);

  size_t loadImg(const QStringList& files, Dimension catDim, bool catScenes, FileFormat format, QString& errorMsg);

  size_t loadImg(const QStringList& files,
                 Dimension catDim,
                 bool catScenes,
                 size_t scene,
                 FileFormat format,
                 QString& errorMsg,
                 ZImgInfo& info,
                 std::vector<std::shared_ptr<ZImgSubBlock>>& sceneSubBlocks);

  size_t loadImg(const ZImgSource& imgSource, QString& errorMsg);

  size_t loadNeuroglancerPrecomputed(const QString& url, QString& errorMsg);

  void sendChangedSignal(size_t id);

private:
  void createActions();

  bool saveImg(ZImgPack* pack,
               const QString& fileName,
               FileFormat format,
               const ZImgWriteParameters& paras,
               QString& errorMsg);

  // notify obj manager about the update
  void packInfoUpdated(ZImgPack* pack);

private:
  std::map<size_t, std::shared_ptr<ZImgPack>> m_idToImgPacks;

  QAction* m_loadImgAction = nullptr;
  QAction* m_loadNeuroglancerPrecomputedAction = nullptr;
  QAction* m_loadNeuroglancerStateAction = nullptr;
  QAction* m_importImgSequenceAction = nullptr;

  QAction* m_stitchImageAction = nullptr;
  QAction* m_alignSectionsAction = nullptr;
  QAction* m_correctChromaticShiftAction = nullptr;
};

} // namespace nim
