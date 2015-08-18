#include "zanalysisworklistmodel.h"

#include <QFile>
#include <QTextStream>
#include "QsLog.h"
#include <QUrl>

namespace nim {

ZAnalysisWorklistModel::ZAnalysisWorklistModel(QObject *parent)
  : QAbstractTableModel(parent)
{
  reset();
}

ZAnalysisWorklistModel::ZAnalysisWorklistModel(QIODevice *file, QObject *parent)
  : QAbstractTableModel(parent)
{
  setSource(file);
}

ZAnalysisWorklistModel::ZAnalysisWorklistModel(const QString &filename, QObject *parent)
  : QAbstractTableModel(parent)
{
  QFile src(filename);
  setSource(&src);
}

ZAnalysisWorklistModel::~ZAnalysisWorklistModel()
{
}

void ZAnalysisWorklistModel::setSource(QIODevice *file, QTextCodec *codec)
{
  beginResetModel();

  reset();

  if(!file->isOpen())
    file->open(QIODevice::ReadOnly);

  QTextStream stream(file);
  if(codec) {
    stream.setCodec(codec);
  } else {
    stream.setAutoDetectUnicode(true);
  }

  QString format("imageName,swcName,punctaName,voxelSizeXInUm,voxelSizeYInUm,voxelSizeZInUm,dendriteChannel,"
                 "axonChannel(can be empty),maxDistToBranch,bluenessExtend,outputFolder(can be empty),doPyramidalFunctionalSeparation(yes or no),doPyramidalSubclassSeparation(yes or no),"
                 "somaPunctaName");

  while(!stream.atEnd()) {
    QString line = file->readLine();
    line = line.trimmed();
    if (line.startsWith("#") || line.isEmpty())
      continue;

    QStringList list = line.split(",");
    if (list.size() == 14) {
      ZAnalysisTextFileInput input;
      bool ok = false;
      input.imgFilename = list[0];
      input.swcFilename = list[1];
      input.punctaFilename = list[2];
      if (!list[3].isEmpty()) {
        input.voxelSizeX = list[3].toDouble(&ok);
        if (!ok) {
          LERROR() << "Can not parse line: <" << line <<  "> with format:" << format;
          continue;
        }
      }
      if (!list[4].isEmpty()) {
        input.voxelSizeY = list[4].toDouble(&ok);
        if (!ok) {
          LERROR() << "Can not parse line: <" << line <<  "> with format:" << format;
          continue;
        }
      }
      if (!list[5].isEmpty()) {
        input.voxelSizeZ = list[5].toDouble(&ok);
        if (!ok) {
          LERROR() << "Can not parse line: <" << line <<  "> with format:" << format;
          continue;
        }
      }
      input.dendriteChannel = list[6].toInt(&ok);
      if (!ok) {
        LERROR() << "Can not parse line: <" << line <<  "> with format:" << format;
        continue;
      }
      if (!list[7].isEmpty()) {
        input.axonChannel = list[7].toInt(&ok);
        if (!ok) {
          LERROR() << "Can not parse line: <" << line <<  "> with format:" << format;
          continue;
        }
      }
      input.maxDistToBranch = list[8].toDouble(&ok);
      if (!ok) {
        LERROR() << "Can not parse line: <" << line <<  "> with format:" << format;
        continue;
      }
      input.bluenessExtend = list[9].toDouble(&ok);
      if (!ok) {
        LERROR() << "Can not parse line: <" << line <<  "> with format:" << format;
        continue;
      }
      input.outputFolder = list[10];
      if (list[11].compare("yes", Qt::CaseInsensitive) == 0) {
        input.doPyramidalFunctionalSeparation = true;
      } else if (list[11].compare("no", Qt::CaseInsensitive) == 0) {
        input.doPyramidalFunctionalSeparation = false;
      } else {
        LERROR() << "Can not parse line: <" << line <<  "> with format:" << format;
        continue;
      }
      if (list[12].compare("yes", Qt::CaseInsensitive) == 0) {
        input.doPyramidalSubclassSeparation = true;
      } else if (list[12].compare("no", Qt::CaseInsensitive) == 0) {
        input.doPyramidalSubclassSeparation = false;
      } else {
        LERROR() << "Can not parse line: <" << line <<  "> with format:" << format;
        continue;
      }
      input.somaPunctaFilename = list[13];
      m_inputs.push_back(input);
    }
  }

  file->close();

  size_t row = 0;
  for (QList<ZAnalysisTextFileInput>::iterator it=m_inputs.begin();
       it != m_inputs.end(); ++it) {
    ZAnalysisTextFileInput &input = *it;
    m_rowToInput[row++] = &input;
  }
  m_rowCount += m_inputs.size();

  endResetModel();
}

void ZAnalysisWorklistModel::setSource(const QString &filename, QTextCodec *codec)
{
  QFile src(filename);
  setSource(&src, codec);
}

void ZAnalysisWorklistModel::toCSV(QIODevice *dest, bool withHeader, QChar separator, QTextCodec *codec) const
{
  if(!dest->isOpen()) dest->open(QIODevice::WriteOnly | QIODevice::Truncate);
  QTextStream stream(dest);
  if(codec) stream.setCodec(codec);
  if(withHeader) {
    stream << m_header.join(separator) << endl;
  }

  for(size_t row = 0; row < static_cast<size_t>(rowCount()); ++row)
  {
    std::map<size_t, ZAnalysisTextFileInput*>::const_iterator it = m_rowToInput.find(row);
    if (it != m_rowToInput.end()) {
      const ZAnalysisTextFileInput* input = it->second;
      stream << input->imgFilename << separator
             << input->swcFilename << separator
             << input->punctaFilename << separator
             << input->voxelSizeX << separator
             << input->voxelSizeY << separator
             << input->voxelSizeZ << separator
             << input->dendriteChannel << separator
             << input->axonChannel << separator
             << input->maxDistToBranch << separator
             << input->bluenessExtend << separator
             << input->outputFolder << separator
             << (input->doPyramidalFunctionalSeparation ? "yes" : "no") << separator
             << (input->doPyramidalSubclassSeparation ? "yes" : "no") << separator
             << input->somaPunctaFilename
             << endl;
    }
  }
  stream << flush;
  dest->close();
}

void ZAnalysisWorklistModel::toCSV(const QString filename, bool withHeader, QChar separator, QTextCodec *codec) const
{
  QFile dest(filename);
  toCSV(&dest, withHeader, separator, codec);
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
  if(index.parent() != QModelIndex()) return QVariant();
  if(role == Qt::DisplayRole || role == Qt::EditRole || role == Qt::UserRole) {
    if(index.row() < 0 || index.column() < 0 ||
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

bool ZAnalysisWorklistModel::setData(const QModelIndex &index, const QVariant &data, int role)
{
  if (index.parent() != QModelIndex()) return false;

  if(role == Qt::DisplayRole || role == Qt::EditRole || role == Qt::UserRole) {
    if (index.row() >= rowCount() || index.column() >= columnCount() || index.row() < 0 || index.column() < 0)
      return false;
    ZAnalysisTextFileInput* input = nullptr;
    if (m_rowToInput.find(index.row()) == m_rowToInput.end()) {
      m_inputs.push_back(ZAnalysisTextFileInput());
      input = &(m_inputs[m_inputs.size()-1]);
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
  if(section < m_header.count() && orientation == Qt::Horizontal && (role == Qt::DisplayRole || role == Qt::EditRole || role == Qt::UserRole))
    return m_header[section];
  else
    return QAbstractTableModel::headerData(section, orientation, role);
}

Qt::ItemFlags ZAnalysisWorklistModel::flags(const QModelIndex &index) const
{
  return Qt::ItemIsEditable | Qt::ItemIsDropEnabled | QAbstractTableModel::flags(index);
}

void ZAnalysisWorklistModel::reset()
{
  m_header.clear();
  m_header << "# imageName" << "swcName" << "punctaName" << "voxelSizeXInUm" << "voxelSizeYInUm"
           << "voxelSizeZInUm" << "dendriteChannel" << "axonChannel(can be empty)"
           << "maxDistToBranch"  << "bluenessExtend" << "outputFolder(can be empty)"
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

bool ZAnalysisWorklistModel::dropMimeData(const QMimeData *data, Qt::DropAction action,
                                          int row, int column, const QModelIndex &parent)
{
  Q_UNUSED(row)
  Q_UNUSED(column)
  //LINFO() << row << column << action << parent.row() << parent.column();

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
