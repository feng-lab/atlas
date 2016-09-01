#include "zregionannotation.h"

#include "zexception.h"
#include "zioutils.h"
#include "zimgconnectedcomponents.h"
#include "zlog.h"
#include "zimgfillhole.h"
#include "zimgsigneddistancemap.h"
#include "zbenchtimer.h"
#include <QStandardPaths>
#include <QFile>
#include <QTemporaryDir>

namespace {

struct ChangeValue
{
  ChangeValue(int64_t from_, int64_t to_)
    : from(from_), to(to_)
  {}

  template<typename TVoxel>
  TVoxel operator()(TVoxel current) const
  {
    return (static_cast<int64_t>(current) == from) ? static_cast<TVoxel>(to) : current;
  }

  int64_t from;
  int64_t to;
};

struct MarkAsIfOtherEqualsOtherWiseZero
{
  MarkAsIfOtherEqualsOtherWiseZero(int64_t as_, int64_t equal_)
    : as(as_), equal(equal_)
  {}

  template<typename TVoxel, typename TVoxelOther>
  TVoxel operator()(TVoxel /*unused*/, TVoxelOther otherVoxel) const
  {
    return (static_cast<int64_t>(otherVoxel) == equal) ? as : 0;
  }

  int64_t as;
  int64_t equal;
};

struct CopyAsIfOtherIsNotZero
{
  explicit CopyAsIfOtherIsNotZero(int64_t as_)
    : as(as_)
  {}

  template<typename TVoxel, typename TVoxelOther>
  TVoxel operator()(TVoxel current, TVoxelOther otherVoxel) const
  {
    return (otherVoxel != 0) ? static_cast<TVoxel>(as) : current;
  }

  int64_t as;
};

} // namespace

