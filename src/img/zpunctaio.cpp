#include "zpunctaio.h"

#include "zpuncta.h"
#include "zeigenutils.h"
#include "zexception.h"
#include "zioutils.h"
#include "zlog.h"
#include "zstringutils.h"
#include <H5Cpp.h>
#include <QFile>
#include <QTextStream>

namespace nim {

ZPunctaIO& ZPunctaIO::instance()
{
  static ZPunctaIO punctaIO;
  return punctaIO;
}

ZPunctaIO::ZPunctaIO()
{
  m_readExts << "nimp"
             << "apo"
             << "marker"
             << "txt"
             << "xyz";
  m_readFilter = QString("All Puncta files (*.") + m_readExts.join(" *.") + QString(")");

  m_writeExts << "nimp"
              << "apo"
              << "txt";
  m_writeFormats.push_back("nimp");
  m_writeFormats.push_back("apo");
  m_writeFormats.push_back("txt");
  m_writeFilters.push_back(QString("Puncta files (*.nimp)"));
  m_writeFilters.push_back(QString("Vaa3d Apo files (*.apo)"));
  m_writeFilters.push_back(QString("n x 4 Matrix files (*.txt)"));
}

bool ZPunctaIO::canReadFile(const QString& filename)
{
  return std::any_of(m_readExts.begin(), m_readExts.end(), [&](const QString& readExt) {
    return filename.endsWith(QString(".%1").arg(readExt), Qt::CaseInsensitive);
  });
}

bool ZPunctaIO::canWriteFile(const QString& filename)
{
  return std::any_of(m_writeExts.begin(), m_writeExts.end(), [&](const QString& writeExt) {
    return filename.endsWith(QString(".%1").arg(writeExt), Qt::CaseInsensitive);
  });
}

void ZPunctaIO::getQtWriteNameFilter(QStringList& filters, QStringList& formats)
{
  filters = m_writeFilters;
  formats = m_writeFormats;
}

void ZPunctaIO::load(const QString& filename, ZPuncta& puncta) const
{
  try {
    puncta.clear();
    if (filename.endsWith(".nimp", Qt::CaseInsensitive)) {
      readNimpFile(filename, puncta);
    } else if (filename.endsWith(".apo", Qt::CaseInsensitive)) {
      readV3DApoFile(filename, puncta);
    } else if (filename.endsWith(".marker", Qt::CaseInsensitive)) {
      readV3DMarkerFile(filename, puncta);
    } else if (filename.endsWith(".txt", Qt::CaseInsensitive) || filename.endsWith(".xyz", Qt::CaseInsensitive)) {
      readMatFile(filename, puncta);
    } else {
      throw ZIOException("Not supported puncta format");
    }
  }
  catch (const ZException& e) {
    throw ZIOException(QString("Can not load puncta %1: %2").arg(filename).arg(e.what()));
  }
}

void ZPunctaIO::save(const ZPuncta& puncta, const QString& filename, QString format) const
{
  try {
    if (format.isEmpty()) {
      for (index_t i = 0; i < m_writeExts.size(); ++i) {
        if (filename.endsWith(QString(".%1").arg(m_writeExts[i]), Qt::CaseInsensitive)) {
          format = m_writeFormats[i];
        }
      }
    }
    CHECK(m_writeFormats.contains(format));

    if (format == "nimp") {
      writeNimpFile(puncta, filename);
    } else if (format == "apo") {
      writeV3DApoFile(puncta, filename);
    } else if (format == "txt") {
      writeMatFile(puncta, filename);
    }
  }
  catch (const ZException& e) {
    throw ZIOException(QString("Can not save puncta %1: %2").arg(filename).arg(e.what()));
  }
}

void ZPunctaIO::readNimpFile(const QString& filename, ZPuncta& puncta)
{
  try {
    H5::Exception::dontPrint();

    H5::H5File file(QFile::encodeName(filename).constData(), H5F_ACC_RDONLY);

    double punctaInfo[13];

    H5::FloatType doubleType(H5::PredType::IEEE_F64LE);
    H5::IntType intType(H5::PredType::STD_I32LE);
    H5::StrType strType(0, H5T_VARIABLE);

    H5::Group allGrp = file.openGroup("Puncta");
    H5::Attribute ver = allGrp.openAttribute("Version");
    int32_t punctaVer;
    ver.read(intType, &punctaVer);

    H5::Attribute numPunctaAttr = allGrp.openAttribute("Number");
    int32_t numPuncta;
    numPunctaAttr.read(intType, &numPuncta);

    for (int32_t i = 0; i < numPuncta; ++i) {
      H5::Group punctumGrp = allGrp.openGroup(fmt::format("Punctum{}", i + 1));
      ZPunctum p;

      H5std_string strBuf;

      if (H5Aexists(punctumGrp.getId(), "Name") > 0) {
        H5::Attribute name = punctumGrp.openAttribute("Name");
        name.read(strType, strBuf);
        p.setName(QString::fromStdString(strBuf));
      }
      if (H5Aexists(punctumGrp.getId(), "Comment") > 0) {
        H5::Attribute comment = punctumGrp.openAttribute("Comment");
        comment.read(strType, strBuf);
        p.setComment(QString::fromStdString(strBuf));
      }
      if (H5Aexists(punctumGrp.getId(), "Property1") > 0) {
        H5::Attribute property = punctumGrp.openAttribute("Property1");
        property.read(strType, strBuf);
        p.setProperty1(QString::fromStdString(strBuf));
      }
      if (H5Aexists(punctumGrp.getId(), "Property2") > 0) {
        H5::Attribute property = punctumGrp.openAttribute("Property2");
        property.read(strType, strBuf);
        p.setProperty2(QString::fromStdString(strBuf));
      }
      if (H5Aexists(punctumGrp.getId(), "Property3") > 0) {
        H5::Attribute property = punctumGrp.openAttribute("Property3");
        property.read(strType, strBuf);
        p.setProperty3(QString::fromStdString(strBuf));
      }

      H5::DataSet info = punctumGrp.openDataSet("Summary");
      info.read(punctaInfo, doubleType);

      p.setZ(punctaInfo[0]);
      p.setX(punctaInfo[1]);
      p.setY(punctaInfo[2]);
      p.setMaxIntensity(punctaInfo[3]);
      p.setMeanIntensity(punctaInfo[4]);
      p.setSDevOfIntensity(punctaInfo[5]);
      p.setVolSize(punctaInfo[6]);
      p.setMass(punctaInfo[7]);
      p.setRadius(punctaInfo[8]);
      p.setColor(col4(punctaInfo[9], punctaInfo[10], punctaInfo[11]));
      p.setScore(punctaInfo[12]);

      if (H5Lexists(punctumGrp.getId(), "VoxelIntensities", H5P_DEFAULT) > 0 &&
          H5Lexists(punctumGrp.getId(), "VoxelLocations", H5P_DEFAULT) > 0) {
        H5::DataSet voxelInten = punctumGrp.openDataSet("VoxelIntensities");
        H5::DataSet voxelList = punctumGrp.openDataSet("VoxelLocations");
        H5::DataSpace voxelListDataspace = voxelList.getSpace();
        H5::DataSpace voxelIntenDataspace = voxelInten.getSpace();

        if (voxelListDataspace.getSimpleExtentNdims() != 2 || voxelIntenDataspace.getSimpleExtentNdims() != 1) {
          throw ZIOException("Wrong puncta file contents");
        }

        hsize_t voxelListDim[2];
        hsize_t voxelIntenDim;

        voxelListDataspace.getSimpleExtentDims(voxelListDim, nullptr);
        voxelIntenDataspace.getSimpleExtentDims(&voxelIntenDim, nullptr);

        if (voxelListDim[1] != 3 || voxelListDim[0] != voxelIntenDim) {
          throw ZIOException("Wrong puncta file contents");
        }

        Eigen::MatrixXi voxelLocations(voxelIntenDim, 3);
        Eigen::VectorXd voxelIntens(voxelIntenDim);

        voxelInten.read(voxelIntens.data(), doubleType);
        voxelList.read(voxelLocations.data(), intType);

        p.setVoxelIntensities(voxelIntens);
        p.setVoxelLocations(voxelLocations);
      }

      puncta.data.push_back(std::move(p));
    }
  }
  catch (const H5::Exception& e) {
    throw ZIOException(fmt::format("hdf5:{}", e.getDetailMsg()));
  }
}

void ZPunctaIO::writeNimpFile(const ZPuncta& puncta, const QString& filename)
{
  try {
    H5::Exception::dontPrint();

    H5::H5File file(QFile::encodeName(filename).constData(), H5F_ACC_TRUNC);

    double punctaInfo[13];
    hsize_t infoDim = 13;
    H5::DataSpace infoDataspace(1, &infoDim);

    hsize_t voxelListDim[2];
    voxelListDim[1] = 3;
    hsize_t voxelIntenDim;

    H5::FloatType doubleType(H5::PredType::IEEE_F64LE);
    H5::IntType intType(H5::PredType::STD_I32LE);
    H5::StrType strType(0, H5T_VARIABLE);

    H5::DataSpace attrDataSpace(H5S_SCALAR);

    H5::Group allGrp = file.createGroup("Puncta");

    H5::Attribute ver = allGrp.createAttribute("Version", intType, attrDataSpace);
    int32_t punctaVer = 100;
    ver.write(intType, &punctaVer);

    int32_t idx = 0;
    for (const auto& p : puncta.data) {
      H5::Group punctumGrp = allGrp.createGroup(fmt::format("Punctum{}", idx + 1));
      ++idx;

      if (!p.name().isEmpty()) {
        H5::Attribute name = punctumGrp.createAttribute("Name", strType, attrDataSpace);
        name.write(strType, p.name().toStdString());
      }
      if (!p.comment().isEmpty()) {
        H5::Attribute comment = punctumGrp.createAttribute("Comment", strType, attrDataSpace);
        comment.write(strType, p.comment().toStdString());
      }
      if (!p.property1().isEmpty()) {
        H5::Attribute property = punctumGrp.createAttribute("Property1", strType, attrDataSpace);
        property.write(strType, p.property1().toStdString());
      }
      if (!p.property2().isEmpty()) {
        H5::Attribute property = punctumGrp.createAttribute("Property2", strType, attrDataSpace);
        property.write(strType, p.property2().toStdString());
      }
      if (!p.property3().isEmpty()) {
        H5::Attribute property = punctumGrp.createAttribute("Property3", strType, attrDataSpace);
        property.write(strType, p.property3().toStdString());
      }

      punctaInfo[0] = p.z();
      punctaInfo[1] = p.x();
      punctaInfo[2] = p.y();
      punctaInfo[3] = p.maxIntensity();
      punctaInfo[4] = p.meanIntensity();
      punctaInfo[5] = p.sDevOfIntensity();
      punctaInfo[6] = p.volSize();
      punctaInfo[7] = p.mass();
      punctaInfo[8] = p.radius();
      punctaInfo[9] = p.color().r;
      punctaInfo[10] = p.color().g;
      punctaInfo[11] = p.color().b;
      punctaInfo[12] = p.score();

      H5::DataSet info = punctumGrp.createDataSet("Summary", doubleType, infoDataspace);
      info.write(punctaInfo, doubleType);

      if (p.voxelLocations().rows() != 0) {
        voxelIntenDim = p.voxelLocations().rows();
        voxelListDim[0] = voxelIntenDim;

        H5::DataSpace voxelListDataspace(2, voxelListDim);
        H5::DataSet voxelList = punctumGrp.createDataSet("VoxelLocations", intType, voxelListDataspace);
        voxelList.write(p.voxelLocations().data(), intType);

        H5::DataSpace voxelIntenDataspace(1, &voxelIntenDim);
        H5::DataSet voxelInten = punctumGrp.createDataSet("VoxelIntensities", doubleType, voxelIntenDataspace);
        voxelInten.write(p.voxelIntensities().data(), doubleType);
      }
    }

    H5::Attribute numPunctaAttr = allGrp.createAttribute("Number", intType, attrDataSpace);
    numPunctaAttr.write(intType, &idx);
  }
  catch (const H5::Exception& e) {
    QFile::remove(filename);
    throw ZIOException(fmt::format("hdf5:{}", e.getDetailMsg()));
  }
}

void ZPunctaIO::readV3DApoFile(const QString& file, ZPuncta& puncta)
{
  QFile qFile(file);
  if (!qFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
    throw ZIOException("Can not read file.");
  }

  QTextStream stream(&qFile);
#if (QT_VERSION < QT_VERSION_CHECK(6, 0, 0))
  stream.setCodec("UTF-8");
#endif
  while (!stream.atEnd()) {
    QString line = stream.readLine().trimmed();
    if (stream.status() != QTextStream::Ok) {
      throw ZIOException("Error while reading file.");
    }
    removeComment(line, QString("#"), true);
#if (QT_VERSION >= QT_VERSION_CHECK(5, 14, 0))
    QStringList fieldList = line.split(",", Qt::KeepEmptyParts);
#else
    QStringList fieldList = line.split(",", QString::KeepEmptyParts);
#endif
    if (fieldList.size() >= 12) {
      ZPunctum punctum;
      bool ok;
      fieldList[0].toInt(&ok);
      if (!ok) {
        throw ZIOException(QString("Wrong Vaa3d Apo format: %1.").arg(line));
      }
      punctum.setName(fieldList[2]);
      punctum.setComment(fieldList[3]);
      punctum.setZ(fieldList[4].toDouble(&ok));
      if (!ok) {
        throw ZIOException(QString("Wrong Vaa3d Apo format: %1.").arg(line));
      }
      punctum.setX(fieldList[5].toDouble(&ok));
      if (!ok) {
        throw ZIOException(QString("Wrong Vaa3d Apo format: %1.").arg(line));
      }
      punctum.setY(fieldList[6].toDouble(&ok));
      if (!ok) {
        throw ZIOException(QString("Wrong Vaa3d Apo format: %1.").arg(line));
      }
      punctum.setMaxIntensity(fieldList[7].toDouble(&ok));
      if (!ok) {
        throw ZIOException(QString("Wrong Vaa3d Apo format: %1.").arg(line));
      }
      punctum.setMeanIntensity(fieldList[8].toDouble(&ok));
      if (!ok) {
        throw ZIOException(QString("Wrong Vaa3d Apo format: %1.").arg(line));
      }
      punctum.setSDevOfIntensity(fieldList[9].toDouble(&ok));
      if (!ok) {
        throw ZIOException(QString("Wrong Vaa3d Apo format: %1.").arg(line));
      }
      punctum.setVolSize(fieldList[10].toDouble(&ok));
      if (!ok) {
        throw ZIOException(QString("Wrong Vaa3d Apo format: %1.").arg(line));
      }
      punctum.setMass(fieldList[11].toDouble(&ok));
      if (!ok) {
        throw ZIOException(QString("Wrong Vaa3d Apo format: %1.").arg(line));
      }
      if (fieldList.size() > 12) {
        punctum.setProperty1(fieldList[12]);
      }
      if (fieldList.size() > 13) {
        punctum.setProperty2(fieldList[13]);
      }
      if (fieldList.size() > 14) {
        punctum.setProperty3(fieldList[14]);
      }
      if (fieldList.size() >= 18) {
        bool ok1, ok2;

        punctum.setColor(col4(fieldList[15].toInt(&ok), fieldList[16].toInt(&ok1), fieldList[17].toInt(&ok2)));
        if (!ok || !ok1 || !ok2) {
          if (fieldList[15].isEmpty() && fieldList[16].isEmpty() && fieldList[17].isEmpty()) {
            punctum.setColor(col4(0, 0, 0));
          } else {
            throw ZIOException(QString("Wrong Vaa3d Apo format: %1.").arg(line));
          }
        }
      }
      using namespace boost::math::double_constants;
      punctum.setRadius(std::pow(three_quarters_pi * punctum.volSize(), 1.0 / 3));
      puncta.data.push_back(std::move(punctum));
    } else if (!line.isEmpty()) {
      throw ZIOException(QString("Wrong Vaa3d Apo format: %1.").arg(line));
    }
  }
}

void ZPunctaIO::writeV3DApoFile(const ZPuncta& puncta, const QString& file)
{
  QFile qFile(file);
  if (!qFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
    throw ZIOException("Can not open file.");
  }

  QTextStream out(&qFile);
#if (QT_VERSION < QT_VERSION_CHECK(6, 0, 0))
  out.setCodec("UTF-8");
#endif
  size_t idx = 0;
  out
    << "#id, , name, comment, z, x, y, maxIntensity, meanIntensity, sDevOfIntensity, volSize, mass, property1, property2, property3, red, green, blue\n";
  if (out.status() != QTextStream::Ok) {
    throw ZIOException("Error while writing file.");
  }
  for (const auto& pun : puncta.data) {
    out << QString("%1,,%2,%3,%4,%5,%6,%7,%8,%9,%10,%11,%12,%13,%14,%15,%16,%17\n")
             .arg(idx + 1)
             .arg(pun.name())
             .arg(pun.comment())
             .arg(pun.z())
             .arg(pun.x())
             .arg(pun.y())
             .arg(pun.maxIntensity())
             .arg(pun.meanIntensity())
             .arg(pun.sDevOfIntensity())
             .arg(pun.volSize())
             .arg(pun.mass())
             .arg(pun.property1())
             .arg(pun.property2())
             .arg(pun.property3())
             .arg(pun.color().r)
             .arg(pun.color().g)
             .arg(pun.color().b);
    ++idx;
    if (out.status() != QTextStream::Ok) {
      throw ZIOException("Error while writing file.");
    }
  }
}

void ZPunctaIO::readV3DMarkerFile(const QString& file, ZPuncta& puncta)
{
  QFile qFile(file);
  if (!qFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
    throw ZIOException("Can not read file.");
  }

  QTextStream stream(&qFile);
#if (QT_VERSION < QT_VERSION_CHECK(6, 0, 0))
  stream.setCodec("UTF-8");
#endif
  while (!stream.atEnd()) {
    QString line = stream.readLine().trimmed();
    if (stream.status() != QTextStream::Ok) {
      throw ZIOException("Error while reading file.");
    }
    removeComment(line, QString("#"), true);
#if (QT_VERSION >= QT_VERSION_CHECK(5, 14, 0))
    QStringList fieldList = line.split(",", Qt::KeepEmptyParts);
#else
    QStringList fieldList = line.split(",", QString::KeepEmptyParts);
#endif
    if (fieldList.size() >= 10) {
      ZPunctum punctum;
      bool ok;
      punctum.setX(fieldList[0].toDouble(&ok));
      if (!ok) {
        throw ZIOException(QString("Wrong Vaa3d Marker format: %1.").arg(line));
      }
      punctum.setY(fieldList[1].toDouble(&ok));
      if (!ok) {
        throw ZIOException(QString("Wrong Vaa3d Marker format: %1.").arg(line));
      }
      punctum.setZ(fieldList[2].toDouble(&ok));
      if (!ok) {
        throw ZIOException(QString("Wrong Vaa3d Marker format: %1.").arg(line));
      }
      punctum.setRadius(fieldList[3].toDouble(&ok));
      if (!ok) {
        throw ZIOException(QString("Wrong Vaa3d Marker format: %1.").arg(line));
      }
      if (punctum.radius() <= 0) {
        punctum.setRadius(2.0);
      }

      punctum.updateVolSize();
      punctum.setMeanIntensity(1);
      punctum.updateMass();

      punctum.setName(fieldList[5]);
      punctum.setComment(fieldList[6]);

      bool ok1, ok2;

      punctum.setColor(col4(fieldList[7].toInt(&ok), fieldList[8].toInt(&ok1), fieldList[9].toInt(&ok2)));
      if (!ok || !ok1 || !ok2) {
        if (fieldList[7].isEmpty() && fieldList[8].isEmpty() && fieldList[9].isEmpty()) {
          punctum.setColor(col4(0, 0, 0));
        } else {
          throw ZIOException(QString("Wrong Vaa3d Marker format: %1.").arg(line));
        }
      }
      puncta.data.push_back(std::move(punctum));
    } else if (!line.isEmpty()) {
      throw ZIOException(QString("Wrong Vaa3d Marker format: %1.").arg(line));
    }
  }
}

void ZPunctaIO::readMatFile(const QString& file, ZPuncta& puncta)
{
  Eigen::MatrixXd mat = ZEigenUtils::readMatrix(file, "", false, 0, "#");
  mat = ZEigenUtils::removeRowsContainNaNOrInF(mat);
  if (mat.rows() > 0 && mat.cols() == 2) {
    for (Eigen::Index i = 0; i < mat.rows(); ++i) {
      puncta.data.emplace_back(mat(i, 0), mat(i, 1), 0.0, 2);
    }
  } else if (mat.rows() > 0 && mat.cols() == 3) {
    for (Eigen::Index i = 0; i < mat.rows(); ++i) {
      puncta.data.emplace_back(mat(i, 0), mat(i, 1), mat(i, 2), 2);
    }
  } else if (mat.rows() > 0 && mat.cols() == 4) {
    for (Eigen::Index i = 0; i < mat.rows(); ++i) {
      puncta.data.emplace_back(mat(i, 0), mat(i, 1), mat(i, 2), mat(i, 3));
    }
  } else {
    throw ZIOException("file is not nx2 or nx3 or nx4 matrix");
  }
}

void ZPunctaIO::writeMatFile(const ZPuncta& puncta, const QString& file)
{
  QFile qFile(file);
  if (!qFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
    throw ZIOException("Can not open file.");
  }

  QTextStream out(&qFile);
#if (QT_VERSION < QT_VERSION_CHECK(6, 0, 0))
  out.setCodec("UTF-8");
#endif
  out << "# x y z radius\n";
  if (out.status() != QTextStream::Ok) {
    throw ZIOException("Error while writing file.");
  }
  for (const auto& pun : puncta.data) {
    out << QString("%1 %2 %3 %4\n").arg(pun.x()).arg(pun.y()).arg(pun.z()).arg(pun.radius());
    if (out.status() != QTextStream::Ok) {
      throw ZIOException("Error while writing file.");
    }
  }
}

} // namespace nim
