#include "zflagfiledocument.h"

#include <QFile>
#include <QSaveFile>

#include <algorithm>

namespace nim {
namespace {

constexpr auto kManualBlockBegin = "# ---- Atlas preserved manual entries: begin ----";
constexpr auto kManualBlockEnd = "# ---- Atlas preserved manual entries: end ----";

QStringList wrapCommentText(const QString& text, int width = 96)
{
  const QString normalized = text.simplified();
  if (normalized.isEmpty()) {
    return {};
  }

  const QStringList words = normalized.split(' ', Qt::SkipEmptyParts);
  QStringList out;
  QString current = "#";
  for (const QString& word : words) {
    if (current.size() == 1) {
      current += ' ';
      current += word;
      continue;
    }
    if (current.size() + 1 + word.size() > width) {
      out.push_back(current);
      current = "# " + word;
    } else {
      current += ' ';
      current += word;
    }
  }
  if (!current.isEmpty()) {
    out.push_back(current);
  }
  return out;
}

bool parseFlagLine(const QString& rawLine, QString* name, QString* value)
{
  const QString trimmedLeft = rawLine.trimmed().isEmpty() ? QString() : rawLine;
  if (trimmedLeft.isEmpty()) {
    return false;
  }

  int start = 0;
  while (start < rawLine.size() && rawLine[start].isSpace()) {
    ++start;
  }
  if (start >= rawLine.size() || rawLine[start] != '-') {
    return false;
  }

  int dashCount = 0;
  while (start + dashCount < rawLine.size() && rawLine[start + dashCount] == '-' && dashCount < 2) {
    ++dashCount;
  }
  if (dashCount == 0) {
    return false;
  }

  const int eqIndex = rawLine.indexOf('=', start + dashCount);
  if (eqIndex < 0) {
    return false;
  }

  *name = rawLine.mid(start + dashCount, eqIndex - (start + dashCount)).trimmed();
  *value = rawLine.mid(eqIndex + 1);
  return !name->isEmpty();
}

void appendCommentBlock(QStringList& lines, const QString& title, const QString& description)
{
  if (!title.trimmed().isEmpty()) {
    lines.push_back("# " + title.trimmed());
  }
  const QStringList wrapped = wrapCommentText(description);
  lines.append(wrapped);
}

} // namespace

bool ZFlagfileDocument::load(const QString& path, const QSet<QString>& managedFlagNames, QString* error)
{
  m_fileExistedAtLoad = false;
  m_loadedBytes.clear();
  m_managedValues.clear();
  m_duplicateManagedFlags.clear();
  m_preservedManualLines.clear();

  QFile file(path);
  if (!file.exists()) {
    return true;
  }
  if (!file.open(QIODevice::ReadOnly)) {
    if (error != nullptr) {
      *error = file.errorString();
    }
    return false;
  }

  m_fileExistedAtLoad = true;
  m_loadedBytes = file.readAll();
  const QString content = QString::fromUtf8(m_loadedBytes);
  const QStringList lines = content.split('\n', Qt::KeepEmptyParts);

  bool inManualBlock = false;
  for (QString line : lines) {
    if (line.endsWith('\r')) {
      line.chop(1);
    }

    const QString trimmed = line.trimmed();
    if (trimmed == QLatin1String(kManualBlockBegin)) {
      inManualBlock = true;
      continue;
    }
    if (trimmed == QLatin1String(kManualBlockEnd)) {
      inManualBlock = false;
      continue;
    }

    QString flagName;
    QString flagValue;
    if (parseFlagLine(line, &flagName, &flagValue)) {
      if (managedFlagNames.contains(flagName)) {
        if (m_managedValues.contains(flagName) && !m_duplicateManagedFlags.contains(flagName)) {
          m_duplicateManagedFlags.push_back(flagName);
        }
        m_managedValues.insert(flagName, flagValue);
      } else if (inManualBlock || !trimmed.isEmpty()) {
        m_preservedManualLines.push_back(line);
      }
      continue;
    }

    if (inManualBlock) {
      m_preservedManualLines.push_back(line);
    }
  }

  return true;
}

bool ZFlagfileDocument::hasManagedValue(const QString& name) const
{
  return m_managedValues.contains(name);
}

QString ZFlagfileDocument::managedValue(const QString& name) const
{
  return m_managedValues.value(name);
}

bool ZFlagfileDocument::matchesFileOnDisk(const QString& path, QString* error) const
{
  QFile file(path);
  if (!m_fileExistedAtLoad) {
    if (file.exists()) {
      if (error != nullptr) {
        *error = QStringLiteral("The settings file was created or changed after this dialog opened.");
      }
      return false;
    }
    return true;
  }

  if (!file.exists()) {
    if (error != nullptr) {
      *error = QStringLiteral("The settings file was removed after this dialog opened.");
    }
    return false;
  }
  if (!file.open(QIODevice::ReadOnly)) {
    if (error != nullptr) {
      *error = file.errorString();
    }
    return false;
  }

  const QByteArray currentBytes = file.readAll();
  if (currentBytes != m_loadedBytes) {
    if (error != nullptr) {
      *error = QStringLiteral("The settings file changed on disk after this dialog opened.");
    }
    return false;
  }
  return true;
}

bool ZFlagfileDocument::writeFile(const QString& path,
                                  const std::vector<ZManagedFlagfileEntry>& managedEntries,
                                  const QStringList& preservedManualLines,
                                  QString* error)
{
  QSaveFile file(path);
  if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
    if (error != nullptr) {
      *error = file.errorString();
    }
    return false;
  }

  const QByteArray bytes = renderFileContents(managedEntries, preservedManualLines);
  if (file.write(bytes) != bytes.size()) {
    if (error != nullptr) {
      *error = file.errorString();
    }
    return false;
  }
  if (!file.commit()) {
    if (error != nullptr) {
      *error = file.errorString();
    }
    return false;
  }

  return true;
}

QByteArray ZFlagfileDocument::renderFileContents(const std::vector<ZManagedFlagfileEntry>& managedEntries,
                                                 const QStringList& preservedManualLines)
{
  QStringList lines;
  lines.push_back("# Atlas user settings");
  lines.push_back("# Atlas reads this file at startup if it exists.");
  lines.push_back("# Changes here take effect the next time Atlas starts.");
  lines.push_back("# The managed section below is rewritten by Atlas Settings.");
  lines.push_back("# Additional custom flags can be placed in the preserved manual block.");
  lines.push_back("");

  QString currentCategory;
  for (const auto& entry : managedEntries) {
    if (entry.category != currentCategory) {
      currentCategory = entry.category;
      if (lines.back().isEmpty()) {
        lines.pop_back();
      }
      lines.push_back("");
      lines.push_back("# ==========================================================================");
      lines.push_back("# " + currentCategory);
      lines.push_back("# ==========================================================================");
    }

    lines.push_back("");
    appendCommentBlock(lines, entry.label, entry.description);
    lines.push_back(QString("--%1=%2").arg(entry.name, entry.value));
  }

  lines.push_back("");
  lines.push_back("# ==========================================================================");
  lines.push_back("# Preserved Manual Entries");
  lines.push_back("# ==========================================================================");
  lines.push_back("# Atlas preserves the lines between the markers below across GUI saves.");
  lines.push_back("# Put custom flags here if Atlas does not expose them in the Settings dialog.");
  lines.push_back(QString::fromLatin1(kManualBlockBegin));
  lines.append(preservedManualLines);
  lines.push_back(QString::fromLatin1(kManualBlockEnd));
  lines.push_back("");

  return lines.join('\n').toUtf8();
}

} // namespace nim
