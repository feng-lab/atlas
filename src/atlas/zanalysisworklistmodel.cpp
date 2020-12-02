#include "zanalysisworklistmodel.h"

#include "zlog.h"
#include <qtcsv/reader.h>
#include <qtcsv/variantdata.h>
#include <qtcsv/writer.h>
#include <QUrl>
#include <QFile>
#include <QTextStream>

namespace nim {

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

QString ZAnalysisWorklistModel::setSource(const QString& filename, QStringConverter::Encoding encoding)
{
  QStringList res;
  beginResetModel();

  reset();

  QList<QStringList> allLines = QtCSV::Reader::readToList(filename, QString(","), QString("\""), encoding);
  if (!allLines.empty()) {
    for (const auto& list: allLines) {
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
  } else {
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

QString ZAnalysisWorklistModel::toCSV(const QString& filename, bool withHeader, QChar separator, QStringConverter::Encoding encoding) const
{
  QtCSV::VariantData vd;
  if (withHeader) {
    vd.addRow(m_header);
  }
  for (size_t row = 0; row < static_cast<size_t>(rowCount()); ++row) {
    std::map<size_t, ZAnalysisTextFileInput*>::const_iterator it = m_rowToInput.find(row);
    if (it != m_rowToInput.end()) {
      const ZAnalysisTextFileInput* input = it->second;
      QList<QVariant> values;
      values << input->imgFilename
             << input->swcFilename
             << input->punctaFilename
             << input->voxelSizeX
             << input->voxelSizeY
             << input->voxelSizeZ
             << input->dendriteChannel
             << input->axonChannel
             << input->maxDistToBranch
             << input->bluenessExtend
             << input->outputFolder
             << (input->doPyramidalFunctionalSeparation ? "yes" : "no")
             << (input->doPyramidalSubclassSeparation ? "yes" : "no")
             << input->somaPunctaFilename;
      vd.addRow(values);
    }
  }
  if (!QtCSV::Writer::write(filename, vd, separator, QString(""), QtCSV::Writer::REWRITE, QStringList(), QStringList(),
                            encoding)) {
    return QString("Can not write csv to file (%1).").arg(filename);
  }
  return QString();
}

int ZAnalysisWorklistModel::rowCount(const QModelIndex& parent) const
{
  if (parent.row() != -1 && parent.column() != -1) return 0;
  return m_rowCount;
}

int ZAnalysisWorklistModel::columnCount(const QModelIndex& parent) const
{
  if (parent.row() != -1 && parent.column() != -1) return 0;
  return m_header.size();
}

QVariant ZAnalysisWorklistModel::data(const QModelIndex& index, int role) const
{
  if (index.parent() != QModelIndex()) return QVariant();
  if (role == Qt::DisplayRole || role == Qt::EditRole || role == Qt::UserRole) {
    if (index.row() < 0 || index.column() < 0 ||
        m_rowToInput.find(index.row()) == m_rowToInput.end() ||
        index.column() >= columnCount())
      return QVariant();
    std::map<size_t, ZAnalysisTextFileInput*>::const_iterator it = m_rowToInput.find(index.row());
    const ZAnalysisTextFileInput* input = it->second;
    switch (index.column()) {
      case 0:
        return input->imgFilename;
        break;
      case 1:
        return input->swcFilename;
        break;
      case 2:
        return input->punctaFilename;
        break;
      case 3:
        return input->voxelSizeX;
        break;
      case 4:
        return input->voxelSizeY;
        break;
      case 5:
        return input->voxelSizeZ;
        break;
      case 6:
        return input->dendriteChannel;
        break;
      case 7:
        return input->axonChannel;
        break;
      case 8:
        return input->maxDistToBranch;
        break;
      case 9:
        return input->bluenessExtend;
        break;
      case 10:
        return input->outputFolder;
        break;
      case 11:
        return input->doPyramidalFunctionalSeparation ? "yes" : "no";
        break;
      case 12:
        return input->doPyramidalSubclassSeparation ? "yes" : "no";
        break;
      case 13:
        return input->somaPunctaFilename;
        break;
      default:
        break;
    }
  }
  return QVariant();
}

bool ZAnalysisWorklistModel::setData(const QModelIndex& index, const QVariant& data, int role)
{
  if (index.parent() != QModelIndex()) return false;

  if (role == Qt::DisplayRole || role == Qt::EditRole || role == Qt::UserRole) {
    if (index.row() >= rowCount() || index.column() >= columnCount() || index.row() < 0 || index.column() < 0)
      return false;
    ZAnalysisTextFileInput* input = nullptr;
    if (m_rowToInput.find(index.row()) == m_rowToInput.end()) {
      m_inputs.push_back(ZAnalysisTextFileInput());
      input = &(m_inputs[m_inputs.size() - 1]);
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

    emit dataChanged(index, index);
    return true;
  }
  return false;
}

QVariant ZAnalysisWorklistModel::headerData(int section, Qt::Orientation orientation, int role) const
{
  if (section < m_header.count() && orientation == Qt::Horizontal &&
      (role == Qt::DisplayRole || role == Qt::EditRole || role == Qt::UserRole))
    return m_header[section];
  else
    return QAbstractTableModel::headerData(section, orientation, role);
}

Qt::ItemFlags ZAnalysisWorklistModel::flags(const QModelIndex& index) const
{
  return Qt::ItemIsEditable | Qt::ItemIsDropEnabled | QAbstractTableModel::flags(index);
}

void ZAnalysisWorklistModel::reset()
{
  m_header.clear();
  m_header << "# imageName" << "swcName" << "punctaName" << "voxelSizeXInUm" << "voxelSizeYInUm"
           << "voxelSizeZInUm" << "dendriteChannel" << "axonChannel(can be empty)"
           << "maxDistToBranch" << "bluenessExtend" << "outputFolder(can be empty)"
           << "doPyramidalFunctionalSeparation(yes or no)" << "doPyramidalSubclassSeparation(yes or no)"
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

bool ZAnalysisWorklistModel::dropMimeData(const QMimeData* data, Qt::DropAction action,
                                          int /*row*/, int /*column*/, const QModelIndex& parent)
{
  //LOG(INFO) << row << " " << column << " " << action << " " << parent.row() << " " << parent.column();

  if (action == Qt::IgnoreAction)
    return true;

  if (!data->hasUrls())
    return false;

  if (parent.column() != 0 && parent.column() != 1 && parent.column() != 2 && parent.column() != 9)
    return false;

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
