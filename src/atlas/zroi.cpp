#include "zroi.h"

#include "zglmutils.h"
#include "zlog.h"
#include "zsaturateoperation.h"
#include "zregionontology.h"
#include "zroiutils.h"
#include "zioutils.h"
#include "zimginterpolate.h"
#include <QFile>
#include <QTransform>
#include <cmath>

namespace nim {

void ZROIShapeOperation::flipAround(double x, double y, bool hFlip, bool vFlip)
{
  if (!hFlip && !vFlip) {
    return;
  }
  if (type == ROIType::Rect || type == ROIType::Ellipse) {
    auto r = rect();
    poly.clear();
    poly.push_back(r.topLeft());
    poly.push_back(r.topRight());
    poly.push_back(r.bottomRight());
    poly.push_back(r.bottomLeft());
  }
  for (auto& pt : poly) {
    if (hFlip) {
      pt.setX(x * 2.0 - pt.x());
    }
    if (vFlip) {
      pt.setY(y * 2.0 - pt.y());
    }
  }
  if (type == ROIType::Rect || type == ROIType::Ellipse) {
    setRect(poly.boundingRect());
  }
}

QPainterPath ZROIShapeOperation::toPainterPath(double scaleX, double scaleY) const
{
  QPainterPath res;
  QTransform tfm;
  tfm.scale(scaleX, scaleY);
  if (type == ROIType::Spline || type == ROIType::Line) {
    res.addPath(ZROIUtils::splineToQPainterPath(tfm.map(poly)));
  } else if (type == ROIType::Polygon) {
    res.addPolygon(tfm.map(poly));
  } else if (type == ROIType::Rect) {
    res.addRect(tfm.mapRect(rect()));
  } else if (type == ROIType::Ellipse) {
    res.addEllipse(tfm.mapRect(rect()));
  }
  return res;
}

void ZSliceROI::updatePaintPath(size_t id)
{
  auto res = QPainterPath();
  for (const auto& so : m_idToShapeOperations.at(id)) {
    if (so.isAdd) {
      if (!res.isEmpty()) {
        CHECK(so.type != ROIType::Line);
      }
      res += so.toPainterPath();
    } else {
      CHECK(!res.isEmpty());
      CHECK(so.type != ROIType::Line);
      res -= so.toPainterPath();
    }
  }
  m_idToPainterPath[id] = res;
}

QPainterPath ZSliceROI::shapeToPainterPath(const std::vector<ZROIShapeOperation>& shape, double scaleX, double scaleY)
{
  auto res = QPainterPath();
  for (const auto& so : shape) {
    if (so.isAdd) {
      if (!res.isEmpty()) {
        CHECK(so.type != ROIType::Line);
      }
      res += so.toPainterPath(scaleX, scaleY);
    } else {
      CHECK(!res.isEmpty());
      CHECK(so.type != ROIType::Line);
      res -= so.toPainterPath(scaleX, scaleY);
    }
  }
  return res;
}

void ZSliceROI::newRect(const QRectF& rect, size_t id)
{
  CHECK(!m_idToShapeOperations.contains(id));
  m_idToShapeOperations.emplace(id, std::vector<ZROIShapeOperation>{ZROIShapeOperation(true, ROIType::Rect, rect)});
  updatePaintPath(id);
}

void ZSliceROI::newEllipse(const QRectF& ellipse, size_t id)
{
  CHECK(!m_idToShapeOperations.contains(id));
  m_idToShapeOperations.emplace(id,
                                std::vector<ZROIShapeOperation>{ZROIShapeOperation(true, ROIType::Ellipse, ellipse)});
  updatePaintPath(id);
}

void ZSliceROI::newPolygon(const QPolygonF& poly, size_t id)
{
  CHECK(!m_idToShapeOperations.contains(id));
  m_idToShapeOperations.emplace(id, std::vector<ZROIShapeOperation>{ZROIShapeOperation(true, ROIType::Polygon, poly)});
  updatePaintPath(id);
}

void ZSliceROI::newSpline(const QPolygonF& spline, size_t id)
{
  CHECK(!m_idToShapeOperations.contains(id));
  m_idToShapeOperations.emplace(id, std::vector<ZROIShapeOperation>{ZROIShapeOperation(true, ROIType::Spline, spline)});
  updatePaintPath(id);
}

void ZSliceROI::newLine(const QPolygonF& line, size_t id)
{
  CHECK(!m_idToShapeOperations.contains(id));
  m_idToShapeOperations.emplace(id, std::vector<ZROIShapeOperation>{ZROIShapeOperation(true, ROIType::Line, line)});
  updatePaintPath(id);
}

void ZSliceROI::addRect(const QRectF& rect, size_t id)
{
  CHECK(m_idToShapeOperations.contains(id));
  m_idToShapeOperations.at(id).emplace_back(true, ROIType::Rect, rect);
  updatePaintPath(id);
}

void ZSliceROI::addEllipse(const QRectF& ellipse, size_t id)
{
  CHECK(m_idToShapeOperations.contains(id));
  m_idToShapeOperations.at(id).emplace_back(true, ROIType::Ellipse, ellipse);
  updatePaintPath(id);
}

void ZSliceROI::addPolygon(const QPolygonF& poly, size_t id)
{
  CHECK(m_idToShapeOperations.contains(id));
  m_idToShapeOperations.at(id).emplace_back(true, ROIType::Polygon, poly);
  updatePaintPath(id);
}

void ZSliceROI::addSpline(const QPolygonF& spline, size_t id)
{
  CHECK(m_idToShapeOperations.contains(id));
  m_idToShapeOperations.at(id).emplace_back(true, ROIType::Spline, spline);
  updatePaintPath(id);
}

void ZSliceROI::subtractRect(const QRectF& rect, size_t id)
{
  CHECK(m_idToShapeOperations.contains(id));
  m_idToShapeOperations.at(id).emplace_back(false, ROIType::Rect, rect);
  updatePaintPath(id);
}

void ZSliceROI::subtractEllipse(const QRectF& ellipse, size_t id)
{
  CHECK(m_idToShapeOperations.contains(id));
  m_idToShapeOperations.at(id).emplace_back(false, ROIType::Ellipse, ellipse);
  updatePaintPath(id);
}

void ZSliceROI::subtractPolygon(const QPolygonF& poly, size_t id)
{
  CHECK(m_idToShapeOperations.contains(id));
  m_idToShapeOperations.at(id).emplace_back(false, ROIType::Polygon, poly);
  updatePaintPath(id);
}

void ZSliceROI::subtractSpline(const QPolygonF& spline, size_t id)
{
  CHECK(m_idToShapeOperations.contains(id));
  m_idToShapeOperations.at(id).emplace_back(false, ROIType::Spline, spline);
  updatePaintPath(id);
}

void ZSliceROI::rotateCtrlPoints(const std::map<size_t, std::vector<ZROIControlPoint>>& shapeIDToControlPoints,
                                 double angle,
                                 double x,
                                 double y,
                                 std::set<size_t>& editedShapes)
{
  bool noOp = true;
  QPointF center(x, y);
  for (const auto& [shapeID, controlPoints] : shapeIDToControlPoints) {
    for (auto& shapeOps = m_idToShapeOperations.at(shapeID); const auto& shapeOp : shapeOps) {
      if ((shapeOp.type == ROIType::Polygon || shapeOp.type == ROIType::Spline) && shapeOp.poly.size() > 3) {
        noOp = false;
      }
      if ((shapeOp.type == ROIType::Line) && shapeOp.poly.size() > 1) {
        noOp = false;
      }
    }
  }
  if (!noOp) {
    for (const auto& [shapeID, controlPoints] : shapeIDToControlPoints) {
      bool shapeChanged = false;
      auto& shapeOps = m_idToShapeOperations.at(shapeID);
      for (const auto& controlPoint : controlPoints) {
        auto& shapeOp = shapeOps[controlPoint.shapeIndex];
        if ((shapeOp.type == ROIType::Polygon || shapeOp.type == ROIType::Spline) && shapeOp.poly.size() > 3) {
          shapeChanged = true;
          QPointF startPt = shapeOp.poly[controlPoint.pointIndex] - center;
          glm::dvec2 rPt = glm::rotate(glm::dvec2(startPt.x(), startPt.y()), angle);
          QPointF resPt = QPointF(rPt.x, rPt.y) + center;
          shapeOp.poly[controlPoint.pointIndex] = resPt;
          CHECK(controlPoint.pointIndex < static_cast<size_t>(shapeOp.poly.size()) - 1);
          if (controlPoint.pointIndex == 0) {
            shapeOp.poly.back() = resPt;
          }
        }
        if ((shapeOp.type == ROIType::Line) && shapeOp.poly.size() > 1) {
          shapeChanged = true;
          QPointF startPt = shapeOp.poly[controlPoint.pointIndex] - center;
          glm::dvec2 rPt = glm::rotate(glm::dvec2(startPt.x(), startPt.y()), angle);
          QPointF resPt = QPointF(rPt.x, rPt.y) + center;
          shapeOp.poly[controlPoint.pointIndex] = resPt;
        }
      }
      if (shapeChanged) {
        updatePaintPath(shapeID);
        editedShapes.insert(shapeID);
      }
    }
  }
}

void ZSliceROI::deleteCtrlPoints(const std::map<size_t, std::vector<ZROIControlPoint>>& shapeIDToControlPoints,
                                 std::set<size_t>& removedShapes,
                                 std::set<size_t>& editedShapes)
{
  for (const auto& [shapeID, controlPoints] : shapeIDToControlPoints) {
    auto& shapeOps = m_idToShapeOperations.at(shapeID);
    std::map<size_t, size_t> shapeIndexToPointIndexSubtract;
    for (size_t i = 0; i < shapeOps.size(); ++i) {
      shapeIndexToPointIndexSubtract[i] = 0;
    }
    for (const auto& controlPoint : controlPoints) {
      if (auto& shapeOp = shapeOps[controlPoint.shapeIndex];
          shapeOp.type == ROIType::Rect || shapeOp.type == ROIType::Ellipse) {
        shapeOp.poly.clear();
      } else if (shapeOp.type == ROIType::Line) {
        if (shapeOp.poly.size() <= 1) {
          shapeOp.poly.clear();
        } else {
          size_t idx = controlPoint.pointIndex - shapeIndexToPointIndexSubtract[controlPoint.shapeIndex];
          shapeIndexToPointIndexSubtract[controlPoint.shapeIndex]++;
          shapeOp.poly.removeAt(idx);
        }
      } else {
        if (shapeOp.poly.size() <= 4) {
          shapeOp.poly.clear();
        } else {
          size_t idx = controlPoint.pointIndex - shapeIndexToPointIndexSubtract[controlPoint.shapeIndex];
          shapeIndexToPointIndexSubtract[controlPoint.shapeIndex]++;
          if (idx == 0) {
            shapeOp.poly.removeFirst();
            shapeOp.poly.removeLast();
            shapeOp.poly.push_back(shapeOp.poly.first());
          } else {
            shapeOp.poly.removeAt(idx);
          }
        }
      }
    }
    std::erase_if(shapeOps, [](const auto& so) {
      return so.poly.empty();
    });

    while (!shapeOps.empty() && !shapeOps[0].isAdd) {
      shapeOps.erase(shapeOps.begin());
    }

    if (shapeOps.empty()) {
      removedShapes.insert(shapeID);
    } else {
      updatePaintPath(shapeID);
      editedShapes.insert(shapeID);
    }
  }
  for (auto shapeID : removedShapes) {
    m_idToPainterPath.erase(shapeID);
    m_idToShapeOperations.erase(shapeID);
  }
}

bool ZSliceROI::addCtrlPoint(const QPointF& pt, std::set<size_t>& editedShapes)
{
  int64_t shapeID = -1;
  int64_t shapeIndex = -1;
  int64_t pos = -1;
  double minDist = std::numeric_limits<double>::max();
  for (const auto& [id, shapes] : m_idToShapeOperations) {
    for (size_t i = 0; i < shapes.size(); ++i) {
      const auto& shape = shapes[i];
      // VLOG(1) << (shape.type == ROIType::Line);
      if (shape.type == ROIType::Polygon || shape.type == ROIType::Spline || shape.type == ROIType::Line) {
        const QPolygonF& poly = shape.poly;
        for (int64_t j = 0; j < poly.size() - 1; ++j) {
          double dist = (pt - poly[j]).manhattanLength() + (pt - poly[j + 1]).manhattanLength();
          if (dist < minDist) {
            minDist = dist;
            shapeID = id;
            shapeIndex = i;
            pos = j + 1;
          }
        }
      }
    }
  }
  if (shapeID >= 0 && shapeIndex >= 0 && pos >= 0) {
    m_idToShapeOperations.at(shapeID)[shapeIndex].poly.insert(pos, pt);
    updatePaintPath(shapeID);
    editedShapes.insert(shapeID);
    return true;
  }
  return false;
}

void ZSliceROI::addCtrlPointToShape(const QPointF& pt, size_t shapeID)
{
  int shapeIndex = -1;
  int pos = -1;
  double minDist = std::numeric_limits<double>::max();
  const auto& shapes = m_idToShapeOperations[shapeID];
  for (size_t i = 0; i < shapes.size(); ++i) {
    const auto& shape = shapes[i];
    if (shape.type == ROIType::Polygon || shape.type == ROIType::Spline || shape.type == ROIType::Line) {
      const QPolygonF& poly = shape.poly;
      for (index_t j = 0; j < poly.size() - 1; ++j) {
        double dist = (pt - poly[j]).manhattanLength() + (pt - poly[j + 1]).manhattanLength();
        if (dist < minDist) {
          minDist = dist;
          shapeIndex = i;
          pos = j + 1;
        }
      }
    }
  }

  if (shapeIndex >= 0 && pos >= 0) {
    m_idToShapeOperations.at(shapeID)[shapeIndex].poly.insert(pos, pt);
    updatePaintPath(shapeID);
  }
}

void ZSliceROI::setTopLeft(double x, double y)
{
  QRectF rect = boundingRect();
  double dx = x - rect.left();
  double dy = y - rect.top();
  translate(dx, dy);
}

void ZSliceROI::translate(double x, double y)
{
  for (auto& [id, shapes] : m_idToShapeOperations) {
    for (auto& shape : shapes) {
      shape.translate(x, y);
    }
    updatePaintPath(id);
  }
}

void ZSliceROI::flipAround(double x, double y, bool hFlip, bool vFlip)
{
  for (auto& [id, shapes] : m_idToShapeOperations) {
    for (auto& shape : shapes) {
      shape.flipAround(x, y, hFlip, vFlip);
    }
    updatePaintPath(id);
  }
}

QRectF ZSliceROI::boundingRect() const
{
  if (m_idToShapeOperations.empty()) {
    return QRectF();
  }
  auto it = m_idToPainterPath.cbegin();
  QRectF res = it->second.boundingRect();
  ++it;
  for (; it != m_idToPainterPath.cend(); ++it) {
    res = res.united(it->second.boundingRect());
  }
  return res;
}

QPainterPath ZSliceROI::paintPath(double scaleX, double scaleY) const
{
  auto res = QPainterPath();
  if (scaleX == 1.0 && scaleY == 1.0) {
    for (const auto& [id, pp] : m_idToPainterPath) {
      res += pp;
    }
  } else {
    for (const auto& [id, shapes] : m_idToShapeOperations) {
      res += shapeToPainterPath(shapes, scaleX, scaleY);
    }
  }

  return res;
}

size_t ZSliceROI::load(H5::Group& sliceGrp, size_t id, int roiVer)
{
  m_idToPainterPath.clear();

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
      if (roiVer == 100) {
        H5::Group shapeGrp = sliceGrp.openGroup(fmt::format("Shape{}", j + 1));

        H5::Attribute typeAttr = shapeGrp.openAttribute("Type");
        typeAttr.read(strType, strBuf);
        auto type = ROIType::Rect;
        if (strBuf == "Rect") {
          type = ROIType::Rect;
        } else if (strBuf == "Ellipse") {
          type = ROIType::Ellipse;
        } else if (strBuf == "Polygon") {
          type = ROIType::Polygon;
        } else if (strBuf == "Spline") {
          type = ROIType::Spline;
        } else if (strBuf == "Line") {
          type = ROIType::Line;
        } else {
          throw ZException(fmt::format("invalid shape type {}", strBuf));
        }

        H5::Attribute isAddAttr = shapeGrp.openAttribute("IsAdd");
        int isAdd;
        isAddAttr.read(intType, &isAdd);

        H5::DataSet points = shapeGrp.openDataSet("Points");
        H5::DataSpace pointsDataspace = points.getSpace();

        if (pointsDataspace.getSimpleExtentNdims() != 2) {
          throw ZException("Wrong ROI file contents");
        }

        hsize_t pointListDim[2];

        pointsDataspace.getSimpleExtentDims(pointListDim, nullptr);

        if (pointListDim[1] != 2 || pointListDim[0] < 2) {
          throw ZException("Wrong ROI file contents");
        }

        QPolygonF poly(pointListDim[0]);
        points.read(poly.data(), doubleType);
        QRectF rect(poly[0], poly[1]);

        switch (type) {
          case ROIType::Rect:
            newRect(rect, id++);
            //            if (isAdd) {
            //              addRect(rect, id++);
            //            } else {
            //              subtractRect(rect, id++);
            //            }
            break;
          case ROIType::Ellipse:
            newEllipse(rect, id++);
            //            if (isAdd) {
            //              addEllipse(rect, id++);
            //            } else {
            //              subtractEllipse(rect, id++);
            //            }
            break;
          case ROIType::Polygon:
            if (!poly.isClosed()) {
              poly.push_back(poly[0]);
            }
            newPolygon(poly, id++);
            //            if (isAdd) {
            //              addPolygon(poly, id++);
            //            } else {
            //              subtractPolygon(poly, id++);
            //            }
            break;
          case ROIType::Spline:
            if (!poly.isClosed()) {
              poly.push_back(poly[0]);
            }
            newSpline(poly, id++);
            //            if (isAdd) {
            //              addSpline(poly, id++);
            //            } else {
            //              subtractSpline(poly, id++);
            //            }
            break;
          case ROIType::Line:
            newLine(poly, id++);
            break;
          default:
            CHECK(false);
        }
      } else {
        H5::Group allShapeGrp = sliceGrp.openGroup(fmt::format("Shape{}", j + 1));

        H5::Attribute numSubShapesAttr = allShapeGrp.openAttribute("SubShapeNumber");
        int numSubShape;
        numSubShapesAttr.read(intType, &numSubShape);

        for (int k = 0; k < numSubShape; ++k) {
          H5::Group shapeGrp = allShapeGrp.openGroup(fmt::format("SubShape{}", k + 1));

          H5::Attribute typeAttr = shapeGrp.openAttribute("Type");
          typeAttr.read(strType, strBuf);
          auto type = ROIType::Rect;
          if (strBuf == "Rect") {
            type = ROIType::Rect;
          } else if (strBuf == "Ellipse") {
            type = ROIType::Ellipse;
          } else if (strBuf == "Polygon") {
            type = ROIType::Polygon;
          } else if (strBuf == "Spline") {
            type = ROIType::Spline;
          } else if (strBuf == "Line") {
            type = ROIType::Line;
          } else {
            throw ZException(fmt::format("invalid shape type {}", strBuf));
          }

          H5::Attribute isAddAttr = shapeGrp.openAttribute("IsAdd");
          int isAdd;
          isAddAttr.read(intType, &isAdd);

          H5::DataSet points = shapeGrp.openDataSet("Points");
          H5::DataSpace pointsDataspace = points.getSpace();

          if (pointsDataspace.getSimpleExtentNdims() != 2) {
            throw ZException("Wrong ROI file contents");
          }

          hsize_t pointListDim[2];

          pointsDataspace.getSimpleExtentDims(pointListDim, nullptr);

          if (pointListDim[1] != 2 || pointListDim[0] < 2) {
            throw ZException("Wrong ROI file contents2");
          }

          QPolygonF poly(pointListDim[0]);
          points.read(poly.data(), doubleType);
          QRectF rect(poly[0], poly[1]);

          switch (type) {
            case ROIType::Rect:
              if (k == 0) {
                newRect(rect, id);
              } else if (isAdd) {
                addRect(rect, id);
              } else {
                subtractRect(rect, id);
              }
              break;
            case ROIType::Ellipse:
              if (k == 0) {
                newEllipse(rect, id);
              } else if (isAdd) {
                addEllipse(rect, id);
              } else {
                subtractEllipse(rect, id);
              }
              break;
            case ROIType::Polygon:
              if (!poly.isClosed()) {
                poly.push_back(poly[0]);
              }
              if (k == 0) {
                newPolygon(poly, id);
              } else if (isAdd) {
                addPolygon(poly, id);
              } else {
                subtractPolygon(poly, id);
              }
              break;
            case ROIType::Spline:
              if (!poly.isClosed()) {
                poly.push_back(poly[0]);
              }
              if (k == 0) {
                newSpline(poly, id);
              } else if (isAdd) {
                addSpline(poly, id);
              } else {
                subtractSpline(poly, id);
              }
              break;
            case ROIType::Line:
              //              if (poly.isClosed()) {
              //                poly.pop_back();
              //              }
              newLine(poly, id);
              break;
            default:
              CHECK(false);
          }
        }
        id++;
      }
    }
  }
  catch (const H5::Exception& e) {
    throw ZException(fmt::format("hdf5:{}", e.getDetailMsg()));
  }

  return id;
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
    int shapeNumber = m_idToShapeOperations.size();
    shapeNumberAttr.write(intType, &shapeNumber);

    size_t i = 0;
    for (const auto& [id, shapes] : m_idToShapeOperations) {
      H5::Group allShapeGrp = sliceGrp.createGroup(fmt::format("Shape{}", i + 1));
      ++i;

      H5::Attribute subShapeNumberAttr = allShapeGrp.createAttribute("SubShapeNumber", intType, attrDataSpace);
      int subShapeNumber = shapes.size();
      subShapeNumberAttr.write(intType, &subShapeNumber);

      for (int si = 0; si < subShapeNumber; ++si) {
        H5::Group shapeGrp = allShapeGrp.createGroup(fmt::format("SubShape{}", si + 1));
        const auto& shape = shapes[si];
        H5::Attribute type = shapeGrp.createAttribute("Type", strType, attrDataSpace);
        switch (shape.type) {
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
          case ROIType::Line:
            type.write(strType, std::string("Line"));
            break;
          default:
            CHECK(false);
            break;
        }

        H5::Attribute isAddAttr = shapeGrp.createAttribute("IsAdd", intType, attrDataSpace);
        int isAdd = shape.isAdd ? 1 : 0;
        isAddAttr.write(intType, &isAdd);

        hsize_t pointListDim[2];
        pointListDim[1] = 2;
        CHECK(shape.poly.size() >= 2);
        pointListDim[0] = shape.poly.size();
        H5::DataSpace pointListDataspace(2, pointListDim);
        H5::DataSet pointList = shapeGrp.createDataSet("Points", doubleType, pointListDataspace);
        pointList.write(shape.poly.data(), doubleType);
      }
    }
  }
  catch (const H5::Exception& e) {
    throw ZException(fmt::format("hdf5:{}", e.getDetailMsg()));
  }
}

