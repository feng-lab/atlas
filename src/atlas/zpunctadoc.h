#pragma once

#include "zobjdoc.h"
#include "zpunctapack.h"

#include <list>
#include <optional>

namespace nim {

class ZPunctaDoc : public ZObjDoc
{
  Q_OBJECT

public:
  explicit ZPunctaDoc(ZDoc& doc);

  // return info of puncta with id, assume puncta exist, otherwise crash
  ZPunctaPack& punctaPack(size_t id)
  {
    return *m_idToPunctaPacks.at(id);
  }

  // Add puncta that does not come from a local file path (e.g. network-backed Neuroglancer annotations).
  // The `sourceJson` is stored in the scene file so the puncta can be reloaded on restore.
  size_t addPunctaFromExternalSource(ZPuncta puncta, QString displayName, QString tooltip, json::value sourceJson);

  [[nodiscard]] std::optional<size_t> findPunctaByExternalSource(const json::value& sourceJson) const;

  void updateExternalPunctaMetadata(size_t id, QString displayName, QString tooltip);

  // Append puncta to an external-source pack without pushing an undo command.
  // Intended for streaming/network sources (e.g. Neuroglancer annotations spatial index).
  void appendExternalPunctaNoUndo(size_t id, std::list<ZPunctum> addedPuncta);

  // ZObjDoc interface

public:
  bool save(size_t id) override;

  bool saveAs(size_t id) override;

  [[nodiscard]] QString typeName() const override
  {
    return "Puncta";
  }

  [[nodiscard]] QString typePluralName() const override
  {
    return "Puncta";
  }

  [[nodiscard]] bool canReadFile(const QString& fileName) const override;

  size_t loadFile(const QString& fileName, QString& errorMsg) override;

  size_t loadFile(const json::value& jValue, QString& errorMsg) override;

  [[nodiscard]] std::vector<QAction*> loadFileActions() const override;

  [[nodiscard]] QMenu* processObjMenu() const override;

  [[nodiscard]] QString objName(size_t id) const override;

  [[nodiscard]] QString objPath(size_t id) const override;

  [[nodiscard]] bool objHasUnsavedChange(size_t id) const override;

  [[nodiscard]] QString objInfo(size_t id) const override;

  [[nodiscard]] QString objTooltip(size_t id) const override;

  [[nodiscard]] const QUndoStack* objUndoStack(size_t id) const override;

  [[nodiscard]] json::value jsonValue(size_t id) const override;

  [[nodiscard]] bool isSameObj(const json::value& v1, const json::value& v2) const override;

  size_t makeAlias(size_t id) override;

  [[nodiscard]] bool isAlias(size_t id) const override;

  QWidget* createObjEditWidget(size_t id) override;

protected:
  void removeObj(size_t id) override;

  void loadPuncta();

  static void detectPuncta();

  static void generateAnalysisTextFiles();

  // append another puncta into this doc
  size_t addPuncta(ZPuncta puncta, const QString& path);

  void setModified(bool clean);

  [[nodiscard]] bool canPrepareLoadAsync(const json::value& jValue) const override;

  [[nodiscard]] folly::coro::Task<ZObjDoc::PreparedLoadResult>
  prepareLoadAsync(const json::value& jValue, const ZObjDoc::AsyncLoadContext& ctx) const override;

private:
  void createActions();

  bool savePuncta(ZPunctaPack* pack, const QString& fileName, QString& errorMsg, const QString& format = "");

  // notify obj manager about the update
  void packInfoUpdated(ZPunctaPack* pack);

private:
  std::map<size_t, std::shared_ptr<ZPunctaPack>> m_idToPunctaPacks;

  QAction* m_loadPunctaAction = nullptr;

  QAction* m_detectPunctaAction = nullptr;
  QAction* m_generateAnalysisTextFilesAction = nullptr;
};

} // namespace nim
