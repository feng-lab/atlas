#include "zcsvtable.h"

#include "zioutils.h"
#include "zlog.h"

#include <csv.hpp>

#include <QString>

#include <utility>

namespace nim {
namespace {

csv::CSVFormat makeCsvFormat(const ZCsvReadOptions& options)
{
  csv::CSVFormat format;
  format.delimiter(options.delimiter).quote(options.quote).no_header();
  if (options.trimOuterSpaces) {
    format.trim({' '});
  }
  if (options.keepVariableColumns) {
    format.variable_columns(csv::VariableColumnPolicy::KEEP);
  }
  return format;
}

template<typename Writer>
void writeRows(Writer& writer, const ZCsvTable& rows)
{
  writer.set_auto_flush(false);
  for (const auto& row : rows) {
    writer << row;
  }
  writer.flush();
}

} // namespace

ZCsvTable readCsvTable(const QString& filename, const ZCsvReadOptions& options)
{
  try {
    std::ifstream stream = openIFStream(filename);
    csv::CSVReader reader(stream, makeCsvFormat(options));
    ZCsvTable table;
    for (csv::CSVRow& row : reader) {
      ZCsvRow fields;
      fields.reserve(row.size());
      for (csv::CSVField& field : row) {
        fields.push_back(field.get<>());
      }
      table.push_back(std::move(fields));
    }
    return table;
  }
  catch (const std::exception& e) {
    throw ZException(fmt::format("Can not read CSV file {}: {}", filename, e.what()));
  }
}

void writeCsvTable(const QString& filename, const ZCsvTable& rows)
{
  std::ofstream out = openOFStream(filename, std::ios::out | std::ios::binary | std::ios::trunc);
  auto writer = csv::make_csv_writer(out);
  writeRows(writer, rows);

  if (!out) {
    throw ZException(fmt::format("Can not write CSV file: {}", filename), ZException::Option::CheckErrno);
  }
}

} // namespace nim
