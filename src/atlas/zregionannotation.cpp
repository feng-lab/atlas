#include "zregionannotation.h"

#include "zexception.h"
#include "zimgconnectedcomponents.h"
#include "zlog.h"
#include "zcpuinfo.h"
#include "zbenchtimer.h"
#include "zioutils.h"
#include <QFile>
#include <QTemporaryDir>
#include <QSvgGenerator>
#include <QPainter>

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
#if 1
  // regions << "GPe" << "STN" << "SNr" << "STRv" << "STRd" << "GPi" << "SPF";
#else
  // regions << "GPe" << "STN" << "SNr" << "STRv" << "STRd" << "GPi" << "SPF" << "grey";
#endif
  readMouseBrainAtlasOntology(regions, m_ontology);
  connect(&m_undoStack, &QUndoStack::cleanChanged,
          this, &ZRegionAnnotation::undoStackCleanChanged);
}

ZRegionAnnotation::ZRegionAnnotation(const QString& filename, QObject* parent)
  : QObject(parent)
{
  ZTree<RegionNode> ontology;
  readMouseBrainAtlasOntology(QStringList(), ontology);
  load(filename);
  for (auto& read_region : m_ontology) {
    for (auto& region : ontology) {
      if (region.id == read_region.id) {
        region.roi = read_region.roi;
        region.mesh = read_region.mesh;
      }
    }
  }
  m_ontology.swap(ontology);
  connect(&m_undoStack, &QUndoStack::cleanChanged,
          this, &ZRegionAnnotation::undoStackCleanChanged);
}

ZRegionAnnotation::~ZRegionAnnotation()
{
  m_undoStack.disconnect(this);
  clear();
}

void ZRegionAnnotation::clear()
{
  m_voxelSizeX = 1;
  m_voxelSizeX = 1;
  m_voxelSizeX = 1;
  m_ontology.clear();
  updateBoundBox();
}