bool ZSliceROI::hasPolyOrSpline() const
{
  for (const auto& [id, shapes] : m_idToShapeOperations) {
    for (const auto& shape : shapes) {
      if (shape.type == ROIType::Polygon || shape.type == ROIType::Spline) {
        return true;
      }
    }
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
    m_undoStack = new QUndoStack(this);
  }
  connect(m_undoStack, &QUndoStack::cleanChanged, this, &ZROI::undoStackCleanChanged);
}

void ZROI::importMaskImage(const QString& fn, FileFormat format)
{
  ZBenchTimer bt;

  std::vector<ZImgInfo> infos = ZImg::readImgInfos(fn, nullptr, format);
  if (infos.size() > 1) {
    throw ZException("mask image with more than one scene is not supported");
  }
  ZImgInfo info = infos[0];
  if (info.isEmpty()) {
    throw ZException("mask image is empty");
  }
  if (info.numChannels > 1 || info.numTimes > 1) {
    throw ZException("mask image can not be time sequence or color image");
  }

  if (info.isType<uint8_t>()) {
    ZImg binaryImg(fn, ZImgRegion(), 0, 1, 1, 1, format);
    binaryImgToROI(binaryImg, *this);
  } else {
    ZImg origMaskImg(fn, ZImgRegion(), 0, 1, 1, 1, format);
    binaryImgToROI(origMaskImg.binarized(), *this);
  }

  LOG(INFO) << "Finish importing mask image";

  STOP_AND_LOG(bt)
}

