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

  // Heuristic: treat strings that look like a Neuroglancer precomputed dataset reference as loadable by this doc.
  // This is a pure string check (no I/O) and is used by both UI and RPC paths.
  [[nodiscard]] static bool looksLikeNeuroglancerPrecomputedUrl(const QString& s);

  ZImgPack& imgPack(size_t id)
  {
    return *m_idToImgPacks.at(id);
  }

  [[nodiscard]] const ZImgPack& imgPack(size_t id) const
  {
    return *m_idToImgPacks.at(id);
  }

  [[nodiscard]] std::shared_ptr<ZImgPack> imgPackShared(size_t id) const
  {
    CHECK(m_idToImgPacks.contains(id));
    return m_idToImgPacks.at(id);
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

  // Add an already-open Neuroglancer precomputed dataset (e.g. opened off the UI thread for RPC tasks).
  // Returns the object id (existing id if already loaded) or 0 on failure.
  size_t addNeuroglancerPrecomputedVolume(std::shared_ptr<ZNeuroglancerPrecomputedVolume> vol, QString& errorMsg);

  [[nodiscard]] std::vector<QAction*> loadFileActions() const override;

  [[nodiscard]] QMenu* processObjMenu() const override;

  [[nodiscard]] QAction* autoTraceAction() const
  {
    return m_autoTraceAction;
  }

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
  void removeObj(size_t id) override;

  void loadImg();

  void loadNeuroglancerPrecomputed();

  void loadNeuroglancerState();

  void importImgSequence();

  void stitchImgs();

  void alignSections();

  void correctChromaticShift();

  void autoTrace();

  // append another img into this doc
  size_t addImgPack(ZImgPack* imgPack);

  [[nodiscard]] bool canPrepareLoadAsync(const json::value& jValue) const override;

  [[nodiscard]] folly::coro::Task<ZObjDoc::PreparedLoadResult>
  prepareLoadAsync(const json::value& jValue, const ZObjDoc::AsyncLoadContext& ctx) const override;

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
  QAction* m_autoTraceAction = nullptr;
};

} // namespace nim