void ZRegionAnnotation::importLabelImage(const QString& fn, FileFormat format, bool createMesh, bool createROI,
                                         double scaleX, double scaleY, double scaleZ)
{
  ZBenchTimer bt;

  std::vector<ZImgInfo> infos = ZImg::readImgInfos(fn, nullptr, format);
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

  ZImg origLabelImg(fn, ZImgRegion(), 0, 1, 1, 1, format);
  //LOG(INFO) << origLabelImg.info().toQString();
//  m_width = origLabelImg.width();
//  m_height = origLabelImg.height();
//  m_depth = origLabelImg.depth();
  // todo: ask user if voxel size not exist
  if (origLabelImg.info().voxelSizeUnit != VoxelSizeUnit::none) {
    m_voxelSizeX = origLabelImg.info().voxelSizeXInUm() * scaleX;
    m_voxelSizeY = origLabelImg.info().voxelSizeYInUm() * scaleY;
    m_voxelSizeZ = origLabelImg.info().voxelSizeZInUm() * scaleZ;
  }

  for (auto it = m_ontology.beginPostOrder(); it != m_ontology.endPostOrder(); ++it) {
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
  std::set<int64_t> ancestorLabels;
  for (auto it = m_ontology.cbeginBreadthFirst(); it != m_ontology.cendBreadthFirst(); ++it) {
    if (labels.find(it->id) != labels.end()) {
      for (auto pit = m_ontology.cbeginAncestor(it); pit != m_ontology.cendAncestor(it); ++pit) {
        if (labels.find(pit->id) == labels.end()) {
          ancestorLabels.insert(pit->id);
        }
      }
    }
  }

  ZImg labelImg = origLabelImg;
  info.setVoxelFormat<uint8_t>();
  ZImg binaryImg(info);
  auto maxPossibleLabelInImg = origLabelImg.dataRangeMax<int64_t>();
  auto minPossibleLabelInImg = origLabelImg.dataRangeMin<int64_t>();

  LOG(INFO) << "Importing Label Image...";
  for (auto it = m_ontology.beginPostOrder(); it != m_ontology.endPostOrder(); ++it) {
    LOG(INFO) << "Processing region " << it->abbreviation << " " << it->id << "...";
    if (it->id > maxPossibleLabelInImg || it->id < minPossibleLabelInImg) {
      continue;
    }

    if (labels.find(it->id) == labels.end()) {
      continue;
    }

    bool processMesh = labels.find(it->id) != labels.end() || ancestorLabels.find(it->id) != ancestorLabels.end();
    bool processROI = labels.find(it->id) != labels.end() || ancestorLabels.find(it->id) != ancestorLabels.end();

    if (!processMesh && ! processROI) {
      continue;
    }

    // create binary image
    binaryImg.binaryOperation(labelImg, MarkAsIfOtherEqualsOtherWiseZero(1, it->id));
//    if (it->id == 997) {
//      for (size_t z = 0; z < binaryImg.depth(); ++z) {
//        ZImg simg = binaryImg.createView(z, 0, 0);
//        ZImg fimg = imFill.run(simg);
//        binaryImg.pasteImg(fimg, ZVoxelCoordinate(0, 0, z));
//      }
//    }
    if (createMesh && processMesh) {
      // create mesh
      it->mesh = std::make_shared<ZMesh>();
      binaryImgToMesh(binaryImg, *it->mesh, scaleX, scaleY, scaleZ);
    }
    if (createROI && processROI) {
      // create contours
      it->roi = this->createROI();
      binaryImgToROI(binaryImg, *it->roi, scaleX, scaleY, scaleZ);
    }

    // update labelImg: change id to parentID (merge current region to parent region)
    if (!ZTree<RegionNode>::isRoot(it)) {
      int64_t parentID = it->parentID;
      auto pit = ZTree<RegionNode>::parent(it);
      while ((parentID > maxPossibleLabelInImg || parentID < minPossibleLabelInImg) &&
             !ZTree<RegionNode>::isRoot(pit)) {
        parentID = pit->parentID;
        pit = ZTree<RegionNode>::parent(pit);
      }
      if (parentID <= maxPossibleLabelInImg && parentID >= minPossibleLabelInImg) {
        labelImg.unaryOperation(ChangeValue(it->id, parentID));
      }
    }
  }
//  for (auto it = m_ontology.beginRoot(); it != m_ontology.endRoot(); ++it) {
//#if 1
//    if (it->abbreviation.compare("STRv", Qt::CaseInsensitive) == 0 ||
//        it->abbreviation.compare("STRd", Qt::CaseInsensitive) == 0) {
//      m_ontology.eraseChildren(it);
//    }
//#else
//    if (it->abbreviation.compare("STRv", Qt::CaseInsensitive) == 0 ||
//        it->abbreviation.compare("STRd", Qt::CaseInsensitive) == 0 ||
//        it->abbreviation.compare("grey", Qt::CaseInsensitive) == 0) {
//      m_ontology.eraseChildren(it);
//    }
//#endif
//  }
  LOG(INFO) << "Finish importing label image";

  STOP_AND_LOG(bt)

  if (createMesh) {
    Q_EMIT allMeshChanged();
  }
  if (createROI) {
    updateBoundBox();
    Q_EMIT allROIChanged();
  }
}

void ZRegionAnnotation::exportLabelImage(const QString& fn, FileFormat format, const ZImgWriteParameters& paras,
                                         double scaleX, double scaleY, double scaleZ,
                                         bool keepOnlyInterpolatedSlices, int interpolationMethod) const
{
  LOG(INFO) << "Exporting Label Image...";

  int64_t minID = std::numeric_limits<int64_t>::max();
  int64_t maxID = std::numeric_limits<int64_t>::lowest();
  for (auto it = m_ontology.cbeginBreadthFirst(); it != m_ontology.cendBreadthFirst(); ++it) {
    minID = std::min(minID, it->id);
    maxID = std::max(maxID, it->id);
  }
  LOG(INFO) << minID << " " << maxID;
  size_t bytePerVoxel = 1;
  VoxelFormat vf = VoxelFormat::Unsigned;
  if (minID < 0) {
    vf = VoxelFormat::Signed;
    maxID = std::max(maxID, -minID);
    if (maxID > std::numeric_limits<int8_t>::max()) {
      bytePerVoxel = 2;
    } else if (maxID > std::numeric_limits<int16_t>::max()) {
      bytePerVoxel = 4;
    } else if (maxID > std::numeric_limits<int32_t>::max()) {
      bytePerVoxel = 8;
    }
  } else {
    if (maxID > std::numeric_limits<uint8_t>::max()) {
      bytePerVoxel = 2;
    } else if (maxID > std::numeric_limits<uint16_t>::max()) {
      bytePerVoxel = 4;
    } else if (maxID > std::numeric_limits<uint32_t>::max()) {
      bytePerVoxel = 8;
    }
  }
  ZImgInfo info(m_boundBox.maxCorner.x + 2, m_boundBox.maxCorner.y + 2, m_boundBox.maxCorner.z + 2,
                1, 1, bytePerVoxel, vf);
  if (scaleX < 1.0 && scaleY < 1.0 && info.byteNumber() < ZCpuInfo::instance().nPhysicalRAM) {
    info.voxelSizeUnit = VoxelSizeUnit::um;
    info.voxelSizeX = m_voxelSizeX;
    info.voxelSizeY = m_voxelSizeY;
    info.voxelSizeZ = m_voxelSizeZ;
    ZImg res(info);
    for (auto it = m_ontology.cbeginBreadthFirst(); it != m_ontology.cendBreadthFirst(); ++it) {
      LOG(INFO) << "Processing region " << it->abbreviation << " " << it->id << "...";
      if (it->roi) {
        LOG(INFO) << "has roi";
        ZImg regionBinaryImg = it->roi->toMaskImg(res.width(), res.height(), res.depth(), true,
                                                  1.0, 1.0,
                                                  keepOnlyInterpolatedSlices, interpolationMethod);
        res.binaryOperation(regionBinaryImg, CopyAsIfOtherIsNotZero(it->id));
      }
    }
//  for (auto it = m_ontology.cbeginBreadthFirst(); it != m_ontology.cendBreadthFirst(); ++it) {
//    if (it->abbreviation.compare("GPe", Qt::CaseInsensitive) == 0 ||
//        it->abbreviation.compare("STN", Qt::CaseInsensitive) == 0) {
//      LOG(INFO) << "Post Processing Region " << it->abbreviation << " " << it->id << "...";
//      if (it->roi) {
//        ZImg regionBinaryImg = it->roi->toMaskImg(res.width(), res.height(), res.depth(), false);
//        res.binaryOperation(regionBinaryImg, CopyAsIfOtherIsNotZero(it->id));
//      }
//    }
//  }
    if (scaleX != 1.0 || scaleY != 1.0 || scaleZ != 1.0) {
      res.zoom(scaleX, scaleY, scaleZ);
    }
    res.save(fn, format, paras);
  } else {
    info = ZImgInfo(m_boundBox.maxCorner.x * scaleX + 2, m_boundBox.maxCorner.y * scaleY + 2,
                    m_boundBox.maxCorner.z + 2,
                    1, 1, bytePerVoxel, vf);
    info.voxelSizeUnit = VoxelSizeUnit::um;
    info.voxelSizeX = std::ceil(m_voxelSizeX / scaleX);
    info.voxelSizeY = std::ceil(m_voxelSizeY / scaleY);
    info.voxelSizeZ = m_voxelSizeZ;
    ZImg res(info);
    for (auto it = m_ontology.cbeginBreadthFirst(); it != m_ontology.cendBreadthFirst(); ++it) {
      LOG(INFO) << "Processing region " << it->abbreviation << " " << it->id << "...";
      if (it->roi) {
        LOG(INFO) << "has roi";
        ZImg regionBinaryImg = it->roi->toMaskImg(res.width(), res.height(), res.depth(), true,
                                                  scaleX, scaleY,
                                                  keepOnlyInterpolatedSlices, interpolationMethod);
        res.binaryOperation(regionBinaryImg, CopyAsIfOtherIsNotZero(it->id));
      }
    }
//  for (auto it = m_ontology.cbeginBreadthFirst(); it != m_ontology.cendBreadthFirst(); ++it) {
//    if (it->abbreviation.compare("GPe", Qt::CaseInsensitive) == 0 ||
//        it->abbreviation.compare("STN", Qt::CaseInsensitive) == 0) {
//      LOG(INFO) << "Post Processing Region " << it->abbreviation << " " << it->id << "...";
//      if (it->roi) {
//        ZImg regionBinaryImg = it->roi->toMaskImg(res.width(), res.height(), res.depth(), false);
//        res.binaryOperation(regionBinaryImg, CopyAsIfOtherIsNotZero(it->id));
//      }
//    }
//  }
    if (scaleZ != 1.0) {
      res.zoom(1.0, 1.0, scaleZ);
    }
    res.save(fn, format, paras);
  }
  LOG(INFO) << "Finish exporting label image";
}

void ZRegionAnnotation::exportSvgImage(const QString& fn_, double scaleX, double scaleY) const
{
  LOG(INFO) << "Exporting Svg Image...";

  QPainter painter;
  QPen pen;
  pen.setColor(QColor(0, 0, 0));
  pen.setWidth(1);
  for (int z = m_boundBox.minCorner.z; z <= m_boundBox.maxCorner.z; ++z) {
    QString fn = fn_;
    QFileInfo fi(fn);
    if (m_boundBox.maxCorner.z > m_boundBox.minCorner.z) {
      fn = QString("%1_slice%2.svg").arg(fi.dir().filePath(fi.completeBaseName())).arg(z);
      LOG(INFO) << fn;
    }
    QSvgGenerator generator;
    generator.setFileName(fn);
    generator.setSize(
      QSize(std::ceil(m_boundBox.maxCorner.x * scaleX) + 10,
            std::ceil(m_boundBox.maxCorner.y * scaleY) + 10));
    generator.setViewBox(
      QRect(0, 0,
            std::ceil(m_boundBox.maxCorner.x * scaleX) + 10,
            std::ceil(m_boundBox.maxCorner.y * scaleY) + 10));
    generator.setTitle(tr("Annotation"));

    painter.begin(&generator);
    painter.setPen(pen);
    for (auto it = m_ontology.cbeginBreadthFirst(); it != m_ontology.cendBreadthFirst(); ++it) {
      LOG(INFO) << "Processing region " << it->abbreviation << " " << it->id << "...";
      if (it->roi) {
        LOG(INFO) << "has roi";
        for (const auto& [slice, sliceROI] : *(it->roi)) {
          if (slice != z) {
            continue;
          }
          auto path = sliceROI.paintPath(scaleX, scaleY);
          painter.setBrush(QBrush(QColor(it->red, it->green, it->blue)));
          painter.drawPath(path);
          painter.drawText(path.pointAtPercent(0.25), it->abbreviation);
        }
      }
    }
    painter.end();
  }

  LOG(INFO) << "Finish exporting svg image";
}

double ZRegionAnnotation::getOptimizedScale() const
{
  return std::min(1.0, std::min(2000. / m_boundBox.maxCorner.x, 2000. / m_boundBox.maxCorner.y));
}

void ZRegionAnnotation::importLabelImageForSlicesWithoutAnnotation(const QString& fn, FileFormat format,
                                                                   double scaleX, double scaleY)
{
  ZBenchTimer bt;

  std::vector<ZImgInfo> infos = ZImg::readImgInfos(fn, nullptr, format);
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

  ZImg origLabelImg(fn, ZImgRegion(), 0, 1, 1, 1, format);
  //LOG(INFO) << origLabelImg.info().toQString();
//  m_width = origLabelImg.width();
//  m_height = origLabelImg.height();
//  m_depth = origLabelImg.depth();
  // todo: ask user if voxel size not exist
  m_voxelSizeX = origLabelImg.info().voxelSizeXInUm() * scaleX;
  m_voxelSizeY = origLabelImg.info().voxelSizeYInUm() * scaleY;
  m_voxelSizeZ = origLabelImg.info().voxelSizeZInUm();

  std::set<int64_t> labels;
  for (size_t i = 0; i < origLabelImg.channelVoxelNumber(); ++i) {
    labels.insert(origLabelImg.value<int64_t>(i));
  }

  ZImg labelImg = origLabelImg;
  info.setVoxelFormat<uint8_t>();
  ZImg binaryImg(info);
  auto maxPossibleLabelInImg = origLabelImg.dataRangeMax<int64_t>();
  auto minPossibleLabelInImg = origLabelImg.dataRangeMin<int64_t>();

  LOG(INFO) << "Importing Label Image...";
  for (auto it = m_ontology.beginPostOrder(); it != m_ontology.endPostOrder(); ++it) {
    LOG(INFO) << "Processing region " << it->abbreviation << " " << it->id << "...";
    if (it->id > maxPossibleLabelInImg || it->id < minPossibleLabelInImg) {
      continue;
    }

    bool processROI = labels.find(it->id) != labels.end();

    if (!processROI) {
      continue;
    }

    // create binary image
    binaryImg.binaryOperation(labelImg, MarkAsIfOtherEqualsOtherWiseZero(1, it->id));

    ZROI roi;
    binaryImgToROI(binaryImg, roi, scaleX, scaleY);
    if (!it->roi) {
      it->roi = createROI();
      Q_EMIT regionROIAdded(it->id, it->roi.get());
    }
    it->roi->mergeWith_Impl(roi);

    // update labelImg: change id to parentID (merge current region to parent region)
    if (!ZTree<RegionNode>::isRoot(it)) {
      int64_t parentID = it->parentID;
      auto pit = ZTree<RegionNode>::parent(it);
      while ((parentID > maxPossibleLabelInImg || parentID < minPossibleLabelInImg) &&
             !ZTree<RegionNode>::isRoot(pit)) {
        parentID = pit->parentID;
        pit = ZTree<RegionNode>::parent(pit);
      }
      if (parentID <= maxPossibleLabelInImg && parentID >= minPossibleLabelInImg) {
        labelImg.unaryOperation(ChangeValue(it->id, parentID));
      }
    }
  }

  LOG(INFO) << "Finish importing label image";

  STOP_AND_LOG(bt)
}

void ZRegionAnnotation::mergeROIToRegion(const ZROI& roi, int64_t regionID)
{
  m_undoStack.beginMacro("Add Region");
  for (auto it = m_ontology.begin(); it != m_ontology.end(); ++it) {
    if (it->id == regionID) {
      if (!it->roi) {
        it->roi = createROI();
        Q_EMIT regionROIAdded(it->id, it->roi.get());
      }
      if (regionID >= 0) {
        it->roi->mergeWith(roi);
      } else {
        it->roi->subtractROI(roi);
        it->roi->mergeWith(roi);
      }
    } else {
      if (it->roi) {
        it->roi->subtractROI(roi);
      }
    }
  }
  m_undoStack.endMacro();
}

void ZRegionAnnotation::mergeLineROI(const ZROI& roi)
{
  m_undoStack.beginMacro("Add Lines");
  for (auto it = m_ontology.begin(); it != m_ontology.end(); ++it) {
    if (it->id == -1) {
      if (!it->roi) {
        it->roi = createROI();
        Q_EMIT regionROIAdded(it->id, it->roi.get());
      }
      it->roi->mergeWith(roi);
    }
  }
  m_undoStack.endMacro();
}

//void ZRegionAnnotation::mergeROIToRegion(const ZROI &roi, int slice, size_t id, int64_t regionID)
//{
//  for (auto it = m_ontology.begin(); it != m_ontology.end(); ++it) {
//    if (it->id == regionID) {
//      if (!it->roi) {
//        it->roi = createROI();
//        Q_EMIT regionROIAdded(it->id, it->roi.get());
//      }
//      it->roi->mergeWith(roi, slice, id);
//
//      return;
//    }
//  }
//}

void ZRegionAnnotation::changeROIRegion(ZROI &roi, int slice, size_t shapeId, int64_t regionID)
{
  for (auto it = m_ontology.begin(); it != m_ontology.end(); ++it) {
    if (it->id == regionID && it->roi.get() == &roi) {
      return;
    }
  }
  for (auto it = m_ontology.begin(); it != m_ontology.end(); ++it) {
    if (it->id == regionID) {
      if (!it->roi) {
        it->roi = createROI();
        Q_EMIT regionROIAdded(it->id, it->roi.get());
      }
      it->roi->mergeWith(roi, slice, shapeId);
      roi.deleteROIShape(slice, shapeId);
      return;
    }
  }
}

const ZMesh* ZRegionAnnotation::meshOfRegion(int64_t regionID) const
{
  for (const auto& node : m_ontology) {
    if (node.id == regionID) {
      return node.mesh.get();
    }
  }
  return nullptr;
}

const ZROI* ZRegionAnnotation::roiOfRegion(int64_t regionID) const
{
  for (const auto& node : m_ontology) {
    if (node.id == regionID) {
      return node.roi.get();
    }
  }
  return nullptr;
}

QString ZRegionAnnotation::nameOfRegion(int64_t regionID) const
{
  for (const auto& node : m_ontology) {
    if (node.id == regionID) {
      return node.name;
    }
  }
  return "";
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

//    allGrp.openAttribute("Width").read(intType, &m_width);
//    allGrp.openAttribute("Height").read(intType, &m_height);
//    allGrp.openAttribute("Depth").read(intType, &m_depth);
    allGrp.openAttribute("VoxelSizeXInUM").read(doubleType, &m_voxelSizeX);
    allGrp.openAttribute("VoxelSizeYInUM").read(doubleType, &m_voxelSizeY);
    allGrp.openAttribute("VoxelSizeZInUM").read(doubleType, &m_voxelSizeZ);

    H5::Attribute numRegionAttr = allGrp.openAttribute("RegionNumber");
    int numRegion;
    numRegionAttr.read(intType, &numRegion);

    std::map<int64_t, RegionNode> nodeMap;
    for (int i = 0; i < numRegion; ++i) {
      H5::Group regionGrp = allGrp.openGroup(fmt::format("Region{}", i + 1));

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
        p.roi = createROI();
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
      auto it = nodeMap.begin();
      while (it != nodeMap.end()) {
        int64_t parentID = it->second.parentID;
        auto nodeIt = itMap.find(parentID);
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
    throw ZIOException(fmt::format("hdf5:{}", e.getDetailMsg()));
  }

  updateBoundBox();
  Q_EMIT allMeshChanged();
  Q_EMIT allROIChanged();
}

void ZRegionAnnotation::save(const QString& filename) const
{
  auto tfn = getTemporaryFilename(filename);

  try {
    H5::Exception::dontPrint();

    H5::H5File file(QFile::encodeName(tfn).constData(), H5F_ACC_TRUNC);

    H5::Group allGrp = file.createGroup("RegionAnnotation");

    H5::IntType intType(H5::PredType::STD_I32LE);
    H5::IntType int64Type(H5::PredType::STD_I64LE);
    H5::FloatType doubleType(H5::PredType::IEEE_F64LE);
    H5::StrType strType(0, H5T_VARIABLE);

    H5::DataSpace attrDataSpace(H5S_SCALAR);

    H5::Attribute ver = allGrp.createAttribute("Version", intType, attrDataSpace);
    int regionAnnotationVer = 100;
    ver.write(intType, &regionAnnotationVer);

//    allGrp.createAttribute("Width", intType, attrDataSpace).write(intType, &m_width);
//    allGrp.createAttribute("Height", intType, attrDataSpace).write(intType, &m_height);
//    allGrp.createAttribute("Depth", intType, attrDataSpace).write(intType, &m_depth);
    allGrp.createAttribute("VoxelSizeXInUM", doubleType, attrDataSpace).write(doubleType, &m_voxelSizeX);
    allGrp.createAttribute("VoxelSizeYInUM", doubleType, attrDataSpace).write(doubleType, &m_voxelSizeY);
    allGrp.createAttribute("VoxelSizeZInUM", doubleType, attrDataSpace).write(doubleType, &m_voxelSizeZ);

    int idx = 0;
    for (auto it = m_ontology.cbegin(); it != m_ontology.cend(); ++it) {
      H5::Group regionGrp = allGrp.createGroup(fmt::format("Region{}", idx + 1));
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

      if (p.roi && !p.roi->isEmpty()) {
        H5::Group roiGrp = regionGrp.createGroup("ROI");
        p.roi->save(roiGrp);
      }

      if (p.mesh && !p.mesh->empty()) {
        H5::Group meshGrp = regionGrp.createGroup("Mesh");
        p.mesh->save(meshGrp);
      }
    }

    H5::Attribute numRegionAttr = allGrp.createAttribute("RegionNumber", intType, attrDataSpace);
    numRegionAttr.write(intType, &idx);

    renameFile(tfn, filename);
  }
  catch (H5::Exception const& e) {
    QFile::remove(tfn);
    throw ZIOException(fmt::format("hdf5:{}", e.getDetailMsg()));
  }
}

void ZRegionAnnotation::interpolateRegionAnnotation(double scale)
{
  QTemporaryDir dir;
  if (dir.isValid()) {
    QString fn = QDir(dir.path()).filePath("temp_region_annotation_label_image.nim");
    exportLabelImage(fn, FileFormat::Unknown, ZImgWriteParameters(), scale, scale, 1.0, true, 0);
    auto cmd = new ZRegionAnnotationInterpolateCommand(*this);
    importLabelImageForSlicesWithoutAnnotation(fn, FileFormat::Unknown, 1.0 / scale, 1.0 / scale);
    cmd->setNewOntology(m_ontology);
    m_undoStack.push(cmd);
    //Q_EMIT modified();
  } else {
    throw ZException(QString("can not create temporary file for region interpolation"));
  }
}

void ZRegionAnnotation::interpolateRegionAnnotation2(double scale)
{
  QTemporaryDir dir;
  if (dir.isValid()) {
    QString fn = QDir(dir.path()).filePath("temp_region_annotation_label_image.nim");
    exportLabelImage(fn, FileFormat::Unknown, ZImgWriteParameters(), scale, scale, 1.0, true, 1);
    auto cmd = new ZRegionAnnotationInterpolateCommand(*this);
    importLabelImageForSlicesWithoutAnnotation(fn, FileFormat::Unknown, 1.0 / scale, 1.0 / scale);
    cmd->setNewOntology(m_ontology);
    m_undoStack.push(cmd);
    //Q_EMIT modified();
  } else {
    throw ZException(QString("can not create temporary file for region interpolation"));
  }
}

void ZRegionAnnotation::updateMesh(double scaleX, double scaleY, double scaleZ)
{
  QTemporaryDir dir;
  if (dir.isValid()) {
    QString fn = QDir(dir.path()).filePath("temp_region_annotation_label_image.nim");
    exportLabelImage(fn, FileFormat::Unknown, ZImgWriteParameters(), scaleX, scaleY, scaleZ);
    auto cmd = new ZRegionAnnotationUpdateMeshCommand(*this);
    importLabelImage(fn, FileFormat::Unknown, true, false, 1.0 / scaleX, 1.0 / scaleY, 1.0 / scaleZ);
    cmd->setNewOntology(m_ontology);
    m_undoStack.push(cmd);
    //Q_EMIT modified();
  } else {
    throw ZException(QString("can not create temporary file for mesh updating"));
  }
}

void ZRegionAnnotation::transformMesh(const glm::mat4& transformation)
{
  auto cmd = new ZRegionAnnotationTransformMeshCommand(*this, transformation);
  m_undoStack.push(cmd);
}

ZBBox<glm::ivec4> ZRegionAnnotation::copiedItemBoundBox() const
{
  ZBBox<glm::ivec4> boundBox;
  for (const auto& node : m_ontology) {
    if (node.roi) {
      boundBox.expand(node.roi->copiedItemBoundBox());
    }
  }
  return boundBox;
}

void ZRegionAnnotation::updateROI_Impl(const ZTree<RegionNode>& newOntology)
{
  auto it = m_ontology.begin();
  auto itn = newOntology.begin();
  for (; it != m_ontology.end(); ++it, ++itn) {
    it->roi = itn->roi;
  }
  Q_EMIT allROIChanged();
}

void ZRegionAnnotation::updateMesh_Impl(const ZTree<RegionNode>& newOntology)
{
  auto it = m_ontology.begin();
  auto itn = newOntology.begin();
  for (; it != m_ontology.end(); ++it, ++itn) {
    it->mesh = itn->mesh;
  }
  Q_EMIT allMeshChanged();
}

void ZRegionAnnotation::transformMesh_Impl(const glm::mat4& trans)
{
  for (RegionNode& rn : m_ontology) {
    if (rn.mesh) {
      rn.mesh->transformVerticesByMatrix(trans);
    }
  }
  Q_EMIT allMeshChanged();
}

ZTree<RegionNode> ZRegionAnnotation::copyAnnotationTree()
{
  ZTree<RegionNode> res(m_ontology);
  auto it = m_ontology.begin();
  auto itn = res.begin();
  for (; it != m_ontology.end(); ++it, ++itn) {
    if (it->mesh) {
      itn->mesh = std::make_shared<ZMesh>(*it->mesh);
    }
    if (it->roi) {
      itn->roi = createROI();
      itn->roi->mergeWith_Impl(*it->roi);
    }
  }
  return res;
}

void ZRegionAnnotation::updateBoundBox()
{
  m_boundBox.reset();
  for (const auto& node : m_ontology) {
    if (node.roi) {
      m_boundBox.expand(node.roi->boundBox());
    }
  }
  Q_EMIT boundBoxChanged();
}

std::shared_ptr<ZROI> ZRegionAnnotation::createROI()
{
  auto res = std::make_shared<ZROI>(undoStack());
  connect(res.get(), &ZROI::boundBoxChanged, this, &ZRegionAnnotation::updateBoundBox);
  return res;
}

void ZRegionAnnotationInterpolateCommand::redo()
{
  if (m_firstRun) {
    m_firstRun = false;
  } else {
    m_regionAnnotation.updateROI_Impl(m_newOntology);
  }
}

void ZRegionAnnotationUpdateMeshCommand::redo()
{
  if (m_firstRun) {
    m_firstRun = false;
  } else {
    m_regionAnnotation.updateMesh_Impl(m_newOntology);
  }
}

void ZRegionAnnotationTransformMeshCommand::redo()
{
  m_regionAnnotation.transformMesh_Impl(m_trans);
}

} // namespace nim