ZImg ZROI::toMaskImg(int outWidth,
                     int outHeight,
                     int outDepth,
                     bool doInterpolation,
                     double scaleX,
                     double scaleY,
                     bool keepOnlyInterpolatedSlices,
                     int interpolationMethod) const
{
  ZImg img;
  const auto& bBox = boundBox();
  if (bBox.minCorner.z == bBox.maxCorner.z) {
    img = ZImg(ZImgInfo(bBox.maxCorner.x * scaleX + 3, bBox.maxCorner.y * scaleY + 3, 1));
    const QPainterPath& path = slicePaintPath(cbegin()->first, scaleX, scaleY);
    auto [mask, x_start, y_start] = ZROIUtils::qPainterPathToMask(path);
    img.pasteImg(mask, ZVoxelCoordinate(x_start, y_start));

    if (outWidth <= 0 || outHeight <= 0 || outDepth <= 0) {
      img = img.crop(ZImgRegion(0, bBox.maxCorner.x * scaleX + 1, 0, bBox.maxCorner.y * scaleY + 1, 0, 1));
    } else {
      img = img.cropWithPad(ZVoxelCoordinate(), ZVoxelCoordinate(outWidth, outHeight, outDepth, 1, 1));
    }
  } else {
    img = ZImg(ZImgInfo(bBox.maxCorner.x * scaleX + 3, bBox.maxCorner.y * scaleY + 3, bBox.maxCorner.z + 3));
    std::map<size_t, ZImg> distMapImgs;
    std::vector<size_t> srcSlices;
    ZImgSignedDistanceMap<> distMap;
    distMap.setInsideIsPositive(false);
    ZImgInterpolate<> interpolator;
    for (const auto& [slice, ROI] : m_sliceROIs) {
      if (slice < 0) {
        continue;
      }
      // VLOG(1) << slice;
      const QPainterPath& path = slicePaintPath(slice, scaleX, scaleY);
      auto [mask, x_start, y_start] = ZROIUtils::qPainterPathToMask(path);
      if (mask.isEmpty()) {
        continue;
      }
      if (mask.sum() == 0.0) {
        continue;
      }
      img.pasteImg(mask, ZVoxelCoordinate(x_start, y_start, slice));
      srcSlices.push_back(slice);
    }

    if (doInterpolation) {
      if (interpolationMethod == 0) {
        img = interpolator.run(img);
      } else {
        for (size_t i = 1; i < srcSlices.size(); ++i) {
          size_t prevSlice = srcSlices[i - 1];
          size_t nextSlice = srcSlices[i];
          if (prevSlice + 1 == nextSlice) {
            continue;
          }
          if (!distMapImgs.contains(prevSlice)) {
            distMapImgs[prevSlice] = distMap.run<double>(img.createView(prevSlice, 0, 0), false);
          }
          if (!distMapImgs.contains(nextSlice)) {
            distMapImgs[nextSlice] = distMap.run<double>(img.createView(nextSlice, 0, 0), false);
          }
          auto prevData = distMapImgs[prevSlice].channelData<double>(0);
          auto nextData = distMapImgs[nextSlice].channelData<double>(0);
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
                dataPts[k][idx] = 1;
              }
            } else if (prevData[idx] <= 0 || nextData[idx] <= 0) {
              for (size_t k = 0; k < numSlice; ++k) {
                double dst = prevData[idx] + progresses[k] * (nextData[idx] - prevData[idx]);
                if (dst <= 0) {
                  dataPts[k][idx] = 1;
                }
              }
            }
          }
          if (i > 1) {
            distMapImgs.erase(distMapImgs.begin());
          }
        }
      }

      if (keepOnlyInterpolatedSlices) {
        for (auto slice : srcSlices) {
          img.createView(slice, 0, 0).fill(0);
        }
      }
    }

    if (outWidth <= 0 || outHeight <= 0 || outDepth <= 0) {
      img = img.crop(
        ZImgRegion(0, bBox.maxCorner.x * scaleX + 1, 0, bBox.maxCorner.y * scaleY + 1, 0, bBox.maxCorner.z + 1));
    } else {
      img = img.cropWithPad(ZVoxelCoordinate(), ZVoxelCoordinate(outWidth, outHeight, outDepth, 1, 1));
    }
  }

  return img;
}