namespace nim {

ZRegionAnnotation::ZRegionAnnotation(QObject* parent)
  : QObject(parent)
{
  clear();

  QStringList regions;
  regions << "GPe" << "STN" << "SNr" << "STRv" << "STRd" << "GPi" << "SPF";
  readMouseBrainAtlasOntology(regions, m_ontology);
  connect(&m_undoStack, &QUndoStack::cleanChanged,
          this, &ZRegionAnnotation::undoStackCleanChanged);
}

ZRegionAnnotation::ZRegionAnnotation(const QString& filename, QObject* parent)
  : QObject(parent)
{
  load(filename);
  connect(&m_undoStack, &QUndoStack::cleanChanged,
          this, &ZRegionAnnotation::undoStackCleanChanged);
}

ZRegionAnnotation::~ZRegionAnnotation()
{
  clear();
}

void ZRegionAnnotation::clear()
{
  m_width = -1;
  m_height = -1;
  m_depth = -1;
  m_voxelSizeX = 1;
  m_voxelSizeX = 1;
  m_voxelSizeX = 1;
  m_ontology.clear();
  m_boundBox.clear();
  m_boundBox.resize(8);
  updateBoundBox();
}

void ZRegionAnnotation::importLabelImage(const QString& fn, FileFormat format, bool createMesh, bool createROI)
{
  ZBenchTimer bt;
  bt.start();

  std::vector<ZImgInfo> infos = ZImg::readImgInfo(fn, nullptr, format);
  if (infos.size() > 1) {
    throw ZIOException("label image with more than one scene is not supported");
  }
  ZImgInfo info = infos[0];
  if (info.isEmpty()) {
    throw ZIOException("label image is empty");
  }
  if (info.voxelFormat == VoxelFormat::Float) {
    throw ZIOException("label image can not be a floating point image");
  }
  if (info.voxelFormat == VoxelFormat::Unsigned && info.bytesPerVoxel == 8) {
    throw ZIOException("uint64 label image is not supported");
  }
  if (info.numChannels > 1 || info.numTimes > 1) {
    throw ZIOException("label image can not be time sequence or color image");
  }

  ZImg origLabelImg(fn, ZImgRegion(), 0, format);
  //LOG(INFO) << origLabelImg.info().toQString();
  m_width = origLabelImg.width();
  m_height = origLabelImg.height();
  m_depth = origLabelImg.depth();
  // todo: ask user if voxel size not exist
  m_voxelSizeX = origLabelImg.voxelSizeXInUm();
  m_voxelSizeY = origLabelImg.voxelSizeYInUm();
  m_voxelSizeZ = origLabelImg.voxelSizeZInUm();
  updateBoundBox();

  for (auto it = m_ontology.beginPost(); it != m_ontology.endPost(); ++it) {
    if (createMesh) {
      it->mesh.reset();
    }
    if (createROI) {
      it->roi.reset();
    }
  }

  std::set<int64_t> labels;
  for (size_t i = 0; i < origLabelImg.channelVoxelNumber(); ++i) {
    labels.insert(origLabelImg.value<int64_t>(i));
  }
  for (auto it = m_ontology.cbeginBreadthFirst(); it != m_ontology.cendBreadthFirst(); ++it) {
    if (labels.find(it->id) != labels.end()) {
      for (auto pit = m_ontology.cbeginAncestor(it);
           pit != m_ontology.cendAncestor(it); ++pit) {
        labels.insert(pit->id);
      }
    }
  }

  ZImg labelImg = origLabelImg;
  info.setVoxelFormat<uint8_t>();
  ZImg binaryImg(info);
  int64_t maxPossibleLabelInImg = origLabelImg.dataRangeMax<int64_t>();
  int64_t minPossibleLabelInImg = origLabelImg.dataRangeMin<int64_t>();
  ZImgFillHole<> imFill;
  imFill.setFullyConnected(true);
  imFill.setForegroundValue(1);

  LOG(INFO) << "Importing Label Image...";
  for (auto it = m_ontology.beginPost(); it != m_ontology.endPost(); ++it) {
    LOG(INFO) << "Processing region " << it->abbreviation << " " << it->id << "...";
    if (it->id > maxPossibleLabelInImg || it->id < minPossibleLabelInImg ||
        labels.find(it->id) == labels.end()) {
      continue;
    }

    // create binary image
    binaryImg.binaryOperation(labelImg, MarkAsIfOtherEqualsOtherWiseZero(1, it->id));
    if (it->id == 997) {
      for (size_t z = 0; z < binaryImg.depth(); ++z) {
        ZImg simg = binaryImg.createView(z, 0, 0);
        ZImg fimg = imFill.run(simg);
        binaryImg.pasteImg(fimg, ZVoxelCoordinate(0, 0, z));
      }
    }
    if (createMesh) {
      // create mesh
      it->mesh = std::make_shared<ZMesh>();
      binaryImgToMesh(binaryImg, *it->mesh.get());
    }
    if (createROI) {
      // create contours
      it->roi = std::make_shared<ZROI>(undoStack());
      binaryImgToROI(binaryImg, *it->roi.get());
    }

    // update labelImg: change id to parentID (merge current region to parent region)
    if (!m_ontology.isRoot(it)) {
      int64_t parentID = it->parentID;
      auto pit = m_ontology.parent(it);
      while ((parentID > maxPossibleLabelInImg || parentID < minPossibleLabelInImg) &&
             !m_ontology.isRoot(pit)) {
        parentID = pit->parentID;
        pit = m_ontology.parent(pit);
      }
      if (parentID <= maxPossibleLabelInImg && parentID >= minPossibleLabelInImg) {
        labelImg.unaryOperation(ChangeValue(it->id, parentID));
      }
    }
  }
  for (auto it = m_ontology.beginRoot(); it != m_ontology.endRoot(); ++it) {
    if (it->abbreviation.compare("STRv", Qt::CaseInsensitive) == 0 ||
        it->abbreviation.compare("STRd", Qt::CaseInsensitive) == 0) {
      m_ontology.eraseChildren(it);
    }
  }
  LOG(INFO) << "Finish importing label image";

  STOP_AND_LOG(bt);

  if (createMesh) {
    emit allMeshChanged();
  }
  if (createROI) {
    emit allROIChanged();
  }
}

void ZRegionAnnotation::exportLabelImage(const QString& fn, FileFormat format, Compression comp) const
{
  LOG(INFO) << "Exporting Label Image...";
  ZImgInfo info(m_width, m_height, m_depth, 1, 1, 2);
  info.voxelSizeUnit = VoxelSizeUnit::um;
  info.voxelSizeX = m_voxelSizeX;
  info.voxelSizeY = m_voxelSizeY;
  info.voxelSizeZ = m_voxelSizeZ;
  ZImg res(info);
  for (auto it = m_ontology.cbeginBreadthFirst(); it != m_ontology.cendBreadthFirst(); ++it) {
    LOG(INFO) << "Processing region " << it->abbreviation << " " << it->id << "...";
    if (it->roi) {
      ZImg regionBinaryImg = it->roi->toMaskImg(res.width(), res.height(), res.depth(), false);
      res.binaryOperation(regionBinaryImg, CopyAsIfOtherIsNotZero(it->id));
    }
  }
  for (auto it = m_ontology.cbeginBreadthFirst(); it != m_ontology.cendBreadthFirst(); ++it) {
    if (it->abbreviation.compare("GPe", Qt::CaseInsensitive) == 0 ||
        it->abbreviation.compare("STN", Qt::CaseInsensitive) == 0) {
      LOG(INFO) << "Post Processing Region " << it->abbreviation << " " << it->id << "...";
      if (it->roi) {
        ZImg regionBinaryImg = it->roi->toMaskImg(res.width(), res.height(), res.depth(), false);
        res.binaryOperation(regionBinaryImg, CopyAsIfOtherIsNotZero(it->id));
      }
    }
  }
  res.save(fn, format, comp);
  LOG(INFO) << "Finish exporting label image";
}

void ZRegionAnnotation::mergeROIToRegion(const ZROI& roi, int64_t regionID)
{
  for (auto it = m_ontology.begin(); it != m_ontology.end(); ++it) {
    if (it->id == regionID) {
      if (!it->roi) {
        it->roi = std::make_shared<ZROI>(undoStack());
        emit regionROIAdded(it->id, it->roi.get());
      }
      it->roi->mergeWith(roi);

      for (auto pit = m_ontology.beginAncestor(it); pit != m_ontology.endAncestor(it); ++pit) {
        if (!pit->roi) {
          pit->roi = std::make_shared<ZROI>(undoStack());
          emit regionROIAdded(it->id, it->roi.get());
        }
        pit->roi->mergeWith(roi);
      }

      return;
    }
  }
}

const ZMesh* ZRegionAnnotation::meshOfRegion(int64_t regionID)
{
  for (const auto& node : m_ontology) {
    if (node.id == regionID) {
      return node.mesh.get();
    }
  }
  return nullptr;
}

const ZROI* ZRegionAnnotation::roiOfRegion(int64_t regionID)
{
  for (const auto& node : m_ontology) {
    if (node.id == regionID) {
      return node.roi.get();
    }
  }
  return nullptr;
}

void ZRegionAnnotation::load(const QString& filename)
{
  clear();

  try {
    H5::Exception::dontPrint();

    H5::H5File file(QFile::encodeName(filename).constData(), H5F_ACC_RDONLY);

    H5::Group allGrp = file.openGroup("RegionAnnotation");

    H5::IntType intType(H5::PredType::STD_I32LE);
    H5::IntType int64Type(H5::PredType::STD_I64LE);
    H5::FloatType doubleType(H5::PredType::IEEE_F64LE);
    H5::StrType strType(0, H5T_VARIABLE);

    H5::Attribute ver = allGrp.openAttribute("Version");
    int regionAnnotationVer;
    ver.read(intType, &regionAnnotationVer);

    allGrp.openAttribute("Width").read(intType, &m_width);
    allGrp.openAttribute("Height").read(intType, &m_height);
    allGrp.openAttribute("Depth").read(intType, &m_depth);
    allGrp.openAttribute("VoxelSizeXInUM").read(doubleType, &m_voxelSizeX);
    allGrp.openAttribute("VoxelSizeYInUM").read(doubleType, &m_voxelSizeY);
    allGrp.openAttribute("VoxelSizeZInUM").read(doubleType, &m_voxelSizeZ);
    updateBoundBox();

    H5::Attribute numRegionAttr = allGrp.openAttribute("RegionNumber");
    int numRegion;
    numRegionAttr.read(intType, &numRegion);

    std::map<int64_t, RegionNode> nodeMap;
    for (int i = 0; i < numRegion; ++i) {
      H5::Group regionGrp = allGrp.openGroup(qUtf8Printable(QString("Region%1").arg(i + 1)));

      RegionNode p;

      H5::Attribute idAttr = regionGrp.openAttribute("ID");
      idAttr.read(int64Type, &p.id);

      H5::Attribute parentIDAttr = regionGrp.openAttribute("ParentID");
      parentIDAttr.read(int64Type, &p.parentID);

      H5::Attribute redAttr = regionGrp.openAttribute("Red");
      redAttr.read(intType, &p.red);

      H5::Attribute greenAttr = regionGrp.openAttribute("Green");
      greenAttr.read(intType, &p.green);

      H5::Attribute blueAttr = regionGrp.openAttribute("Blue");
      blueAttr.read(intType, &p.blue);

      H5std_string strBuf;

      H5::Attribute nameAttr = regionGrp.openAttribute("Name");
      nameAttr.read(strType, strBuf);
      p.name = QString::fromStdString(strBuf);

      H5::Attribute abbreviationAttr = regionGrp.openAttribute("Abbreviation");
      abbreviationAttr.read(strType, strBuf);
      p.abbreviation = QString::fromStdString(strBuf);

      if (H5Lexists(regionGrp.getId(), "ROI", H5P_DEFAULT) > 0) {
        H5::Group roiGrp = regionGrp.openGroup("ROI");
        p.roi = std::make_shared<ZROI>(undoStack());
        p.roi->load(roiGrp);
      }

      if (H5Lexists(regionGrp.getId(), "Mesh", H5P_DEFAULT) > 0) {
        H5::Group meshGrp = regionGrp.openGroup("Mesh");
        p.mesh = std::make_shared<ZMesh>();
        p.mesh->load(meshGrp);
      }

      nodeMap[p.id] = p;
    }

    std::map<int64_t, ZTree<RegionNode>::Iterator> itMap;
    while (!nodeMap.empty()) {
      std::map<int64_t, RegionNode>::iterator it = nodeMap.begin();
      while (it != nodeMap.end()) {
        int64_t parentID = it->second.parentID;
        std::map<int64_t, ZTree<RegionNode>::Iterator>::const_iterator nodeIt = itMap.find(parentID);
        if (nodeIt != itMap.end()) {
          itMap[it->first] = m_ontology.appendChild(nodeIt->second, it->second);
          it = nodeMap.erase(it);
        } else if (nodeMap.find(parentID) == nodeMap.end()) {
          itMap[it->first] = m_ontology.appendRoot(it->second);
          it = nodeMap.erase(it);
        } else {
          ++it;
        }
      }
    }
  }
  catch (H5::Exception const& e) {
    throw ZIOException(QString("hdf5:%1").arg(e.getDetailMsg().c_str()));
  }

  emit allMeshChanged();
  emit allROIChanged();
}

void ZRegionAnnotation::save(const QString& filename) const
{
  try {
    H5::Exception::dontPrint();

    H5::H5File file(QFile::encodeName(filename).constData(), H5F_ACC_TRUNC);

    H5::Group allGrp = file.createGroup("RegionAnnotation");

    H5::IntType intType(H5::PredType::STD_I32LE);
    H5::IntType int64Type(H5::PredType::STD_I64LE);
    H5::FloatType doubleType(H5::PredType::IEEE_F64LE);
    H5::StrType strType(0, H5T_VARIABLE);

    H5::DataSpace attrDataSpace(H5S_SCALAR);

    H5::Attribute ver = allGrp.createAttribute("Version", intType, attrDataSpace);
    int regionAnnotationVer = 100;
    ver.write(intType, &regionAnnotationVer);

    allGrp.createAttribute("Width", intType, attrDataSpace).write(intType, &m_width);
    allGrp.createAttribute("Height", intType, attrDataSpace).write(intType, &m_height);
    allGrp.createAttribute("Depth", intType, attrDataSpace).write(intType, &m_depth);
    allGrp.createAttribute("VoxelSizeXInUM", doubleType, attrDataSpace).write(doubleType, &m_voxelSizeX);
    allGrp.createAttribute("VoxelSizeYInUM", doubleType, attrDataSpace).write(doubleType, &m_voxelSizeY);
    allGrp.createAttribute("VoxelSizeZInUM", doubleType, attrDataSpace).write(doubleType, &m_voxelSizeZ);

    int idx = 0;
    for (auto it = m_ontology.cbegin(); it != m_ontology.cend(); ++it) {
      H5::Group regionGrp = allGrp.createGroup(qUtf8Printable(QString("Region%1").arg(idx + 1)));
      ++idx;
      const RegionNode& p = *it;

      H5::Attribute idAttr = regionGrp.createAttribute("ID", int64Type, attrDataSpace);
      idAttr.write(int64Type, &p.id);

      H5::Attribute parentIDAttr = regionGrp.createAttribute("ParentID", int64Type, attrDataSpace);
      parentIDAttr.write(int64Type, &p.parentID);

      H5::Attribute redAttr = regionGrp.createAttribute("Red", intType, attrDataSpace);
      redAttr.write(intType, &p.red);

      H5::Attribute greenAttr = regionGrp.createAttribute("Green", intType, attrDataSpace);
      greenAttr.write(intType, &p.green);

      H5::Attribute blueAttr = regionGrp.createAttribute("Blue", intType, attrDataSpace);
      blueAttr.write(intType, &p.blue);

      H5::Attribute name = regionGrp.createAttribute("Name", strType, attrDataSpace);
      name.write(strType, p.name.toStdString());

      H5::Attribute abbreviation = regionGrp.createAttribute("Abbreviation", strType, attrDataSpace);
      abbreviation.write(strType, p.abbreviation.toStdString());

      if (p.roi) {
        H5::Group roiGrp = regionGrp.createGroup("ROI");
        p.roi->save(roiGrp);
      }

      if (p.mesh) {
        H5::Group meshGrp = regionGrp.createGroup("Mesh");
        p.mesh->save(meshGrp);
      }
    }

    H5::Attribute numRegionAttr = allGrp.createAttribute("RegionNumber", intType, attrDataSpace);
    numRegionAttr.write(intType, &idx);
  }
  catch (H5::Exception const& e) {
    QFile::remove(filename);
    throw ZIOException(QString("hdf5:%1").arg(e.getDetailMsg().c_str()));
  }
}

void ZRegionAnnotation::updateMesh()
{
  QTemporaryDir dir;
  if (dir.isValid()) {
    QString fn = QDir(dir.path()).filePath("temp_region_annotation_label_image.mhd");
    exportLabelImage(fn, FileFormat::MetaImage, Compression::AUTO);
    auto cmd = new ZRegionAnnotationUpdateMeshCommand(*this);
    importLabelImage(fn, FileFormat::MetaImage, true, false);
    cmd->setNewOntology(m_ontology);
    m_undoStack.push(cmd);
    //emit modified();
  } else {
    throw ZException(QString("can not create temporary file for mesh updating"));
  }
}

void ZRegionAnnotation::updateMesh_Impl(const ZTree<RegionNode>& newOntology)
{
  auto it = m_ontology.begin();
  auto itn = newOntology.begin();
  for (; it != m_ontology.end(); ++it, ++itn) {
    it->mesh = itn->mesh;
  }
  emit allMeshChanged();
}

void ZRegionAnnotation::updateBoundBox()
{
  if (m_width <= 0 || m_height <= 0 || m_depth <= 0) {
    m_boundBox[0] = m_boundBox[2] = m_boundBox[4] = m_boundBox[6] = std::numeric_limits<int>::max();
    m_boundBox[1] = m_boundBox[3] = m_boundBox[5] = m_boundBox[7] = std::numeric_limits<int>::min();
  } else {
    m_boundBox[0] = 0;
    m_boundBox[1] = m_width - 1;
    m_boundBox[2] = 0;
    m_boundBox[3] = m_height - 1;
    m_boundBox[4] = 0;
    m_boundBox[5] = (m_depth - 1);
    m_boundBox[6] = 0;
    m_boundBox[7] = 0;
  }
  emit boundBoxChanged();
}

void ZRegionAnnotationUpdateMeshCommand::redo()
{
  if (m_firstRun) {
    m_firstRun = false;
  } else {
    m_regionAnnotation.updateMesh_Impl(m_newOntology);
  }
}

} // namespace nim
