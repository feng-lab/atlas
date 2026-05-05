#pragma once

#include <string>
#include <vector>

class QString;

namespace nim {

using ZCsvRow = std::vector<std::string>;
using ZCsvTable = std::vector<ZCsvRow>;

struct ZCsvReadOptions
{
  char delimiter = ',';
  char quote = '"';
  bool trimOuterSpaces = true;
  bool keepVariableColumns = true;
};

[[nodiscard]] ZCsvTable readCsvTable(const QString& filename, const ZCsvReadOptions& options = {});

void writeCsvTable(const QString& filename, const ZCsvTable& rows);

} // namespace nim