void ZROI::clear()
{
  for (const auto& [slice, ROI] : m_sliceROIs) {
    Q_EMIT roiDeleted(slice);
  }
  m_sliceROIs.clear();
  resetBoundBox();
}

void ZROI::deleteSliceROI(int slice)
{
  if (m_sliceROIs.contains(slice)) {
    m_undoStack->push(new ZROIDeleteSliceROICommand(*this, slice));
  }
}

void ZROI::mergeWith(const ZROI& other, int64_t slice, int64_t shapeID)
{
  m_undoStack->push(new ZROIMergeROICommand(*this, other.m_sliceROIs, slice, shapeID));
}

std::set<int> ZROI::mergeWith_Impl(const std::map<int, ZSliceROI>& sliceROIs, int64_t slice, int64_t shapeID)
{
  std::set<int> changedSlices;
  if (shapeID >= 0) { // merge one shape
    changedSlices.insert(slice);
    const auto& sliceROI = sliceROIs.at(slice);
    std::set<size_t> newShapes;

    m_sliceROIs[slice].m_idToShapeOperations[m_shapeID] = sliceROI.m_idToShapeOperations.at(shapeID);
    newShapes.insert(m_shapeID);
    m_sliceROIs[slice].m_idToPainterPath[m_shapeID++] = sliceROI.m_idToPainterPath.at(shapeID);

    onSliceROIUpdated(slice, newShapes, std::set<size_t>(), std::set<size_t>());
  } else { // merge all
    for (const auto& [s, sliceROI] : sliceROIs) {
      if (!sliceROI.isEmpty()) {
        changedSlices.insert(s);
        std::set<size_t> newShapes;

        for (const auto& [ido, shape] : sliceROI.m_idToShapeOperations) {
          m_sliceROIs[s].m_idToShapeOperations[m_shapeID] = shape;
          newShapes.insert(m_shapeID);
          m_sliceROIs[s].m_idToPainterPath[m_shapeID++] = sliceROI.m_idToPainterPath.at(ido);
        }

        onSliceROIUpdated(s, newShapes, std::set<size_t>(), std::set<size_t>());
      }
    }
  }
  return changedSlices;
}

