#include "zroi.h"

#include "zglmutils.h"
#include "zlog.h"
#include "zsaturateoperation.h"
#include "zregionontology.h"
#include <Mathematics/GteNaturalSplineCurve.h>
#include <QFile>
#include <cmath>

namespace {

QPainterPath splineToQPainterPath(const QPolygonF& spline, bool showLastSeg = true)
{
  QPainterPath res;
  if (spline.size() < 2)
    return res;
  bool isClosed = spline.isClosed();
  if ((isClosed && spline.size() < 4) ||
      (!isClosed && spline.size() < 3)) {
    res.moveTo(spline[0]);
    res.lineTo(spline[1]);
    return res;
  }

  int numSegments = spline.size() - 1;
  std::vector<double> times(spline.size());
  times[0] = 0;
  for (size_t i = 1; i < times.size(); ++i) {
    times[i] = times[i - 1] + std::sqrt(QPointF::dotProduct(spline[i] - spline[i - 1], spline[i] - spline[i - 1]));
  }

  if (isClosed) {
    gte::NaturalSplineCurve<2, double> splineCurve(false, spline.size(), (gte::Vector<2, double> const*) spline.data(),
                                                   times.data());
    res.moveTo(spline[0]);
    int endSeg = showLastSeg ? numSegments : numSegments - 1;
    for (int i = 0; i < endSeg; ++i) {
      gte::Vector<2, double> values0[4];
      gte::Vector<2, double> values1[4];
      splineCurve.Evaluate(times[i], 1, values0);
      splineCurve.Evaluate(times[i + 1], 1, values1);
      gte::Vector<2, double>& m0 = values0[1];
      gte::Vector<2, double>& m1 = values1[1];
      m0 *= times[i + 1] - times[i];
      m1 *= times[i + 1] - times[i];
      //LOG(INFO) << m0.X() << " " << m0.Y() << " " << m1.X() << " " << m1.Y() << " " << cspline[i] << " " << cspline[i+1];
      res.cubicTo(spline[i].x() + 1. / 3. * m0[0], spline[i].y() + 1. / 3. * m0[1],
                  spline[i + 1].x() - 1. / 3. * m1[0], spline[i + 1].y() - 1. / 3. * m1[1],
                  spline[i + 1].x(), spline[i + 1].y());
    }
  } else {
    gte::NaturalSplineCurve<2, double> splineCurve(true, spline.size(), (gte::Vector<2, double> const*) spline.data(),
                                                   times.data());
    res.moveTo(spline[0]);
    int endSeg = showLastSeg ? numSegments : numSegments - 1;
    for (int i = 0; i < endSeg; ++i) {
      gte::Vector<2, double> values0[4];
      gte::Vector<2, double> values1[4];
      splineCurve.Evaluate(times[i], 1, values0);
      splineCurve.Evaluate(times[i + 1], 1, values1);
      gte::Vector<2, double>& m0 = values0[1];
      gte::Vector<2, double>& m1 = values1[1];
      m0 *= times[i + 1] - times[i];
      m1 *= times[i + 1] - times[i];
      //LOG(INFO) << m0.X() << " " << m0.Y() << " " << m1.X() << " " << m1.Y() << " " << cspline[i] << " " << cspline[i+1];
      res.cubicTo(spline[i].x() + 1. / 3. * m0[0], spline[i].y() + 1. / 3. * m0[1],
                  spline[i + 1].x() - 1. / 3. * m1[0], spline[i + 1].y() - 1. / 3. * m1[1],
                  spline[i + 1].x(), spline[i + 1].y());
    }
  }
  return res;
}

} // namespace

