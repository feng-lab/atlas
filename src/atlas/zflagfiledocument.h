#pragma once

#include "zflagsettingsregistry.h"

#include <QByteArray>
#include <QHash>
#include <QString>
#include <QStringList>

namespace nim {

class ZFlagfileDocument
{
public:
  bool load(const QString& path, const QSet<QString>& managedFlagNames, QString* error = nullptr);

  [[nodiscard]] bool fileExistedAtLoad() const
  {
    return m_fileExistedAtLoad;
  }

  [[nodiscard]] bool hasManagedValue(const QString& name) const;

  [[nodiscard]] QString managedValue(const QString& name) const;

  [[nodiscard]] QStringList duplicateManagedFlags() const
  {
    return m_duplicateManagedFlags;
  }

  [[nodiscard]] QStringList preservedManualLines() const
  {
    return m_preservedManualLines;
  }

  [[nodiscard]] bool matchesFileOnDisk(const QString& path, QString* error = nullptr) const;

  static bool writeFile(const QString& path,
                        const std::vector<ZManagedFlagfileEntry>& managedEntries,
                        const QStringList& preservedManualLines,
                        QString* error = nullptr);

private:
  static QByteArray renderFileContents(const std::vector<ZManagedFlagfileEntry>& managedEntries,
                                       const QStringList& preservedManualLines);

private:
  bool m_fileExistedAtLoad = false;
  QByteArray m_loadedBytes;
  QHash<QString, QString> m_managedValues;
  QStringList m_duplicateManagedFlags;
  QStringList m_preservedManualLines;
};

} // namespace nim