void ZROI::subtractROI(const ZROI& other, int64_t slice, int64_t shapeID)
{
  m_undoStack->push(new ZROISubtractROICommand(*this, other.m_sliceROIs, slice, shapeID));
}

std::set<int> ZROI::subtractROI_Impl(const std::map<int, ZSliceROI>& sliceROIs, int64_t slice, int64_t shapeID)
{
  std::set<int> changedSlices;
  if (shapeID >= 0 && m_sliceROIs.contains(slice) && !m_sliceROIs.at(slice).isEmpty() &&
      sliceROIs.at(slice).m_idToShapeOperations.at(shapeID)[0].type != ROIType::Line) { // subtract one shape
    changedSlices.insert(slice);
    const auto& sliceROI = sliceROIs.at(slice);
    std::set<size_t> editedShapes;

    for (auto& [id, pp] : m_sliceROIs.at(slice).m_idToPainterPath) {
      if (m_sliceROIs.at(slice).m_idToShapeOperations.at(id)[0].type == ROIType::Line) {
        continue;
      }
      const auto& shapePP = sliceROI.m_idToPainterPath.at(shapeID);
      if (pp.intersects(shapePP)) {
        editedShapes.insert(id);
        for (auto subShape : sliceROI.m_idToShapeOperations.at(shapeID)) {
          subShape.isAdd = !subShape.isAdd;
          m_sliceROIs.at(slice).m_idToShapeOperations[id].push_back(subShape);
        }
        pp -= shapePP;
      }
    }

    onSliceROIUpdated(slice, std::set<size_t>(), std::set<size_t>(), editedShapes);
  } else { // all
    for (const auto& [s, sliceROI] : sliceROIs) {
      if (!sliceROI.isEmpty() && m_sliceROIs.contains(s) && !m_sliceROIs.at(s).isEmpty()) {
        changedSlices.insert(s);
        std::set<size_t> editedShapes;

        for (const auto& [shID, shapePP] : sliceROI.m_idToPainterPath) {
          if (sliceROI.m_idToShapeOperations.at(shID)[0].type == ROIType::Line) {
            continue;
          }
          for (auto& [id, pp] : m_sliceROIs.at(s).m_idToPainterPath) {
            if (m_sliceROIs.at(s).m_idToShapeOperations.at(id)[0].type == ROIType::Line) {
              continue;
            }
            if (pp.intersects(shapePP)) {
              editedShapes.insert(id);
              for (auto subShape : sliceROI.m_idToShapeOperations.at(shID)) {
                subShape.isAdd = !subShape.isAdd;
                m_sliceROIs.at(s).m_idToShapeOperations[id].push_back(subShape);
              }
              pp -= shapePP;
            }
          }
        }

        onSliceROIUpdated(s, std::set<size_t>(), std::set<size_t>(), editedShapes);
      }
    }
  }
  return changedSlices;
}

