#include "zpunctaio.h"

#include "zpuncta.h"
#include "zeigenutils.h"
#include "zexception.h"
#include "zioutils.h"
#include "zlog.h"
#include "zstringutils.h"
#include <H5Cpp.h>
#include <QFile>

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
      throw ZException("Not supported puncta format");
    }
  }
  catch (const ZException& e) {
    throw ZException(fmt::format("Can not load puncta {}: {}", filename, e.what()));
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
    throw ZException(fmt::format("Can not save puncta {}: {}", filename, e.what()));
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

      if (H5Aexists(punctumGrp.getId(), "Name") > 0) {
        H5::Attribute name = punctumGrp.openAttribute("Name");
        name.read(strType, p.name);
      }
      if (H5Aexists(punctumGrp.getId(), "Comment") > 0) {
        H5::Attribute comment = punctumGrp.openAttribute("Comment");
        comment.read(strType, p.comment);
      }
      if (H5Aexists(punctumGrp.getId(), "Property1") > 0) {
        H5::Attribute property = punctumGrp.openAttribute("Property1");
        property.read(strType, p.property1);
      }
      if (H5Aexists(punctumGrp.getId(), "Property2") > 0) {
        H5::Attribute property = punctumGrp.openAttribute("Property2");
        property.read(strType, p.property2);
      }
      if (H5Aexists(punctumGrp.getId(), "Property3") > 0) {
        H5::Attribute property = punctumGrp.openAttribute("Property3");
        property.read(strType, p.property3);
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
      p.setColor(col4{static_cast<col4::value_type>(punctaInfo[9]),
                      static_cast<col4::value_type>(punctaInfo[10]),
                      static_cast<col4::value_type>(punctaInfo[11])});
      p.setScore(punctaInfo[12]);

      if (H5Lexists(punctumGrp.getId(), "VoxelIntensities", H5P_DEFAULT) > 0 &&
          H5Lexists(punctumGrp.getId(), "VoxelLocations", H5P_DEFAULT) > 0) {
        H5::DataSet voxelInten = punctumGrp.openDataSet("VoxelIntensities");
        H5::DataSet voxelList = punctumGrp.openDataSet("VoxelLocations");
        H5::DataSpace voxelListDataspace = voxelList.getSpace();
        H5::DataSpace voxelIntenDataspace = voxelInten.getSpace();

        if (voxelListDataspace.getSimpleExtentNdims() != 2 || voxelIntenDataspace.getSimpleExtentNdims() != 1) {
          throw ZException("Wrong puncta file contents");
        }

        hsize_t voxelListDim[2];
        hsize_t voxelIntenDim;

        voxelListDataspace.getSimpleExtentDims(voxelListDim, nullptr);
        voxelIntenDataspace.getSimpleExtentDims(&voxelIntenDim, nullptr);

        if (voxelListDim[1] != 3 || voxelListDim[0] != voxelIntenDim) {
          throw ZException("Wrong puncta file contents");
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
    throw ZException(fmt::format("hdf5:{}", e.getDetailMsg()), ZException::Option::CheckErrno);
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

      if (!p.name.empty()) {
        H5::Attribute name = punctumGrp.createAttribute("Name", strType, attrDataSpace);
        name.write(strType, p.name);
      }
      if (!p.comment.empty()) {
        H5::Attribute comment = punctumGrp.createAttribute("Comment", strType, attrDataSpace);
        comment.write(strType, p.comment);
      }
      if (!p.property1.empty()) {
        H5::Attribute property = punctumGrp.createAttribute("Property1", strType, attrDataSpace);
        property.write(strType, p.property1);
      }
      if (!p.property2.empty()) {
        H5::Attribute property = punctumGrp.createAttribute("Property2", strType, attrDataSpace);
        property.write(strType, p.property2);
      }
      if (!p.property3.empty()) {
        H5::Attribute property = punctumGrp.createAttribute("Property3", strType, attrDataSpace);
        property.write(strType, p.property3);
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
    throw ZException(fmt::format("hdf5:{}", e.getDetailMsg()), ZException::Option::CheckErrno);
  }
}

void ZPunctaIO::readV3DApoFile(const QString& filename, ZPuncta& puncta)
{
  std::ifstream file = openIFStream(filename, std::ios_base::in);
  if (!file) {
    throw ZException("Can not open file", ZException::Option::CheckErrno);
  }

  std::string line;
  while (std::getline(file, line)) {
    auto cleanLineView = absl::StripAsciiWhitespace(removeComment(line));
    std::vector<std::string_view> fieldList = absl::StrSplit(cleanLineView, absl::ByChar(','));
    if (fieldList.size() >= 12) {
      for (auto& field : fieldList) {
        field = absl::StripAsciiWhitespace(field);
      }
      ZPunctum punctum;
      punctum.name = fieldList[2];
      punctum.comment = fieldList[3];
      stringToValue(fieldList[4], punctum.m_z);
      stringToValue(fieldList[5], punctum.m_x);
      stringToValue(fieldList[6], punctum.m_y);
      stringToValue(fieldList[7], punctum.m_maxIntensity);
      stringToValue(fieldList[8], punctum.m_meanIntensity);
      stringToValue(fieldList[9], punctum.m_sDevOfIntensity);
      stringToValue(fieldList[10], punctum.m_volSize);
      stringToValue(fieldList[11], punctum.m_mass);
      if (fieldList.size() > 12) {
        punctum.property1 = fieldList[12];
      }
      if (fieldList.size() > 13) {
        punctum.property2 = fieldList[13];
      }
      if (fieldList.size() > 14) {
        punctum.property3 = fieldList[14];
      }
      if (fieldList.size() >= 18) {
        stringToValue(fieldList[15], punctum.m_color.r);
        stringToValue(fieldList[16], punctum.m_color.g);
        stringToValue(fieldList[17], punctum.m_color.b);
      }
      using namespace boost::math::double_constants;
      punctum.setRadius(std::pow(three_quarters_pi * punctum.volSize(), 1.0 / 3));
      puncta.data.push_back(std::move(punctum));
    } else if (!cleanLineView.empty()) {
      throw ZException(fmt::format("Wrong Vaa3d Apo format: {}", line));
    }
    if (fieldList.size() > 18) {
      LOG(WARNING) << "Potential error in Vaa3d Apo file: " << line;
    }
  }

  if (!file.eof()) {
    throw ZException("Error while reading file", ZException::Option::CheckErrno);
  }
}

void ZPunctaIO::writeV3DApoFile(const ZPuncta& puncta, const QString& file)
{
  auto of = openOFStream(file, std::ios_base::out);

  of << "#id, , name, comment, z, x, y, maxIntensity, meanIntensity, sDevOfIntensity, volSize, mass, property1, "
        "property2, property3, red, green, blue\n";
  if (!of) {
    throw ZException("Error while writing file header", ZException::Option::CheckErrno);
  }
  size_t idx = 1;
  for (const auto& pun : puncta.data) {
    of << fmt::format("{},,{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{}\n",
                      idx,
                      pun.name,
                      pun.comment,
                      pun.z(),
                      pun.x(),
                      pun.y(),
                      pun.maxIntensity(),
                      pun.meanIntensity(),
                      pun.sDevOfIntensity(),
                      pun.volSize(),
                      pun.mass(),
                      pun.property1,
                      pun.property2,
                      pun.property3,
                      pun.color().r,
                      pun.color().g,
                      pun.color().b);
    ++idx;
    if (!of) {
      throw ZException("Error while writing file", ZException::Option::CheckErrno);
    }
  }
}

void ZPunctaIO::readV3DMarkerFile(const QString& filename, ZPuncta& puncta)
{
  std::ifstream file = openIFStream(filename, std::ios_base::in);
  if (!file) {
    throw ZException("Can not open file", ZException::Option::CheckErrno);
  }

  std::string line;
  while (std::getline(file, line)) {
    auto cleanLineView = absl::StripAsciiWhitespace(removeComment(line));
    std::vector<std::string_view> fieldList = absl::StrSplit(cleanLineView, absl::ByChar(','));
    if (fieldList.size() >= 10) {
      for (auto& field : fieldList) {
        field = absl::StripAsciiWhitespace(field);
      }
      ZPunctum punctum;
      stringToValue(fieldList[0], punctum.m_x);
      stringToValue(fieldList[1], punctum.m_y);
      stringToValue(fieldList[2], punctum.m_z);
      stringToValue(fieldList[3], punctum.m_radius);
      if (punctum.radius() <= 0) {
        punctum.setRadius(2.0);
      }

      punctum.updateVolSize();
      punctum.setMeanIntensity(1);
      punctum.updateMass();

      punctum.name = fieldList[5];
      punctum.comment = fieldList[6];
      stringToValue(fieldList[7], punctum.m_color.r);
      stringToValue(fieldList[8], punctum.m_color.g);
      stringToValue(fieldList[9], punctum.m_color.b);

      puncta.data.push_back(std::move(punctum));
    } else if (!cleanLineView.empty()) {
      throw ZException(fmt::format("Wrong Vaa3d Marker format: {}", line));
    }
    if (fieldList.size() > 10) {
      LOG(WARNING) << "Potential error in Vaa3d Marker file: " << line;
    }
  }

  if (!file.eof()) {
    throw ZException("Error while reading file", ZException::Option::CheckErrno);
  }
}

void ZPunctaIO::readMatFile(const QString& file, ZPuncta& puncta)
{
  Eigen::MatrixXd mat = ZEigenUtils::readMatrix(file, 0);
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
    throw ZException("file is not nx2 or nx3 or nx4 matrix");
  }
}

void ZPunctaIO::writeMatFile(const ZPuncta& puncta, const QString& file)
{
  auto of = openOFStream(file, std::ios_base::out);
  of << "# x y z radius\n";
  if (!of) {
    throw ZException("Error while writing file header", ZException::Option::CheckErrno);
  }
  for (const auto& pun : puncta.data) {
    of << fmt::format("{} {} {} {}\n", pun.x(), pun.y(), pun.z(), pun.radius());
    if (!of) {
      throw ZException("Error while writing file", ZException::Option::CheckErrno);
    }
  }
}

} // namespace nim