namespace nim {

void ZSliceROI::updateROI(bool moveOnly)
{
  m_roi = QPainterPath();
  for (int i = 0; i < m_shapeOperations.size(); ++i) {
    QPainterPath tmp;
    switch (m_shapeOperations[i].type) {
      case ROIType::Rect:
        tmp.addRect(m_shapeOperations[i].rect());
        break;
      case ROIType::Ellipse:
        tmp.addEllipse(m_shapeOperations[i].rect());
        break;
      case ROIType::Polygon:
        tmp.addPolygon(m_shapeOperations[i].poly);
        break;
      case ROIType::Spline:
        tmp.addPath(splineToQPainterPath(m_shapeOperations[i].poly));
        break;
      default:
        CHECK(false);
        break;
    }
    if (m_shapeOperations[i].isAdd) {
      m_roi += tmp;
    } else {
      m_roi -= tmp;
    }
  }

  if (!moveOnly && m_roi.isEmpty()) {
    m_shapeOperations.clear();
    m_roi = QPainterPath();
  }
}

void ZSliceROI::addRect(const QRectF& rect)
{
  if (m_roi.contains(rect))
    return;
  m_shapeOperations.push_back(ZROIShapeOperation(true, ROIType::Rect, rect));
  updateROI(false);
}

void ZSliceROI::addEllipse(const QRectF& ellipse)
{
  if (m_roi.contains(ellipse))
    return;
  m_shapeOperations.push_back(ZROIShapeOperation(true, ROIType::Ellipse, ellipse));
  updateROI(false);
}

void ZSliceROI::addPolygon(const QPolygonF& poly)
{
  QPainterPath tmp;
  tmp.addPolygon(poly);
  if (m_roi.contains(tmp))
    return;
  m_shapeOperations.push_back(ZROIShapeOperation(true, ROIType::Polygon, poly));
  updateROI(false);
}

void ZSliceROI::addSpline(const QPolygonF& spline)
{
  QPainterPath tmp;
  tmp.addPolygon(spline);
  if (m_roi.contains(tmp))
    return;
  m_shapeOperations.push_back(ZROIShapeOperation(true, ROIType::Spline, spline));
  updateROI(false);
}

void ZSliceROI::subtractRect(const QRectF& rect)
{
  if (!m_roi.intersects(rect))
    return;
  m_shapeOperations.push_back(ZROIShapeOperation(false, ROIType::Rect, rect));
  updateROI(false);
}

void ZSliceROI::subtractEllipse(const QRectF& ellipse)
{
  if (!m_roi.intersects(ellipse))
    return;
  m_shapeOperations.push_back(ZROIShapeOperation(false, ROIType::Ellipse, ellipse));
  updateROI(false);
}

void ZSliceROI::subtractPolygon(const QPolygonF& poly)
{
  QPainterPath tmp;
  tmp.addPolygon(poly);
  if (!m_roi.intersects(tmp))
    return;
  m_shapeOperations.push_back(ZROIShapeOperation(false, ROIType::Polygon, poly));
  updateROI(false);
}

void ZSliceROI::subtractSpline(const QPolygonF& spline)
{
  QPainterPath tmp;
  tmp.addPolygon(spline);
  if (!m_roi.intersects(tmp))
    return;
  m_shapeOperations.push_back(ZROIShapeOperation(false, ROIType::Spline, spline));
  updateROI(false);
}

void ZSliceROI::rotateCtrlPoints(const std::map<size_t, std::set<size_t> >& shapeOpIndexToPointIndices, double angle)
{
  for (const auto& shapeOpIndexPointIndices : shapeOpIndexToPointIndices) {
    const std::set<size_t>& pointIndices = shapeOpIndexPointIndices.second;
    ZROIShapeOperation& shapeOp = m_shapeOperations[shapeOpIndexPointIndices.first];
    QPolygonF& poly = shapeOp.poly;
    if (poly.size() > 3 && (shapeOp.type == ROIType::Polygon || shapeOp.type == ROIType::Spline)) {
      QPointF center = poly[0];
      for (int i = 1; i < poly.size(); ++i) {
        center += poly[i];
      }
      center /= poly.size();

      for (auto idx : pointIndices) {
        QPointF startPt = poly[idx] - center;
        glm::dvec2 rPt = glm::rotate(glm::dvec2(startPt.x(), startPt.y()), angle);
        QPointF resPt = QPointF(rPt.x, rPt.y) + center;
        poly[idx] = resPt;
        if (idx == 0 || idx == static_cast<size_t>(poly.size()) - 1) {
          poly[0] = resPt;
          poly[poly.size() - 1] = resPt;
        }
      }
    }
  }
  updateROI(true);
}

void ZSliceROI::deleteCtrlPoints(const std::map<size_t, std::set<size_t>>& shapeOpIndexToPointIndices)
{
  size_t shapeOpIndexSubtract = 0;
  for (const auto& shapeOpIndexPointIndices : shapeOpIndexToPointIndices) {
    size_t i = shapeOpIndexPointIndices.first - shapeOpIndexSubtract;
    const std::set<size_t>& pointIndices = shapeOpIndexPointIndices.second;
    ZROIShapeOperation& shapeOp = m_shapeOperations[i];

    bool deleteAll = shapeOp.type == ROIType::Rect ||
                     shapeOp.type == ROIType::Ellipse ||
                     (shapeOp.poly.size() - static_cast<int>(pointIndices.size()) < 4);
    //LOG(INFO) << m_shapeOperations.size() << " " << shapeOp->poly.size();
    if (deleteAll) {
      m_shapeOperations.removeAt(i);
      ++shapeOpIndexSubtract;
    } else {
      size_t pointIndexSubtract = 0;
      for (auto pointIndex : pointIndices) {
        size_t idx = pointIndex - pointIndexSubtract;
        ++pointIndexSubtract;
        if ((idx == 0 || idx == static_cast<size_t>(shapeOp.poly.size() - 1))) {
          m_shapeOperations[i].poly.removeFirst();
          m_shapeOperations[i].poly.removeLast();
          m_shapeOperations[i].poly.push_back(m_shapeOperations[i].poly.first());
        } else {
          m_shapeOperations[i].poly.removeAt(idx);
        }
      }
    }
    //LOG(INFO) << m_shapeOperations.size();
  }
  updateROI(false);
}

bool ZSliceROI::addCtrlPoint(const QPointF& pt)
{
  int shapeIdx = -1;
  int pos = -1;
  double minDist = std::numeric_limits<double>::max();
  for (int i = 0; i < m_shapeOperations.size(); ++i) {
    if (m_shapeOperations[i].type == ROIType::Polygon ||
        m_shapeOperations[i].type == ROIType::Spline) {
      const QPolygonF& poly = m_shapeOperations[i].poly;
      for (int j = 0; j < poly.size() - 1; ++j) {
        double dist = (pt - poly[j]).manhattanLength() + (pt - poly[j + 1]).manhattanLength();
        if (dist < minDist) {
          minDist = dist;
          shapeIdx = i;
          pos = j + 1;
        }
      }
    }
  }
  if (shapeIdx >= 0 && pos >= 0) {
    m_shapeOperations[shapeIdx].poly.insert(pos, pt);
    updateROI(false);
    return true;
  }
  return false;
}

void ZSliceROI::mergeWith(const ZSliceROI& other)
{
  m_shapeOperations.append(other.m_shapeOperations);
  updateROI(false);
}

void ZSliceROI::setTopLeft(double x, double y)
{
  QRectF rect = m_roi.boundingRect();
  double dx = x - rect.left();
  double dy = y - rect.top();
  for (int i = 0; i < m_shapeOperations.size(); ++i) {
    m_shapeOperations[i].translate(dx, dy);
  }
  updateROI(true);
}

void ZSliceROI::load(H5::Group& sliceGrp)
{
  m_shapeOperations.clear();
  m_roi = QPainterPath();

  try {
    H5::Exception::dontPrint();

    H5::FloatType doubleType(H5::PredType::IEEE_F64LE);
    H5::IntType intType(H5::PredType::STD_I32LE);
    H5::StrType strType(0, H5T_VARIABLE);

    H5::Attribute numShapeAttr = sliceGrp.openAttribute("ShapeNumber");
    int numShape;
    numShapeAttr.read(intType, &numShape);

    H5std_string strBuf;
    for (int j = 0; j < numShape; ++j) {
      H5::Group shapeGrp = sliceGrp.openGroup(qUtf8Printable(QString("Shape%1").arg(j + 1)));

      H5::Attribute typeAttr = shapeGrp.openAttribute("Type");
      typeAttr.read(strType, strBuf);
      ROIType type = ROIType::Rect;
      if (strBuf == "Rect") {
        type = ROIType::Rect;
      } else if (strBuf == "Ellipse") {
        type = ROIType::Ellipse;
      } else if (strBuf == "Polygon") {
        type = ROIType::Polygon;
      } else if (strBuf == "Spline") {
        type = ROIType::Spline;
      } else {
        throw ZIOException(QString("invalid shape type %1").arg(QString::fromStdString(strBuf)));
      }

      H5::Attribute isAddAttr = shapeGrp.openAttribute("IsAdd");
      int isAdd;
      isAddAttr.read(intType, &isAdd);

      H5::DataSet points = shapeGrp.openDataSet("Points");
      H5::DataSpace pointsDataspace = points.getSpace();

      if (pointsDataspace.getSimpleExtentNdims() != 2)
        throw ZIOException("Wrong ROI file contents");

      hsize_t pointListDim[2];

      pointsDataspace.getSimpleExtentDims(pointListDim, nullptr);

      if (pointListDim[1] != 2 || pointListDim[0] < 2)
        throw ZIOException("Wrong ROI file contents");

      QPolygonF poly(pointListDim[0]);
      points.read(poly.data(), doubleType);
      QRectF rect(poly[0], poly[1]);

      switch (type) {
        case ROIType::Rect:
          m_shapeOperations.push_back(ZROIShapeOperation(isAdd, ROIType::Rect, rect));
          break;
        case ROIType::Ellipse:
          m_shapeOperations.push_back(ZROIShapeOperation(isAdd, ROIType::Ellipse, rect));
          break;
        case ROIType::Polygon:
          if (!poly.isClosed()) {
            poly.push_back(poly[0]);
          }
          m_shapeOperations.push_back(ZROIShapeOperation(isAdd, ROIType::Polygon, poly));
          break;
        case ROIType::Spline:
          if (!poly.isClosed()) {
            poly.push_back(poly[0]);
          }
          m_shapeOperations.push_back(ZROIShapeOperation(isAdd, ROIType::Spline, poly));
          break;
        default:
          CHECK(false);
          break;
      }
    }
    updateROI(false);
  }
  catch (H5::Exception const& e) {
    throw ZIOException(QString("hdf5:%1").arg(e.getDetailMsg().c_str()));
  }
}

void ZSliceROI::save(H5::Group& sliceGrp) const
{
  try {
    H5::Exception::dontPrint();

    H5::FloatType doubleType(H5::PredType::IEEE_F64LE);
    H5::IntType intType(H5::PredType::STD_I32LE);
    H5::StrType strType(0, H5T_VARIABLE);

    H5::DataSpace attrDataSpace(H5S_SCALAR);

    H5::Attribute shapeNumberAttr = sliceGrp.createAttribute("ShapeNumber", intType, attrDataSpace);
    int shapeNumber = m_shapeOperations.size();
    shapeNumberAttr.write(intType, &shapeNumber);

    for (int i = 0; i < m_shapeOperations.size(); ++i) {
      H5::Group shapeGrp = sliceGrp.createGroup(qUtf8Printable(QString("Shape%1").arg(i + 1)));

      H5::Attribute type = shapeGrp.createAttribute("Type", strType, attrDataSpace);
      switch (m_shapeOperations[i].type) {
        case ROIType::Rect:
          type.write(strType, std::string("Rect"));
          break;
        case ROIType::Ellipse:
          type.write(strType, std::string("Ellipse"));
          break;
        case ROIType::Polygon:
          type.write(strType, std::string("Polygon"));
          break;
        case ROIType::Spline:
          type.write(strType, std::string("Spline"));
          break;
        default:
          CHECK(false);
          break;
      }

      H5::Attribute isAddAttr = shapeGrp.createAttribute("IsAdd", intType, attrDataSpace);
      int isAdd = m_shapeOperations[i].isAdd ? 1 : 0;
      isAddAttr.write(intType, &isAdd);

      hsize_t pointListDim[2];
      pointListDim[1] = 2;
      CHECK(m_shapeOperations[i].poly.size() >= 2);
      pointListDim[0] = m_shapeOperations[i].poly.size();
      H5::DataSpace pointListDataspace(2, pointListDim);
      H5::DataSet pointList = shapeGrp.createDataSet("Points", doubleType, pointListDataspace);
      pointList.write(m_shapeOperations[i].poly.data(), doubleType);
    }
  }
  catch (H5::Exception const& e) {
    throw ZIOException(QString("hdf5:%1").arg(e.getDetailMsg().c_str()));
  }
}

bool ZSliceROI::hasPolyOrSpline() const
{
  for (int i = 0; i < m_shapeOperations.size(); ++i) {
    if (m_shapeOperations[i].type == ROIType::Polygon ||
        m_shapeOperations[i].type == ROIType::Spline)
      return true;
  }
  return false;
}

ZROI::ZROI(QUndoStack* undoStack, QObject* parent)
  : QObject(parent)
  , m_undoStack(undoStack)
{
  resetBoundBox();
  if (!m_undoStack) {
    m_undoStack = new QUndoStack(this);
  }
}

void ZROI::importMaskImage(const QString& fn, nim::FileFormat format)
{
  ZBenchTimer bt;
  bt.start();

  std::vector<ZImgInfo> infos = ZImg::readImgInfos(fn, nullptr, format);
  if (infos.size() > 1) {
    throw ZIOException("mask image with more than one scene is not supported");
  }
  ZImgInfo info = infos[0];
  if (info.isEmpty()) {
    throw ZIOException("mask image is empty");
  }
  if (info.numChannels > 1 || info.numTimes > 1) {
    throw ZIOException("mask image can not be time sequence or color image");
  }

  if (info.isType<uint8_t>()) {
    ZImg binaryImg(fn, ZImgRegion(), 0, 1, format);
    binaryImgToROI(binaryImg, *this);
  } else {
    ZImg origMaskImg(fn, ZImgRegion(), 0, 1, format);
    binaryImgToROI(origMaskImg.binarized(), *this);
  }

  LOG(INFO) << "Finish importing mask image";

  STOP_AND_LOG(bt);
}

ZImg ZROI::toMaskImg(int outWidth, int outHeight, int outDepth, bool doInterpolation) const
{
  ZImg img;
  const auto& bBox = boundBox();
  if (bBox.minCorner().z == bBox.maxCorner().z) {
    img = ZImg(ZImgInfo(bBox.maxCorner().x + 3, bBox.maxCorner().y + 3, 1));
    const ZSliceROI& sliceROI = cbegin()->second;
    const QPainterPath& path = sliceROI.paintPath();
    for (size_t x = std::max(0, bBox.minCorner().x); x < img.width(); ++x) {
      for (size_t y = std::max(0, bBox.minCorner().y); y < img.height(); ++y) {
        if (path.contains(QPointF(x, y))) {
          *img.data<uint8_t>(x, y, 0) = 255;
        }
      }
    }

    if (outWidth <= 0 || outHeight <= 0 || outDepth <= 0) {
      img = img.crop(ZImgRegion(0, bBox.maxCorner().x + 1, 0, bBox.maxCorner().y + 1, 0, 1));
    } else {
      img = img.cropWithPad(ZVoxelCoordinate(), ZVoxelCoordinate(outWidth, outHeight, outDepth, 1, 1));
    }
  } else {
    img = ZImg(ZImgInfo(bBox.maxCorner().x + 3, bBox.maxCorner().y + 3, bBox.maxCorner().z + 3));
    std::map<size_t, ZImg> distMapImgs;
    std::vector<size_t> srcSlices;
    ZImgSignedDistanceMap<> distMap;
    distMap.setInsideIsPositive(false);
    for (const auto& sliceROI : m_sliceROIs) {
      if (sliceROI.first < 0)
        continue;
      size_t slice = sliceROI.first;
      //LOG(INFO) << slice;
      const QPainterPath& path = sliceROI.second.paintPath();
      QRectF pathRect = path.boundingRect();
      size_t minX = std::max(static_cast<int>(std::floor(pathRect.left())),
                             std::max(0, bBox.minCorner().x));
      size_t maxX = std::min(img.width(), static_cast<size_t>(std::ceil(pathRect.right())));
      size_t minY = std::max(static_cast<int>(std::floor(pathRect.top())),
                             std::max(0, bBox.minCorner().y));
      size_t maxY = std::min(img.height(), static_cast<size_t>(std::ceil(pathRect.bottom())));
      for (size_t x = minX; x < maxX; ++x) {
        for (size_t y = minY; y < maxY; ++y) {
          if (path.contains(QPointF(x, y))) {
            *img.data<uint8_t>(x, y, slice) = 255;
          }
        }
      }
      srcSlices.push_back(slice);
    }

    if (doInterpolation) {
      for (size_t i = 1; i < srcSlices.size(); ++i) {
        size_t prevSlice = srcSlices[i - 1];
        size_t nextSlice = srcSlices[i];
        if (prevSlice + 1 == nextSlice)
          continue;
        if (distMapImgs.find(prevSlice) == distMapImgs.end()) {
          distMapImgs[prevSlice] = distMap.run<double>(img.createView(prevSlice, 0, 0), false);
        }
        if (distMapImgs.find(nextSlice) == distMapImgs.end()) {
          distMapImgs[nextSlice] = distMap.run<double>(img.createView(nextSlice, 0, 0), false);
        }
        double* prevData = distMapImgs[prevSlice].channelData<double>(0);
        double* nextData = distMapImgs[nextSlice].channelData<double>(0);
        size_t numSlice = nextSlice - prevSlice - 1;
        std::vector<uint8_t*> dataPts;
        std::vector<double> progresses;
        for (size_t slice = prevSlice + 1; slice < nextSlice; ++slice) {
          dataPts.push_back(img.planeData<uint8_t>(slice));
          progresses.push_back(double(slice - prevSlice) / double(nextSlice - prevSlice));
        }
        for (size_t idx = 0; idx < img.planeVoxelNumber(); ++idx) {
          if (prevData[idx] <= 0 && nextData[idx] <= 0) {
            for (size_t k = 0; k < numSlice; ++k) {
              dataPts[k][idx] = 255;
            }
          } else if (prevData[idx] <= 0 || nextData[idx] <= 0) {
            for (size_t k = 0; k < numSlice; ++k) {
              double dst = prevData[idx] + progresses[k] * (nextData[idx] - prevData[idx]);
              if (dst <= 0)
                dataPts[k][idx] = 255;
            }
          }
        }
        if (i > 1) {
          distMapImgs.erase(distMapImgs.begin());
        }
      }
    }

    if (outWidth <= 0 || outHeight <= 0 || outDepth <= 0) {
      img = img.crop(ZImgRegion(0, bBox.maxCorner().x + 1, 0, bBox.maxCorner().y + 1, 0, bBox.maxCorner().z + 1));
    } else {
      img = img.cropWithPad(ZVoxelCoordinate(), ZVoxelCoordinate(outWidth, outHeight, outDepth, 1, 1));
    }
  }

  return img;
}

void ZROI::clear()
{
  for (const auto& sliceROI : m_sliceROIs) {
    emit roiDeleted(sliceROI.first);
  }
  m_sliceROIs.clear();
  resetBoundBox();
}

void ZROI::deleteSliceROI(int slice)
{
  if (m_sliceROIs.find(slice) != m_sliceROIs.end()) {
    m_undoStack->push(new ZROIDeleteSliceROICommand(*this, slice));
  }
}

void ZROI::mergeWith(const ZROI& other)
{
  m_undoStack->push(new ZROIMergeROICommand(*this, other.m_sliceROIs));
}

std::set<int> ZROI::mergeWith_Impl(const std::map<int, ZSliceROI>& sliceROIs)
{
  std::set<int> changedSlices;
  for (const auto& sliceROI : sliceROIs) {
    if (!sliceROI.second.isEmpty()) {
      changedSlices.insert(sliceROI.first);
      m_sliceROIs[sliceROI.first].mergeWith(sliceROI.second);
      onSliceROIUpdated(sliceROI.first);
    }
  }
  return changedSlices;
}

QPainterPath ZROI::splineToPainterPath(const QPolygonF& spline, bool makeCloseIfNot)
{
  QPainterPath res;
  if (spline.size() < 2)
    return res;
  QPolygonF cspline = spline;
  if (cspline[cspline.size() - 1] == cspline[cspline.size() - 2])
    cspline.removeLast();
  if (cspline.size() < 2)
    return res;

  if (!cspline.isClosed() && makeCloseIfNot)
    cspline << cspline[0];

  return splineToQPainterPath(cspline);
}

std::vector<ZROIControlPoint> ZROI::sliceControlPoints(int slice) const
{
  std::vector<ZROIControlPoint> res;
  const ZSliceROI& sliceROI = m_sliceROIs.at(slice);

  const QList<ZROIShapeOperation>& shapeOps = sliceROI.m_shapeOperations;
  for (int i = 0; i < shapeOps.size(); ++i) {
    if (shapeOps[i].type == ROIType::Polygon || shapeOps[i].type == ROIType::Spline) {
      for (int j = 0; j < shapeOps[i].poly.size() - 1; ++j) {
        res.emplace_back(slice, i, ZROIControlPoint::Pos::Any, j);
      }
    } else {
      res.emplace_back(slice, i, ZROIControlPoint::Pos::MidLeft);
      res.emplace_back(slice, i, ZROIControlPoint::Pos::BottomMid);
      res.emplace_back(slice, i, ZROIControlPoint::Pos::MidRight);
      res.emplace_back(slice, i, ZROIControlPoint::Pos::TopMid);
      res.emplace_back(slice, i, ZROIControlPoint::Pos::Center);
      if (shapeOps[i].type == ROIType::Rect) {
        res.emplace_back(slice, i, ZROIControlPoint::Pos::TopLeft);
        res.emplace_back(slice, i, ZROIControlPoint::Pos::BottomLeft);
        res.emplace_back(slice, i, ZROIControlPoint::Pos::BottomRight);
        res.emplace_back(slice, i, ZROIControlPoint::Pos::TopRight);
      }
    }
  }
  return res;
}

void ZROI::rotateROIControlPoints(const std::vector<ZROIControlPoint>& controlPoints, double angle)
{
  if (!controlPoints.empty())
    m_undoStack->push(new ZROIRotateControlPointsCommand(*this, controlPoints, angle));
}

std::set<int> ZROI::rotateROIControlPoints_Impl(const std::vector<ZROIControlPoint>& controlPoints, double angle)
{
  std::set<int> slices;
  std::map<int, std::map<size_t, std::set<size_t>>> sliceToShapeOpIndexToPointIndices;
  for (const auto& controlPoint : controlPoints) {
    sliceToShapeOpIndexToPointIndices[controlPoint.slice][controlPoint.shapeOperationIndex].insert(
      controlPoint.pointIndex);
    slices.insert(controlPoint.slice);
  }
  for (const auto& sliceOthers : sliceToShapeOpIndexToPointIndices) {
    auto sit = m_sliceROIs.find(sliceOthers.first);
    if (sit != m_sliceROIs.end()) {
      sit->second.rotateCtrlPoints(sliceOthers.second, angle);
      onSliceROIMoved(sliceOthers.first);
    }
  }
  return slices;
}

void ZROI::deleteROIControlPoints(const std::vector<ZROIControlPoint>& controlPoints)
{
  if (!controlPoints.empty())
    m_undoStack->push(new ZROIDeleteControlPointsCommand(*this, controlPoints));
}

std::set<int> ZROI::deleteROIControlPoints_Impl(const std::vector<ZROIControlPoint>& controlPoints)
{
  std::set<int> slices;
  std::map<int, std::map<size_t, std::set<size_t>>> sliceToShapeOpIndexToPointIndices;
  for (const auto& controlPoint : controlPoints) {
    sliceToShapeOpIndexToPointIndices[controlPoint.slice][controlPoint.shapeOperationIndex].insert(
      controlPoint.pointIndex);
    slices.insert(controlPoint.slice);
  }
  for (const auto& sliceOthers : sliceToShapeOpIndexToPointIndices) {
    auto sit = m_sliceROIs.find(sliceOthers.first);
    if (sit != m_sliceROIs.end()) {
      sit->second.deleteCtrlPoints(sliceOthers.second);
      onSliceROIUpdated(sliceOthers.first);
    }
  }
  return slices;
}

QPointF ZROI::controlPointCoord(const ZROIControlPoint& ctrlPt) const
{
  const ZROIShapeOperation& shapeOp = m_sliceROIs.at(ctrlPt.slice).m_shapeOperations[ctrlPt.shapeOperationIndex];

  double midX = 0;
  double midY = 0;
  if (shapeOp.type == ROIType::Ellipse || shapeOp.type == ROIType::Rect) {
    midX = shapeOp.rect().center().x();
    midY = shapeOp.rect().center().y();
  }
  switch (ctrlPt.pos) {
    case ZROIControlPoint::Pos::TopLeft:
      return shapeOp.rect().topLeft();
      break;
    case ZROIControlPoint::Pos::MidLeft:
      return QPointF(shapeOp.rect().left(), midY);
      break;
    case ZROIControlPoint::Pos::BottomLeft:
      return shapeOp.rect().bottomLeft();
      break;
    case ZROIControlPoint::Pos::BottomMid:
      return QPointF(midX, shapeOp.rect().bottom());
      break;
    case ZROIControlPoint::Pos::BottomRight:
      return shapeOp.rect().bottomRight();
      break;
    case ZROIControlPoint::Pos::MidRight:
      return QPointF(shapeOp.rect().right(), midY);
      break;
    case ZROIControlPoint::Pos::TopRight:
      return shapeOp.rect().topRight();
      break;
    case ZROIControlPoint::Pos::TopMid:
      return QPointF(midX, shapeOp.rect().top());
      break;
    case ZROIControlPoint::Pos::Center:
      return shapeOp.rect().center();
      break;
    case ZROIControlPoint::Pos::Any:
      return shapeOp.poly.at(ctrlPt.pointIndex);
      break;
    default:
      CHECK(false);
      return QPointF();
      break;
  }
}

QPointF ZROI::setControlPointCoord(const ZROIControlPoint& ctrlPt, const QPointF& coord)
{
  m_changedSlices.insert(ctrlPt.slice);

  ZROIShapeOperation& shapeOp = m_sliceROIs.at(ctrlPt.slice).m_shapeOperations[ctrlPt.shapeOperationIndex];
  QPointF newPos = coord;
  QRectF rect;
  if (shapeOp.type == ROIType::Ellipse || shapeOp.type == ROIType::Rect) {
    rect = shapeOp.rect();
  }

  switch (ctrlPt.pos) {
    case ZROIControlPoint::Pos::TopLeft:
      newPos.setX(qMin(shapeOp.rect().right() - 1, newPos.x()));
      newPos.setY(qMin(shapeOp.rect().bottom() - 1, newPos.y()));
      rect.setTopLeft(newPos);
      shapeOp.setRect(rect);
      break;
    case ZROIControlPoint::Pos::MidLeft:
      newPos.setX(qMin(shapeOp.rect().right() - 1, newPos.x()));
      newPos.setY(rect.center().y());
      rect.setLeft(newPos.x());
      shapeOp.setRect(rect);
      break;
    case ZROIControlPoint::Pos::BottomLeft:
      newPos.setX(qMin(shapeOp.rect().right() - 1, newPos.x()));
      newPos.setY(qMax(shapeOp.rect().top() + 1, newPos.y()));
      rect.setBottomLeft(newPos);
      shapeOp.setRect(rect);
      break;
    case ZROIControlPoint::Pos::BottomMid:
      newPos.setX(rect.center().x());
      newPos.setY(qMax(shapeOp.rect().top() + 1, newPos.y()));
      rect.setBottom(newPos.y());
      shapeOp.setRect(rect);
      break;
    case ZROIControlPoint::Pos::BottomRight:
      newPos.setX(qMax(shapeOp.rect().left() + 1, newPos.x()));
      newPos.setY(qMax(shapeOp.rect().top() + 1, newPos.y()));
      rect.setBottomRight(newPos);
      shapeOp.setRect(rect);
      break;
    case ZROIControlPoint::Pos::MidRight:
      newPos.setX(qMax(shapeOp.rect().left() + 1, newPos.x()));
      newPos.setY(rect.center().y());
      rect.setRight(newPos.x());
      shapeOp.setRect(rect);
      break;
    case ZROIControlPoint::Pos::TopRight:
      newPos.setX(qMax(shapeOp.rect().left() + 1, newPos.x()));
      newPos.setY(qMin(shapeOp.rect().bottom() - 1, newPos.y()));
      rect.setTopRight(newPos);
      shapeOp.setRect(rect);
      break;
    case ZROIControlPoint::Pos::TopMid:
      newPos.setX(rect.center().x());
      newPos.setY(qMin(shapeOp.rect().bottom() - 1, newPos.y()));
      rect.setTop(newPos.y());
      shapeOp.setRect(rect);
      break;
    case ZROIControlPoint::Pos::Center:
      rect.translate(newPos - rect.center());
      shapeOp.setRect(rect);
      break;
    case ZROIControlPoint::Pos::Any:
      shapeOp.poly[ctrlPt.pointIndex] = newPos;
      if (ctrlPt.pointIndex == 0)
        shapeOp.poly.last() = newPos;
      break;
    default:
      CHECK(false);
      break;
  }

  m_sliceROIs.at(ctrlPt.slice).updateROI(true);
  onSliceROIMoved(ctrlPt.slice);

  return newPos;
}

const ZROIShapeOperation& ZROI::controlPointShapeOp(const ZROIControlPoint& ctrlPt) const
{
  return m_sliceROIs.at(ctrlPt.slice).m_shapeOperations[ctrlPt.shapeOperationIndex];
}

void ZROI::sliceAddCtrlPoint(int slice, const QPointF& pt)
{
  m_undoStack->push(new ZROISliceAddControlPointCommand(*this, slice, pt));
}

void ZROI::startMoveSelectedControlPointsCommand()
{
  CHECK(!m_moveSelectedControlPointsCommand);
  m_changedSlices.clear();
  m_moveSelectedControlPointsCommand = new ZROISliceMoveSelectedControlPointsCommand(*this);
}

void ZROI::endMoveSelectedControlPointsCommand()
{
  if (m_moveSelectedControlPointsCommand) {
    m_moveSelectedControlPointsCommand->setNewSliceROIs(m_sliceROIs);
    m_moveSelectedControlPointsCommand->setChangedSlices(m_changedSlices);
    m_undoStack->push(m_moveSelectedControlPointsCommand);
    m_moveSelectedControlPointsCommand = nullptr;
  }
}

void ZROI::changeSliceROIs(const std::map<int, ZSliceROI>& sliceROIs, const std::set<int>& changedSlices)
{
  for (auto slice : changedSlices) {
    if (m_sliceROIs.find(slice) != m_sliceROIs.end()) {
      emit roiDeleted(slice);
    }
    if (sliceROIs.find(slice) != sliceROIs.end()) {
      m_sliceROIs[slice] = sliceROIs.at(slice);
      emit roiChanged(slice);
    }
  }
}

void ZROI::load(const QString& filename)
{
  clear();

  try {
    H5::Exception::dontPrint();

    H5::H5File file(QFile::encodeName(filename).constData(), H5F_ACC_RDONLY);

    H5::Group allGrp = file.openGroup("ROI");

    load(allGrp);
  }
  catch (H5::Exception const& e) {
    throw ZIOException(QString("hdf5:%1").arg(e.getDetailMsg().c_str()));
  }
}

void ZROI::save(const QString& filename) const
{
  try {
    H5::Exception::dontPrint();

    H5::H5File file(QFile::encodeName(filename).constData(), H5F_ACC_TRUNC);

    H5::Group allGrp = file.createGroup("ROI");

    save(allGrp);
  }
  catch (H5::Exception const& e) {
    QFile::remove(filename);
    throw ZIOException(QString("hdf5:%1").arg(e.getDetailMsg().c_str()));
  }
}

void ZROI::load(H5::Group& allGrp)
{
  clear();

  try {
    H5::Exception::dontPrint();

    H5::IntType intType(H5::PredType::STD_I32LE);

    H5::Attribute ver = allGrp.openAttribute("Version");
    int roiVer;
    ver.read(intType, &roiVer);

    H5::Attribute numSliceAttr = allGrp.openAttribute("SliceNumber");
    int numSlice;
    numSliceAttr.read(intType, &numSlice);

    for (int i = 0; i < numSlice; ++i) {
      H5::Group sliceGrp = allGrp.openGroup(qUtf8Printable(QString("Slice%1").arg(i + 1)));

      H5::Attribute sliceAttr = sliceGrp.openAttribute("Slice");
      int slice;
      sliceAttr.read(intType, &slice);

      m_sliceROIs[slice].load(sliceGrp);

      onSliceROIUpdated(slice);
    }
  }
  catch (H5::Exception const& e) {
    throw ZIOException(QString("hdf5:%1").arg(e.getDetailMsg().c_str()));
  }
}

void ZROI::save(H5::Group& allGrp) const
{
  try {
    H5::Exception::dontPrint();

    H5::IntType intType(H5::PredType::STD_I32LE);

    H5::DataSpace attrDataSpace(H5S_SCALAR);

    H5::Attribute ver = allGrp.createAttribute("Version", intType, attrDataSpace);
    int roiVer = 100;
    ver.write(intType, &roiVer);

    int idx = 0;
    for (const auto& sliceROI : m_sliceROIs) {
      if (sliceROI.second.isEmpty())
        continue;

      H5::Group sliceGrp = allGrp.createGroup(qUtf8Printable(QString("Slice%1").arg(idx + 1)));
      ++idx;

      H5::Attribute sliceAttr = sliceGrp.createAttribute("Slice", intType, attrDataSpace);
      sliceAttr.write(intType, &sliceROI.first);

      sliceROI.second.save(sliceGrp);
    }

    H5::Attribute numSliceAttr = allGrp.createAttribute("SliceNumber", intType, attrDataSpace);
    numSliceAttr.write(intType, &idx);
  }
  catch (H5::Exception const& e) {
    throw ZIOException(QString("hdf5:%1").arg(e.getDetailMsg().c_str()));
  }
}

void ZROI::resetBoundBox()
{
  m_boundBox.reset();
  for (const auto& sliceROI : m_sliceROIs) {
    QRectF rect = sliceROI.second.paintPath().boundingRect();
    m_boundBox.expand(glm::ivec4(roundTo<int>(rect.left()), roundTo<int>(rect.top()),
                                 sliceROI.first, 0));
    m_boundBox.expand(glm::ivec4(roundTo<int>(rect.right() - 1), roundTo<int>(rect.bottom() - 1),
                                 sliceROI.first, 0));
  }
  emit boundBoxChanged();
}

void ZROI::onSliceROIUpdated(int slice)
{
  //LOG(INFO) << "..";
  if (m_sliceROIs.at(slice).isEmpty()) {
    deleteSliceROI_Impl(slice);
    resetBoundBox();
  } else {
    resetBoundBox();
    emit roiChanged(slice);
  }
}

void ZROI::onSliceROIMoved(int slice)
{
  resetBoundBox();
  emit roiMoved(slice);
}

void ZROISliceMoveSelectedControlPointsCommand::redo()
{
  if (m_firstRun) {
    m_firstRun = false;
  } else {
    m_roi.changeSliceROIs(m_newSliceROIs, m_changedSlices);
  }
}

} // namespace nim