QPainterPath ZROI::splineToPainterPath(const QPolygonF& spline, bool makeCloseIfNot)
{
  QPainterPath res;
  if (spline.size() < 2) {
    return res;
  }
  QPolygonF cspline = spline;
  if (cspline[cspline.size() - 1] == cspline[cspline.size() - 2]) {
    cspline.removeLast();
  }
  if (cspline.size() < 2) {
    return res;
  }

  if (!cspline.isClosed() && makeCloseIfNot) {
    cspline << cspline[0];
  }

  return ZROIUtils::splineToQPainterPath(cspline);
}

std::vector<ZROIControlPoint> ZROI::sliceControlPoints(int slice) const
{
  std::vector<ZROIControlPoint> res;
  const ZSliceROI& sliceROI = m_sliceROIs.at(slice);

  for (const auto& [id, shapes] : sliceROI.m_idToShapeOperations) {
    for (size_t si = 0; si < shapes.size(); ++si) {
      const auto& shape = shapes[si];
      if (shape.type == ROIType::Polygon || shape.type == ROIType::Spline) {
        for (index_t j = 0; j < shape.poly.size() - 1; ++j) {
          res.emplace_back(slice, id, si, ZROIControlPoint::Pos::Any, j);
        }
      } else if (shape.type == ROIType::Line) {
        for (index_t j = 0; j < shape.poly.size(); ++j) {
          res.emplace_back(slice, id, si, ZROIControlPoint::Pos::Any, j);
        }
      } else {
        res.emplace_back(slice, id, si, ZROIControlPoint::Pos::MidLeft);
        res.emplace_back(slice, id, si, ZROIControlPoint::Pos::BottomMid);
        res.emplace_back(slice, id, si, ZROIControlPoint::Pos::MidRight);
        res.emplace_back(slice, id, si, ZROIControlPoint::Pos::TopMid);
        res.emplace_back(slice, id, si, ZROIControlPoint::Pos::Center);
        if (shape.type == ROIType::Rect) {
          res.emplace_back(slice, id, si, ZROIControlPoint::Pos::TopLeft);
          res.emplace_back(slice, id, si, ZROIControlPoint::Pos::BottomLeft);
          res.emplace_back(slice, id, si, ZROIControlPoint::Pos::BottomRight);
          res.emplace_back(slice, id, si, ZROIControlPoint::Pos::TopRight);
        }
      }
    }
  }
  return res;
}

std::vector<ZROIControlPoint> ZROI::sliceControlPoints(int slice, size_t shapeID) const
{
  std::vector<ZROIControlPoint> res;
  const ZSliceROI& sliceROI = m_sliceROIs.at(slice);

  const auto& shapes = sliceROI.m_idToShapeOperations.at(shapeID);
  for (size_t si = 0; si < shapes.size(); ++si) {
    const auto& shape = shapes[si];
    if (shape.type == ROIType::Polygon || shape.type == ROIType::Spline) {
      for (index_t j = 0; j < shape.poly.size() - 1; ++j) {
        res.emplace_back(slice, shapeID, si, ZROIControlPoint::Pos::Any, j);
      }
    } else if (shape.type == ROIType::Line) {
      for (index_t j = 0; j < shape.poly.size(); ++j) {
        res.emplace_back(slice, shapeID, si, ZROIControlPoint::Pos::Any, j);
      }
    } else {
      res.emplace_back(slice, shapeID, si, ZROIControlPoint::Pos::MidLeft);
      res.emplace_back(slice, shapeID, si, ZROIControlPoint::Pos::BottomMid);
      res.emplace_back(slice, shapeID, si, ZROIControlPoint::Pos::MidRight);
      res.emplace_back(slice, shapeID, si, ZROIControlPoint::Pos::TopMid);
      res.emplace_back(slice, shapeID, si, ZROIControlPoint::Pos::Center);
      if (shape.type == ROIType::Rect) {
        res.emplace_back(slice, shapeID, si, ZROIControlPoint::Pos::TopLeft);
        res.emplace_back(slice, shapeID, si, ZROIControlPoint::Pos::BottomLeft);
        res.emplace_back(slice, shapeID, si, ZROIControlPoint::Pos::BottomRight);
        res.emplace_back(slice, shapeID, si, ZROIControlPoint::Pos::TopRight);
      }
    }
  }

  return res;
}

void ZROI::rotateROIControlPoints(const std::vector<ZROIControlPoint>& controlPoints, double angle, double x, double y)
{
  if (!controlPoints.empty()) {
    m_undoStack->push(new ZROIRotateControlPointsCommand(*this, controlPoints, angle, x, y));
  }
}

std::set<int>
ZROI::rotateROIControlPoints_Impl(const std::vector<ZROIControlPoint>& controlPoints, double angle, double x, double y)
{
  std::set<int> slices;
  std::map<int, std::map<size_t, std::vector<ZROIControlPoint>>> sliceToShapeIDToControlPoints;
  for (const auto& controlPoint : controlPoints) {
    sliceToShapeIDToControlPoints[controlPoint.slice][controlPoint.shapeID].push_back(controlPoint);
    slices.insert(controlPoint.slice);
  }
  for (const auto& [slice, shapeIDToControlPoints] : sliceToShapeIDToControlPoints) {
    auto sit = m_sliceROIs.find(slice);
    if (sit != m_sliceROIs.end()) {
      std::set<size_t> editedShapes;
      sit->second.rotateCtrlPoints(shapeIDToControlPoints, angle, x, y, editedShapes);
      onSliceROIMoved(slice, editedShapes);
    }
  }
  return slices;
}

void ZROI::deleteROIControlPoints(const std::vector<ZROIControlPoint>& controlPoints)
{
  if (!controlPoints.empty()) {
    m_undoStack->push(new ZROIDeleteControlPointsCommand(*this, controlPoints));
  }
}

std::set<int> ZROI::deleteROIControlPoints_Impl(const std::vector<ZROIControlPoint>& controlPoints)
{
  std::set<int> slices;
  std::map<int, std::map<size_t, std::vector<ZROIControlPoint>>> sliceToShapeIDToControlPoints;
  for (const auto& controlPoint : controlPoints) {
    sliceToShapeIDToControlPoints[controlPoint.slice][controlPoint.shapeID].push_back(controlPoint);
    slices.insert(controlPoint.slice);
  }
  for (const auto& [slice, shapeIDToControlPoints] : sliceToShapeIDToControlPoints) {
    auto sit = m_sliceROIs.find(slice);
    if (sit != m_sliceROIs.end()) {
      std::set<size_t> removedShapes;
      std::set<size_t> editedShapes;
      sit->second.deleteCtrlPoints(shapeIDToControlPoints, removedShapes, editedShapes);
      onSliceROIUpdated(slice, std::set<size_t>(), removedShapes, editedShapes);
    }
  }
  return slices;
}

