#pragma once

#include <QString>
#include <QStringList>
#include <QSet>

#include <vector>

namespace nim {

enum class ZFlagSettingEditorKind
{
  Auto,
  Choice,
  FilePath,
  DirectoryPath,
};

struct ZFlagSettingSpec
{
  QString name;
  QString category;
  QString label;
  ZFlagSettingEditorKind editor = ZFlagSettingEditorKind::Auto;
  QStringList choices;
  bool advanced = false;
};

struct ZManagedFlagfileEntry
{
  QString category;
  QString label;
  QString name;
  QString description;
  QString value;
};

[[nodiscard]] const std::vector<ZFlagSettingSpec>& atlasFlagSettingSpecs();

[[nodiscard]] const ZFlagSettingSpec* atlasFindFlagSettingSpec(const QString& name);

[[nodiscard]] QSet<QString> atlasManagedFlagNames();

[[nodiscard]] QString atlasUserSettingsFlagfileName();

[[nodiscard]] QString atlasUserSettingsFlagfilePath();

[[nodiscard]] std::vector<ZManagedFlagfileEntry> atlasDefaultFlagfileEntries();

} // namespace nim
