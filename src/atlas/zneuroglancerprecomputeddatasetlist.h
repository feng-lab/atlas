#pragma once

#include "zjson.h"

#include <QString>
#include <vector>

namespace nim {

class ZNeuroglancerPrecomputedDatasetList
{
public:
  struct Entry
  {
    QString name;
    QString kind;
    QString url;
  };

  [[nodiscard]] static QString examplesFilename();
  [[nodiscard]] static QString userHistoryFilename();

  [[nodiscard]] static QString examplesFilePath();
  [[nodiscard]] static QString userHistoryFilePath();

  [[nodiscard]] static std::vector<Entry> loadExamples(QString* errorMsg);
  [[nodiscard]] static std::vector<Entry> loadUserHistory(QString* errorMsg);

  [[nodiscard]] static bool saveUserHistory(const std::vector<Entry>& entries, QString* errorMsg);

  static void normalizeAndDeduplicate(std::vector<Entry>* entries);
  static void upsertMostRecent(std::vector<Entry>* entries, Entry entry);

private:
  [[nodiscard]] static QString normalizedUrl(QString url);
  [[nodiscard]] static std::vector<Entry> parseListObject(const json::object& jo, QString* errorMsg);
  [[nodiscard]] static json::object toListObject(const std::vector<Entry>& entries);
};

} // namespace nim