void ZROI::deleteROIShape(int slice, size_t shapeID)
{
  m_undoStack->push(new ZROIDeleteROIShapeCommand(*this, slice, shapeID));
}

void ZROI::deleteROIShape_Impl(int slice, size_t shapeID)
{
  m_sliceROIs.at(slice).m_idToPainterPath.erase(shapeID);
  m_sliceROIs.at(slice).m_idToShapeOperations.erase(shapeID);
  onSliceROIUpdated(slice, std::set<size_t>(), std::set<size_t>{shapeID}, std::set<size_t>());
}

void ZROI::copyROIFromControlPoints(const std::vector<ZROIControlPoint>& controlPoints)
{
  clearCopy();

  std::map<int, std::set<size_t>> sliceToShapeID;
  for (const auto& controlPoint : controlPoints) {
    sliceToShapeID[controlPoint.slice].insert(controlPoint.shapeID);
  }
  for (const auto& [slice, shapeIDs] : sliceToShapeID) {
    for (auto shapeID : shapeIDs) {
      m_sliceROICopy[slice].m_idToShapeOperations[shapeID] = m_sliceROIs[slice].m_idToShapeOperations[shapeID];
      m_sliceROICopy[slice].m_idToPainterPath[shapeID] = m_sliceROIs[slice].m_idToPainterPath[shapeID];
    }
  }
}

ZBBox<glm::ivec4> ZROI::copiedItemBoundBox() const
{
  ZBBox<glm::ivec4> boundBox;
  for (const auto& [slice, ROI] : m_sliceROICopy) {
    QRectF rect = ROI.boundingRect();
    boundBox.expand(glm::ivec4(roundTo<int>(rect.left()), roundTo<int>(rect.top()), slice, 0));
    boundBox.expand(glm::ivec4(roundTo<int>(rect.right() - 1), roundTo<int>(rect.bottom() - 1), slice, 0));
  }
  return boundBox;
}

void ZROI::pasteROIToCoord(int slice, QPointF point, const ZBBox<glm::ivec4>& srcBoundBox, bool hFlip, bool vFlip)
{
  if (m_sliceROICopy.empty()) {
    return;
  }

  std::map<int, ZSliceROI> sliceROIs;
  for (const auto& [s, sliceROI] : m_sliceROICopy) {
    auto targetSlice = s + slice - srcBoundBox.minCorner.z;
    sliceROIs[targetSlice] = sliceROI;
    if (hFlip || vFlip) {
      sliceROIs[targetSlice].flipAround(point.x(), point.y(), hFlip, vFlip);
    } else {
      sliceROIs[targetSlice].translate(point.x() - srcBoundBox.minCorner.x, point.y() - srcBoundBox.minCorner.y);
    }
  }

  m_undoStack->push(new ZROIMergeROICommand(*this, sliceROIs));
}

QPointF ZROI::controlPointCoord(const ZROIControlPoint& ctrlPt) const
{
  const ZROIShapeOperation& shapeOp =
    m_sliceROIs.at(ctrlPt.slice).m_idToShapeOperations.at(ctrlPt.shapeID)[ctrlPt.shapeIndex];

  double midX = 0;
  double midY = 0;
  if (shapeOp.type == ROIType::Ellipse || shapeOp.type == ROIType::Rect) {
    midX = shapeOp.rect().center().x();
    midY = shapeOp.rect().center().y();
  }
  switch (ctrlPt.pos) {
    case ZROIControlPoint::Pos::TopLeft:
      return shapeOp.rect().topLeft();
    case ZROIControlPoint::Pos::MidLeft:
      return QPointF(shapeOp.rect().left(), midY);
    case ZROIControlPoint::Pos::BottomLeft:
      return shapeOp.rect().bottomLeft();
    case ZROIControlPoint::Pos::BottomMid:
      return QPointF(midX, shapeOp.rect().bottom());
    case ZROIControlPoint::Pos::BottomRight:
      return shapeOp.rect().bottomRight();
    case ZROIControlPoint::Pos::MidRight:
      return QPointF(shapeOp.rect().right(), midY);
    case ZROIControlPoint::Pos::TopRight:
      return shapeOp.rect().topRight();
    case ZROIControlPoint::Pos::TopMid:
      return QPointF(midX, shapeOp.rect().top());
    case ZROIControlPoint::Pos::Center:
      return shapeOp.rect().center();
    case ZROIControlPoint::Pos::Any:
      return shapeOp.poly.at(ctrlPt.pointIndex);
    default:
      CHECK(false);
      return QPointF();
  }
}

void ZROI::shiftControlPointsCoords(const std::vector<ZROIControlPoint>& controlPoints, const QPointF& coordShift)
{
  std::map<int, std::set<size_t>> sliceToShapeOpIDs;
  for (const auto& ctrlPt : controlPoints) {
    m_changedSlices.insert(ctrlPt.slice);
    sliceToShapeOpIDs[ctrlPt.slice].insert(ctrlPt.shapeID);

    ZROIShapeOperation& shapeOp = m_sliceROIs.at(ctrlPt.slice).m_idToShapeOperations[ctrlPt.shapeID][ctrlPt.shapeIndex];
    QPointF newPos = controlPointCoord(ctrlPt) + coordShift;
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
        if (ctrlPt.pointIndex == 0 && shapeOp.type != ROIType::Line) {
          shapeOp.poly.last() = newPos;
        }
        break;
      default:
        CHECK(false);
        break;
    }
  }

  for (const auto& [slice, shapeOpIDs] : sliceToShapeOpIDs) {
    for (auto shapeOpID : shapeOpIDs) {
      m_sliceROIs.at(slice).updatePaintPath(shapeOpID);
    }
    onSliceROIMoved(slice, shapeOpIDs);
  }
}

QPointF ZROI::setControlPointCoord(const ZROIControlPoint& ctrlPt, const QPointF& coord)
{
  m_changedSlices.insert(ctrlPt.slice);

  ZROIShapeOperation& shapeOp = m_sliceROIs.at(ctrlPt.slice).m_idToShapeOperations[ctrlPt.shapeID][ctrlPt.shapeIndex];
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
      if (ctrlPt.pointIndex == 0 && shapeOp.type != ROIType::Line) {
        shapeOp.poly.last() = newPos;
      }
      break;
    default:
      CHECK(false);
      break;
  }

  m_sliceROIs.at(ctrlPt.slice).updatePaintPath(ctrlPt.shapeID);

  onSliceROIMoved(ctrlPt.slice, std::set<size_t>{ctrlPt.shapeID});

  return newPos;
}

