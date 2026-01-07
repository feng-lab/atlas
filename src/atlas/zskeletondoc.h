#pragma once

#include "zobjdoc.h"

#include "zskeleton.h"

#include <optional>

namespace nim {

class ZSkeletonDoc : public ZObjDoc
{
  Q_OBJECT

public:
  explicit ZSkeletonDoc(ZDoc& doc);

  ZSkeleton& skeleton(size_t id)
  {
    return m_idToSkeletonPacks.at(id)->skeleton;
  }

  // Add a skeleton that does not come from a local file path (e.g. network-backed Neuroglancer skeleton).
  // The `sourceJson` is stored in the scene file so the skeleton can be reloaded on restore.
  size_t addSkeletonFromExternalSource(ZSkeleton& skeleton, QString displayName, QString tooltip, json::value sourceJson);

  [[nodiscard]] std::optional<size_t> findSkeletonByExternalSource(const json::value& sourceJson) const;

  // Update the display name / tooltip of an external-source skeleton (e.g. Neuroglancer),
  // without modifying its geometry. This is useful when optional metadata (like
  // segment_properties) becomes available after the skeleton has already been loaded.
  void updateExternalSkeletonMetadata(size_t id, QString displayName, QString tooltip);

Q_SIGNALS:
  void skeletonChanged(size_t id);

  // ZObjDoc interface

public:
  bool save(size_t id) override;

  bool saveAs(size_t id) override;

  [[nodiscard]] QString typeName() const override
  {
    return "Skeleton";
  }

  [[nodiscard]] QString typePluralName() const override
  {
    return "Skeletons";
  }

  [[nodiscard]] bool canReadFile(const QString& fileName) const override;

  size_t loadFile(const QString& fileName, QString& errorMsg) override;

  size_t loadFile(const json::value& jValue, QString& errorMsg) override;

  [[nodiscard]] std::vector<QAction*> loadFileActions() const override;

  void removeObj(size_t id) override;

  [[nodiscard]] QString objName(size_t id) const override;

  [[nodiscard]] QString objPath(size_t id) const override;

  [[nodiscard]] bool objHasUnsavedChange(size_t id) const override;

  [[nodiscard]] QString objInfo(size_t id) const override;

  [[nodiscard]] QString objTooltip(size_t id) const override;

  [[nodiscard]] json::value jsonValue(size_t id) const override;

  [[nodiscard]] bool isSameObj(const json::value& v1, const json::value& v2) const override;

  size_t makeAlias(size_t id) override;

  [[nodiscard]] bool isAlias(size_t id) const override;

private:
  struct SkeletonPack
  {
    SkeletonPack(ZSkeleton& iskel, QString displayName, QString tooltip, json::value sourceJson);

    ~SkeletonPack() = default;

    void updateDerivedData();

    [[nodiscard]] const QString& info() const;

    [[nodiscard]] const QString& name() const
    {
      return m_name;
    }

    [[nodiscard]] const QString& tooltip() const
    {
      return m_tooltip;
    }

    ZSkeleton skeleton;
    json::value sourceJson;
    QString displayNameOverride;
    QString tooltipOverride;
    bool hasUnsavedChange = false;

  private:
    mutable QString m_info;
    QString m_name;
    QString m_tooltip;
  };

  void createActions();

  void packInfoUpdated(SkeletonPack* pack);

private:
  std::map<size_t, std::shared_ptr<SkeletonPack>> m_idToSkeletonPacks;
};

} // namespace nim
