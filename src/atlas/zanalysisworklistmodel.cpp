#include "zanalysisworklistmodel.h"

#include "zcsvtable.h"
#include "zexception.h"

#include <fmt/core.h>

#include <QUrl>
#include <QFileInfo>

#include <algorithm>
#include <iterator>
#include <string>
#include <utility>

namespace nim {
namespace {

QStringList toQStringList(const ZCsvRow& row)
{
  QStringList fields;
  for (const auto& field : row) {
    fields << QString::fromStdString(field);
  }
  return fields;
}

} // namespace

ZAnalysisWorklistModel::ZAnalysisWorklistModel(QObject* parent)
  : QAbstractTableModel(parent)
{
  reset();
}

ZAnalysisWorklistModel::ZAnalysisWorklistModel(const QString& filename, QObject* parent)
  : QAbstractTableModel(parent)
{
  setSource(filename);
}

QString ZAnalysisWorklistModel::setSource(const QString& filename)
{
  QStringList res;
  beginResetModel();

  reset();

  ZCsvTable allLines;
  try {
    allLines = readCsvTable(filename);
  }
  catch (const ZException& e) {
    res << QString::fromUtf8(e.what());
  }
  if (!allLines.empty()) {
    for (const auto& row : allLines) {
      const QStringList list = toQStringList(row);
      if (list.empty() || list.at(0).startsWith("#")) {
        continue;
      }
      if (list.size() != m_header.size()) {
        res << QString("Can not parse line (%1) with format <%2>.").arg(list.join(",")).arg(m_header.join(","));
        continue;
      }
      ZAnalysisTextFileInput input;
      bool ok = false;
      input.imgFilename = list[0];
      input.swcFilename = list[1];
      input.punctaFilename = list[2];
      if (!list[3].isEmpty()) {
        input.voxelSizeX = list[3].toDouble(&ok);
        if (!ok) {
          res << QString("Can not parse line (%1) with format <%2>.").arg(list.join(",")).arg(m_header.join(","));
          continue;
        }
      }
      if (!list[4].isEmpty()) {
        input.voxelSizeY = list[4].toDouble(&ok);
        if (!ok) {
          res << QString("Can not parse line (%1) with format <%2>.").arg(list.join(",")).arg(m_header.join(","));
          continue;
        }
      }
      if (!list[5].isEmpty()) {
        input.voxelSizeZ = list[5].toDouble(&ok);
        if (!ok) {
          res << QString("Can not parse line (%1) with format <%2>.").arg(list.join(",")).arg(m_header.join(","));
          continue;
        }
      }
      input.dendriteChannel = list[6].toInt(&ok);
      if (!ok) {
        res << QString("Can not parse line (%1) with format <%2>.").arg(list.join(",")).arg(m_header.join(","));
        continue;
      }
      if (!list[7].isEmpty()) {
        input.axonChannel = list[7].toInt(&ok);
        if (!ok) {
          res << QString("Can not parse line (%1) with format <%2>.").arg(list.join(",")).arg(m_header.join(","));
          continue;
        }
      }
      input.maxDistToBranch = list[8].toDouble(&ok);
      if (!ok) {
        res << QString("Can not parse line (%1) with format <%2>.").arg(list.join(",")).arg(m_header.join(","));
        continue;
      }
      input.bluenessExtend = list[9].toDouble(&ok);
      if (!ok) {
        res << QString("Can not parse line (%1) with format <%2>.").arg(list.join(",")).arg(m_header.join(","));
        continue;
      }
      input.outputFolder = list[10];
      if (list[11].compare("yes", Qt::CaseInsensitive) == 0) {
        input.doPyramidalFunctionalSeparation = true;
      } else if (list[11].compare("no", Qt::CaseInsensitive) == 0) {
        input.doPyramidalFunctionalSeparation = false;
      } else {
        res << QString("Can not parse line (%1) with format <%2>.").arg(list.join(",")).arg(m_header.join(","));
        continue;
      }
      if (list[12].compare("yes", Qt::CaseInsensitive) == 0) {
        input.doPyramidalSubclassSeparation = true;
      } else if (list[12].compare("no", Qt::CaseInsensitive) == 0) {
        input.doPyramidalSubclassSeparation = false;
      } else {
        res << QString("Can not parse line (%1) with format <%2>.").arg(list.join(",")).arg(m_header.join(","));
        continue;
      }
      input.somaPunctaFilename = list[13];
      m_inputs.push_back(input);
    }
  } else if (res.empty()) {
    res << QString("Can not parse file (%1) or file is empty.").arg(filename);
  }

  size_t row = 0;
  for (auto& input : m_inputs) {
    m_rowToInput[row++] = &input;
  }
  m_rowCount += m_inputs.size();

  endResetModel();

  return res.join("\n");
}

QString ZAnalysisWorklistModel::toCSV(const QString& filename, bool withHeader) const
{
  ZCsvTable rows;
  if (withHeader) {
    ZCsvRow header;
    header.reserve(static_cast<size_t>(m_header.size()));
    std::transform(m_header.cbegin(), m_header.cend(), std::back_inserter(header), [](const QString& value) {
      return value.toStdString();
    });
    rows.push_back(std::move(header));
  }
  for (size_t row = 0; row < static_cast<size_t>(rowCount()); ++row) {
    auto it = m_rowToInput.find(row);
    if (it != m_rowToInput.end()) {
      const ZAnalysisTextFileInput* input = it->second;
      rows.push_back({
        input->imgFilename.toStdString(),
        input->swcFilename.toStdString(),
        input->punctaFilename.toStdString(),
        fmt::format("{}", input->voxelSizeX),
        fmt::format("{}", input->voxelSizeY),
        fmt::format("{}", input->voxelSizeZ),
        fmt::format("{}", input->dendriteChannel),
        fmt::format("{}", input->axonChannel),
        fmt::format("{}", input->maxDistToBranch),
        fmt::format("{}", input->bluenessExtend),
        input->outputFolder.toStdString(),
        input->doPyramidalFunctionalSeparation ? "yes" : "no",
        input->doPyramidalSubclassSeparation ? "yes" : "no",
        input->somaPunctaFilename.toStdString(),
      });
    }
  }
  try {
    writeCsvTable(filename, rows);
  }
  catch (const ZException& e) {
    return QString("Can not write csv to file (%1): %2").arg(filename, QString::fromUtf8(e.what()));
  }
  return {};
}

int ZAnalysisWorklistModel::rowCount(const QModelIndex& parent) const
{
  if (parent.row() != -1 && parent.column() != -1) {
    return 0;
  }
  return m_rowCount;
}

int ZAnalysisWorklistModel::columnCount(const QModelIndex& parent) const
{
  if (parent.row() != -1 && parent.column() != -1) {
    return 0;
  }
  return m_header.size();
}

QVariant ZAnalysisWorklistModel::data(const QModelIndex& index, int role) const
{
  if (index.parent() != QModelIndex()) {
    return {};
  }
  if (role == Qt::DisplayRole || role == Qt::EditRole || role == Qt::UserRole) {
    if (index.row() < 0 || index.column() < 0 || !m_rowToInput.contains(index.row()) ||
        index.column() >= columnCount()) {
      return {};
    }
    auto it = m_rowToInput.find(index.row());
    const ZAnalysisTextFileInput* input = it->second;
    switch (index.column()) {
      case 0:
        return input->imgFilename;
      case 1:
        return input->swcFilename;
      case 2:
        return input->punctaFilename;
      case 3:
        return input->voxelSizeX;
      case 4:
        return input->voxelSizeY;
      case 5:
        return input->voxelSizeZ;
      case 6:
        return input->dendriteChannel;
      case 7:
        return input->axonChannel;
      case 8:
        return input->maxDistToBranch;
      case 9:
        return input->bluenessExtend;
      case 10:
        return input->outputFolder;
      case 11:
        return input->doPyramidalFunctionalSeparation ? "yes" : "no";
      case 12:
        return input->doPyramidalSubclassSeparation ? "yes" : "no";
      case 13:
        return input->somaPunctaFilename;
      default:
        break;
    }
  }
  return {};
}

bool ZAnalysisWorklistModel::setData(const QModelIndex& index, const QVariant& data, int role)
{
  if (index.parent() != QModelIndex()) {
    return false;
  }

  if (role == Qt::DisplayRole || role == Qt::EditRole || role == Qt::UserRole) {
    if (index.row() >= rowCount() || index.column() >= columnCount() || index.row() < 0 || index.column() < 0) {
      return false;
    }
    ZAnalysisTextFileInput* input;
    if (!m_rowToInput.contains(index.row())) {
      m_inputs.emplace_back();
      input = &m_inputs.back();
      m_rowToInput[index.row()] = input;
    } else {
      input = m_rowToInput[index.row()];
    }

    switch (index.column()) {
      case 0:
        input->imgFilename = data.toString();
        break;
      case 1:
        input->swcFilename = data.toString();
        break;
      case 2:
        input->punctaFilename = data.toString();
        break;
      case 3:
        input->voxelSizeX = data.toDouble();
        break;
      case 4:
        input->voxelSizeY = data.toDouble();
        break;
      case 5:
        input->voxelSizeZ = data.toDouble();
        break;
      case 6:
        input->dendriteChannel = data.toInt();
        break;
      case 7:
        input->axonChannel = data.toInt();
        break;
      case 8:
        input->maxDistToBranch = data.toDouble();
        break;
      case 9:
        input->bluenessExtend = data.toDouble();
        break;
      case 10:
        input->outputFolder = data.toString();
        break;
      case 11:
        input->doPyramidalFunctionalSeparation = data.toString().compare("yes", Qt::CaseInsensitive) == 0;
        break;
      case 12:
        input->doPyramidalSubclassSeparation = data.toString().compare("yes", Qt::CaseInsensitive) == 0;
        break;
      case 13:
        input->somaPunctaFilename = data.toString();
        break;
      default:
        break;
    }

    Q_EMIT dataChanged(index, index);
    return true;
  }
  return false;
}

QVariant ZAnalysisWorklistModel::headerData(int section, Qt::Orientation orientation, int role) const
{
  if (section < m_header.count() && orientation == Qt::Horizontal &&
      (role == Qt::DisplayRole || role == Qt::EditRole || role == Qt::UserRole)) {
    return m_header[section];
  } else {
    return QAbstractTableModel::headerData(section, orientation, role);
  }
}

Qt::ItemFlags ZAnalysisWorklistModel::flags(const QModelIndex& index) const
{
  return Qt::ItemIsEditable | Qt::ItemIsDropEnabled | QAbstractTableModel::flags(index);
}

void ZAnalysisWorklistModel::reset()
{
  m_header.clear();
  m_header << "# imageName"
           << "swcName"
           << "punctaName"
           << "voxelSizeXInUm"
           << "voxelSizeYInUm"
           << "voxelSizeZInUm"
           << "dendriteChannel"
           << "axonChannel(can be empty)"
           << "maxDistToBranch"
           << "bluenessExtend"
           << "outputFolder(can be empty)"
           << "doPyramidalFunctionalSeparation(yes or no)"
           << "doPyramidalSubclassSeparation(yes or no)"
           << "somaPunctaName";
  m_inputs.clear();
  m_rowToInput.clear();
  m_rowCount = 5000;
}

QStringList ZAnalysisWorklistModel::mimeTypes() const
{
  QStringList qstrList;
  qstrList << "text/uri-list";
  return qstrList;
}

Qt::DropActions ZAnalysisWorklistModel::supportedDropActions() const
{
  return Qt::MoveAction | Qt::CopyAction | Qt::LinkAction;
}

bool ZAnalysisWorklistModel::dropMimeData(const QMimeData* data,
                                          Qt::DropAction action,
                                          int /*row*/,
                                          int /*column*/,
                                          const QModelIndex& parent)
{
  // VLOG(1) << row << " " << column << " " << action << " " << parent.row() << " " << parent.column();

  if (action == Qt::IgnoreAction) {
    return true;
  }

  if (!data->hasUrls()) {
    return false;
  }

  if (parent.column() != 0 && parent.column() != 1 && parent.column() != 2 && parent.column() != 9) {
    return false;
  }

  QUrl url = data->urls().at(0);
  QString file = url.toLocalFile();
  if (!file.isEmpty()) {
    QFileInfo fi(file);
    QModelIndex idx = index(parent.row(), parent.column(), QModelIndex());
    if (parent.column() == 0 || parent.column() == 1 || parent.column() == 2) {
      setData(idx, fi.absoluteFilePath(), Qt::EditRole);
    } else {
      setData(idx, fi.isDir() ? fi.absoluteFilePath() : fi.absolutePath(), Qt::EditRole);
    }
  }

  return true;
}

} // namespace nim