const ZROIShapeOperation& ZROI::controlPointShapeOp(const ZROIControlPoint& ctrlPt) const
{
  return m_sliceROIs.at(ctrlPt.slice).m_idToShapeOperations.at(ctrlPt.shapeID)[ctrlPt.shapeIndex];
}

void ZROI::sliceAddCtrlPoint(int slice, const QPointF& pt, int shapeID)
{
  m_undoStack->push(new ZROISliceAddControlPointCommand(*this, slice, pt, shapeID));
}

void ZROI::sliceSubtractShape(int slice, size_t shapeID, const std::vector<ZROIShapeOperation>& otherShape)
{
  if (otherShape[0].type == ROIType::Line ||
      m_sliceROIs.at(slice).m_idToShapeOperations.at(shapeID)[0].type == ROIType::Line) {
    return;
  }
  QPainterPath otherShapePp = ZSliceROI::shapeToPainterPath(otherShape);
  const auto& shapePp = m_sliceROIs.at(slice).m_idToPainterPath.at(shapeID);
  if (otherShapePp.contains(shapePp)) {
    m_undoStack->push(new ZROIDeleteROIShapeCommand(*this, slice, shapeID));
  } else if (otherShapePp.intersects(shapePp)) {
    m_undoStack->push(new ZROISliceSubtractShapeCommand(*this, slice, shapeID, otherShape));
  } else {
    LOG(WARNING) << "no intersection area, give up subtracting";
  }
}

void ZROI::sliceSubtractShape_Impl(int slice, size_t shapeID, const std::vector<ZROIShapeOperation>& otherShape)
{
  std::set<size_t> shapes;
  shapes.insert(shapeID);
  auto& shape = m_sliceROIs.at(slice).m_idToShapeOperations.at(shapeID);
  for (const auto& otherShapeOp : otherShape) {
    ZROIShapeOperation shapeOpCopy = otherShapeOp;
    shapeOpCopy.isAdd = !shapeOpCopy.isAdd;
    shape.push_back(shapeOpCopy);
  }
  m_sliceROIs.at(slice).updatePaintPath(shapeID);
  onSliceROIUpdated(slice, std::set<size_t>(), std::set<size_t>(), shapes);
}

void ZROI::startMoveSelectedControlPointsCommand()
{
  if (!m_moveSelectedControlPointsCommand) {
    m_changedSlices.clear();
    m_moveSelectedControlPointsCommand = new ZROISliceMoveSelectedControlPointsCommand(*this);
  }
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
  if (changedSlices.empty()) {
    return;
  }
  for (auto slice : changedSlices) {
    if (m_sliceROIs.contains(slice)) {
      Q_EMIT roiDeleted(slice);
    }
    if (sliceROIs.contains(slice)) {
      m_sliceROIs[slice] = sliceROIs.at(slice);
      Q_EMIT roiChanged(slice, std::set<size_t>(), std::set<size_t>(), std::set<size_t>());
    }
  }
  resetBoundBox();
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
    throw ZException(fmt::format("hdf5:{}", e.getDetailMsg()), ZException::Option::CheckErrno);
  }
}

void ZROI::save(const QString& filename) const
{
  auto tfn = getTemporaryFilename(filename);
  try {
    {
      H5::Exception::dontPrint();

      H5::H5File file(QFile::encodeName(tfn).constData(), H5F_ACC_TRUNC);

      H5::Group allGrp = file.createGroup("ROI");

      save(allGrp);
    }

    renameFile(tfn, filename);
  }
  catch (const H5::Exception& e) {
    QFile::remove(tfn);
    throw ZException(fmt::format("hdf5:{}", e.getDetailMsg()), ZException::Option::CheckErrno);
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
      H5::Group sliceGrp = allGrp.openGroup(fmt::format("Slice{}", i + 1));

      H5::Attribute sliceAttr = sliceGrp.openAttribute("Slice");
      int slice;
      sliceAttr.read(intType, &slice);

      m_shapeID = m_sliceROIs[slice].load(sliceGrp, m_shapeID, roiVer);

      onSliceROIUpdated(slice, std::set<size_t>(), std::set<size_t>(), std::set<size_t>());
    }
  }
  catch (const H5::Exception& e) {
    throw ZException(fmt::format("hdf5:{}", e.getDetailMsg()));
  }
}

void ZROI::save(H5::Group& allGrp) const
{
  try {
    H5::Exception::dontPrint();

    H5::IntType intType(H5::PredType::STD_I32LE);

    H5::DataSpace attrDataSpace(H5S_SCALAR);

    H5::Attribute ver = allGrp.createAttribute("Version", intType, attrDataSpace);
    int roiVer = 200;
    ver.write(intType, &roiVer);

    int idx = 0;
    for (const auto& [slice, ROI] : m_sliceROIs) {
      if (ROI.isEmpty()) {
        continue;
      }

      H5::Group sliceGrp = allGrp.createGroup(fmt::format("Slice{}", idx + 1));
      ++idx;

      H5::Attribute sliceAttr = sliceGrp.createAttribute("Slice", intType, attrDataSpace);
      sliceAttr.write(intType, &slice);

      ROI.save(sliceGrp);
    }

    H5::Attribute numSliceAttr = allGrp.createAttribute("SliceNumber", intType, attrDataSpace);
    numSliceAttr.write(intType, &idx);
  }
  catch (const H5::Exception& e) {
    throw ZException(fmt::format("hdf5:{}", e.getDetailMsg()));
  }
}

void ZROI::resetBoundBox()
{
  m_boundBox.reset();
  for (const auto& [slice, ROI] : m_sliceROIs) {
    QRectF rect = ROI.boundingRect();
    m_boundBox.expand(glm::ivec4(roundTo<int>(rect.left()), roundTo<int>(rect.top()), slice, 0));
    m_boundBox.expand(glm::ivec4(roundTo<int>(rect.right() - 1), roundTo<int>(rect.bottom() - 1), slice, 0));
  }
  Q_EMIT boundBoxChanged();
}

void ZROI::onSliceROIUpdated(int slice,
                             const std::set<size_t>& newShapes,
                             const std::set<size_t>& deletedShapes,
                             const std::set<size_t>& changedShapes)
{
  // VLOG(1) << "..";
  if (m_sliceROIs.at(slice).isEmpty()) {
    deleteSliceROI_Impl(slice);
    resetBoundBox();
  } else {
    resetBoundBox();
    Q_EMIT roiChanged(slice, newShapes, deletedShapes, changedShapes);
  }
}

void ZROI::onSliceROIMoved(int slice, const std::set<size_t>& changedShapes)
{
  resetBoundBox();
  Q_EMIT roiMoved(slice, changedShapes);
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
