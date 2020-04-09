#pragma once

#include "zimg.h"
#include "zimgsigneddistancemap.h"
#include "zlog.h"
#include "zglmutils.h"
#include "zbbox.h"
#include <H5Cpp.h>
#include <QObject>
#include <QPointF>
#include <QPainterPath>
#include <QUndoStack>
#include <map>

namespace nim {

enum class ROIType
{
  Rect,
  Ellipse,
  Polygon,
  Spline
};

struct ZROIShapeOperation
{
  ZROIShapeOperation() = default;

  ZROIShapeOperation(bool isAdd_, ROIType type_, const QRectF& rect)
    : isAdd(isAdd_), type(type_)
  {
    poly.push_back(rect.topLeft());
    poly.push_back(rect.bottomRight());
  }

  ZROIShapeOperation(bool isAdd_, ROIType type_, const QPolygonF& poly_)
    : isAdd(isAdd_), type(type_), poly(poly_)
  {
    CHECK(poly.isClosed());
  }

  void translate(double x, double y)
  { poly.translate(x, y); }

  QRectF rect() const
  {
    CHECK(poly.size() == 2);
    return QRectF(poly[0], poly[1]);
  }

  void setRect(const QRectF& rect)
  {
    poly.clear();
    poly.push_back(rect.topLeft());
    poly.push_back(rect.bottomRight());
  }

  QPainterPath toPainterPath() const;

  bool isAdd = true;
  ROIType type;
  QPolygonF poly;
};

struct ZROIControlPoint
{
  enum class Pos
  {
    TopLeft,
    MidLeft,
    BottomLeft,
    BottomMid,
    BottomRight,
    MidRight,
    TopRight,
    TopMid,
    Center,
    Any
  };

  ZROIControlPoint(int slice_, size_t shapeID_, size_t shapeIndex_, Pos pos_, size_t pointIndex_ = 0)
    : slice(slice_), shapeID(shapeID_), shapeIndex(shapeIndex_), pos(pos_), pointIndex(pointIndex_)
  {}

  int slice;
  size_t shapeID;
  size_t shapeIndex;
  Pos pos;
  size_t pointIndex;
};

class ZSliceROI
{
public:
  bool isEmpty() const
  { return m_idToShapeOperations.empty(); }

  void updatePaintPath(size_t id);

  void newRect(const QRectF& rect, size_t id);

  void newEllipse(const QRectF& ellipse, size_t id);

  void newPolygon(const QPolygonF& poly, size_t id);

  void newSpline(const QPolygonF& spline, size_t id);

  void addRect(const QRectF& rect, size_t id);

  void addEllipse(const QRectF& ellipse, size_t id);

  void addPolygon(const QPolygonF& poly, size_t id);

  void addSpline(const QPolygonF& spline, size_t id);

  void subtractRect(const QRectF& rect, size_t id);

  void subtractEllipse(const QRectF& ellipse, size_t id);

  void subtractPolygon(const QPolygonF& poly, size_t id);

  void subtractSpline(const QPolygonF& spline, size_t id);

  void rotateCtrlPoints(const std::map<size_t, std::vector<ZROIControlPoint>>& shapeIDToControlPoints, double angle,
                        std::vector<size_t>& editedShapes);

  void deleteCtrlPoints(const std::map<size_t, std::vector<ZROIControlPoint>>& shapeIDToControlPoints,
                        std::vector<size_t>& removedShapes, std::vector<size_t>& editedShapes);

  bool addCtrlPoint(const QPointF& pt, std::vector<size_t>& editedShapes);

  void addCtrlPointToShape(const QPointF& pt, size_t id);

  size_t mergeWith(const ZSliceROI& other, size_t id, std::vector<size_t>& newShapes);

  void setTopLeft(double x, double y);

  void translate(double x, double y);

  QRectF boundingRect() const;

  QPainterPath paintPath() const;

  size_t load(H5::Group& sliceGrp, size_t id, int roiVer);

  void save(H5::Group& sliceGrp) const;

  bool hasPolyOrSpline() const;

protected:
  friend class ZROI;

  std::map<size_t, std::vector<ZROIShapeOperation>> m_idToShapeOperations;
  std::map<size_t, QPainterPath> m_idToPainterPath;
};

class ZROISliceMoveSelectedControlPointsCommand;

class ZROI : public QObject
{
Q_OBJECT
public:
  using const_iterator = std::map<int, ZSliceROI>::const_iterator;

  explicit ZROI(QUndoStack* undoStack = nullptr, QObject* parent = nullptr);

  void importMaskImage(const QString& fn, FileFormat format);

  ZImg toMaskImg(int outWidth = 0, int outHeight = 0, int outDepth = -1, bool doInterpolation = true) const;

  const ZBBox<glm::ivec4>& boundBox() const
  { return m_boundBox; }

  QUndoStack* undoStack()
  { return m_undoStack; }

  bool isEmpty() const
  { return m_sliceROIs.empty(); }

  void clear();

  void deleteSliceROI(int slice);

  void deleteSliceROI_Impl(int slice)
  { emit roiDeleted(slice); m_sliceROIs.erase(slice); }

  void newRect(int slice, const QRectF& rect)
  {
    m_sliceROIs[slice].newRect(rect, m_shapeID++);
    onSliceROIUpdated(slice, std::vector<size_t>{m_shapeID-1}, std::vector<size_t>(), std::vector<size_t>());
  }

  void newEllipse(int slice, const QRectF& ellipse)
  {
    m_sliceROIs[slice].newEllipse(ellipse, m_shapeID++);
    onSliceROIUpdated(slice, std::vector<size_t>{m_shapeID-1}, std::vector<size_t>(), std::vector<size_t>());
  }

  void newPolygon(int slice, const QPolygonF& poly)
  {
    m_sliceROIs[slice].newPolygon(poly, m_shapeID++);
    onSliceROIUpdated(slice, std::vector<size_t>{m_shapeID-1}, std::vector<size_t>(), std::vector<size_t>());
  }

  void newSpline(int slice, const QPolygonF& spline)
  {
    m_sliceROIs[slice].newSpline(spline, m_shapeID++);
    onSliceROIUpdated(slice, std::vector<size_t>{m_shapeID-1}, std::vector<size_t>(), std::vector<size_t>());
  }

  void addRect(int slice, const QRectF& rect, int64_t shapeID = -1)
  {
    if (m_sliceROIs[slice].isEmpty()) {
      newRect(slice, rect);
      return;
    }
    if (shapeID < 0) {
      shapeID = m_sliceROIs[slice].m_idToShapeOperations.crbegin()->first;
    }
    m_sliceROIs[slice].addRect(rect, shapeID);
    onSliceROIUpdated(slice, std::vector<size_t>(), std::vector<size_t>(), std::vector<size_t>{size_t(shapeID)});
  }

  void addEllipse(int slice, const QRectF& ellipse, int64_t shapeID = -1)
  {
    if (m_sliceROIs[slice].isEmpty()) {
      newEllipse(slice, ellipse);
      return;
    }
    if (shapeID < 0) {
      shapeID = m_sliceROIs[slice].m_idToShapeOperations.crbegin()->first;
    }
    m_sliceROIs[slice].addEllipse(ellipse, shapeID);
    onSliceROIUpdated(slice, std::vector<size_t>(), std::vector<size_t>(), std::vector<size_t>{size_t(shapeID)});
  }

  void addPolygon(int slice, const QPolygonF& poly, int64_t shapeID = -1)
  {
    if (m_sliceROIs[slice].isEmpty()) {
      newPolygon(slice, poly);
      return;
    }
    if (shapeID < 0) {
      shapeID = m_sliceROIs[slice].m_idToShapeOperations.crbegin()->first;
    }
    m_sliceROIs[slice].addPolygon(poly, shapeID);
    onSliceROIUpdated(slice, std::vector<size_t>(), std::vector<size_t>(), std::vector<size_t>{size_t(shapeID)});
  }

  void addSpline(int slice, const QPolygonF& spline, int64_t shapeID = -1)
  {
    if (m_sliceROIs[slice].isEmpty()) {
      newSpline(slice, spline);
      return;
    }
    if (shapeID < 0) {
      shapeID = m_sliceROIs[slice].m_idToShapeOperations.crbegin()->first;
    }
    m_sliceROIs[slice].addSpline(spline, shapeID);
    onSliceROIUpdated(slice, std::vector<size_t>(), std::vector<size_t>(), std::vector<size_t>{size_t(shapeID)});
  }

  void subtractRect(int slice, const QRectF& rect, int64_t shapeID = -1)
  {
    if (m_sliceROIs[slice].isEmpty()) {
      return;
    }
    if (shapeID < 0) {
      shapeID = m_sliceROIs[slice].m_idToShapeOperations.crbegin()->first;
    }
    m_sliceROIs[slice].subtractRect(rect, shapeID);
    onSliceROIUpdated(slice, std::vector<size_t>(), std::vector<size_t>(), std::vector<size_t>{size_t(shapeID)});
  }

  void subtractEllipse(int slice, const QRectF& ellipse, int64_t shapeID = -1)
  {
    if (m_sliceROIs[slice].isEmpty()) {
      return;
    }
    if (shapeID < 0) {
      shapeID = m_sliceROIs[slice].m_idToShapeOperations.crbegin()->first;
    }
    m_sliceROIs[slice].subtractEllipse(ellipse, shapeID);
    onSliceROIUpdated(slice, std::vector<size_t>(), std::vector<size_t>(), std::vector<size_t>{size_t(shapeID)});
  }

  void subtractPolygon(int slice, const QPolygonF& poly, int64_t shapeID = -1)
  {
    if (m_sliceROIs[slice].isEmpty()) {
      return;
    }
    if (shapeID < 0) {
      shapeID = m_sliceROIs[slice].m_idToShapeOperations.crbegin()->first;
    }
    m_sliceROIs[slice].subtractPolygon(poly, shapeID);
    onSliceROIUpdated(slice, std::vector<size_t>(), std::vector<size_t>(), std::vector<size_t>{size_t(shapeID)});
  }

  void subtractSpline(int slice, const QPolygonF& spline, int64_t shapeID = -1)
  {
    if (m_sliceROIs[slice].isEmpty()) {
      return;
    }
    if (shapeID < 0) {
      shapeID = m_sliceROIs[slice].m_idToShapeOperations.crbegin()->first;
    }
    m_sliceROIs[slice].subtractSpline(spline, shapeID);
    onSliceROIUpdated(slice, std::vector<size_t>(), std::vector<size_t>(), std::vector<size_t>{size_t(shapeID)});
  }

  void mergeWith(const ZROI& other, int64_t slice = -1, int64_t shapeID = -1);

  std::set<int> mergeWith_Impl(const std::map<int, ZSliceROI>& sliceROIs, int64_t slice = -1, int64_t shapeID = -1);

  static QPainterPath splineToPainterPath(const QPolygonF& spline, bool makeCloseIfNot = false);

  const_iterator begin() const noexcept
  { return m_sliceROIs.begin(); }

  const_iterator end() const noexcept
  { return m_sliceROIs.end(); }

  const_iterator cbegin() const noexcept
  { return m_sliceROIs.cbegin(); }

  const_iterator cend() const noexcept
  { return m_sliceROIs.cend(); }

  size_t numSlices() const
  { return m_sliceROIs.size(); }

  std::vector<ZROIControlPoint> sliceControlPoints(int slice) const;

  std::vector<ZROIControlPoint> sliceControlPoints(int slice, size_t shapeID) const;

  bool hasSlice(int slice) const
  { return m_sliceROIs.find(slice) != m_sliceROIs.end(); }

  std::vector<size_t> sliceShapeIDs(int slice) const
  {
    std::vector<size_t> res;
    for (const auto&[id, shape] : m_sliceROIs.at(slice).m_idToShapeOperations) {
      res.push_back(id);
    }
    return res;
  }

  const QPainterPath& shapePainterPath(int slice, size_t id) const
  {
    return m_sliceROIs.at(slice).m_idToPainterPath.at(id);
  }

  const std::vector<ZROIShapeOperation>& shapeOperations(int slice, size_t id) const
  {
    return m_sliceROIs.at(slice).m_idToShapeOperations.at(id);
  }

  void rotateROIControlPoints(const std::vector<ZROIControlPoint>& controlPoints, double angle);

  std::set<int> rotateROIControlPoints_Impl(const std::vector<ZROIControlPoint>& controlPoints, double angle);

  void deleteROIControlPoints(const std::vector<ZROIControlPoint>& controlPoints);

  std::set<int> deleteROIControlPoints_Impl(const std::vector<ZROIControlPoint>& controlPoints);

  void clearCopy()
  { m_sliceROICopy.clear(); }

  void copyROIFromControlPoints(const std::vector<ZROIControlPoint>& controlPoints);

  void pasteROIToCoord(int slice, QPointF point);

  QPointF controlPointCoord(const ZROIControlPoint& ctrlPt) const;

  void shiftControlPointsCoords(const std::vector<ZROIControlPoint>& controlPoints, const QPointF& coordShift);

  QPointF setControlPointCoord(const ZROIControlPoint& ctrlPt, const QPointF& coord);

  const ZROIShapeOperation& controlPointShapeOp(const ZROIControlPoint& ctrlPt) const;

  bool sliceHasPolyOrSpline(int slice) const
  { return m_sliceROIs.at(slice).hasPolyOrSpline(); }

  QPainterPath slicePaintPath(int slice) const
  { return m_sliceROIs.at(slice).paintPath(); }

  void sliceAddCtrlPoint(int slice, const QPointF& pt, int shapeID = -1);

  void sliceAddCtrlPoint_Impl(int slice, const QPointF& pt, int shapeID = -1)
  {
    std::vector<size_t> shapes;
    if (shapeID >= 0) {
      shapes.push_back(shapeID);
      m_sliceROIs.at(slice).addCtrlPointToShape(pt, shapeID);
      onSliceROIUpdated(slice, std::vector<size_t>(), std::vector<size_t>(), shapes);
    } else if (m_sliceROIs.at(slice).addCtrlPoint(pt, shapes)) {
      onSliceROIUpdated(slice, std::vector<size_t>(), std::vector<size_t>(), shapes);
    }
  }

  void sliceSetTopLeft(int slice, double x, double y)
  {
    m_sliceROIs.at(slice).setTopLeft(x, y);
    onSliceROIMoved(slice, std::vector<size_t>());
  }

  void startMoveSelectedControlPointsCommand();

  void endMoveSelectedControlPointsCommand();

  void changeSliceROIs(const std::map<int, ZSliceROI>& sliceROIs, const std::set<int>& changedSlices);

  // qt style read write name filter for filedialog
  static QString fileExtension()
  { return ".nimroi"; }

  // qt style read write name filter for filedialog
  static bool canReadFile(const QString& filename)
  { return filename.endsWith(".nimroi", Qt::CaseInsensitive); }

  static bool canWriteFile(const QString& filename)
  { return filename.endsWith(".nimroi", Qt::CaseInsensitive); }

  static QString getQtReadNameFilter()
  { return QString("ROI files (*.nimroi)"); }

  static QString getQtWriteNameFilter()
  { return QString("ROI files (*.nimroi)"); }

  // might throw ZIOException
  void load(const QString& filename);

  void save(const QString& filename) const;

  void load(H5::Group& allGrp);

  void save(H5::Group& allGrp) const;

signals:

  void roiChanged(int slice, const std::vector<size_t>& newShapes,
                  const std::vector<size_t>& deletedShapes,
                  const std::vector<size_t>& changedShapes);

  void roiDeleted(int slice);

  void roiMoved(int slice, const std::vector<size_t>& changedShapes);

  void boundBoxChanged();

  void selectShape(int slice, size_t shapeID, bool append);

  void undoStackCleanChanged(bool clean);

protected:
  void resetBoundBox();

  void onSliceROIUpdated(int slice,
                         const std::vector<size_t>& newShapes,
                         const std::vector<size_t>& deletedShapes,
                         const std::vector<size_t>& changedShapes);

  void onSliceROIMoved(int slice, const std::vector<size_t>& changedShapes);

protected:
  friend class ZROICommand;

  std::map<int, ZSliceROI> m_sliceROIs;
  ZBBox<glm::ivec4> m_boundBox;

  QUndoStack* m_undoStack;
  ZROISliceMoveSelectedControlPointsCommand* m_moveSelectedControlPointsCommand = nullptr;
  std::set<int> m_changedSlices;

  size_t m_shapeID = 0;

  std::map<int, ZSliceROI> m_sliceROICopy;
};

class ZROICommand : public QUndoCommand
{
public:
  explicit ZROICommand(ZROI& roi)
    : QUndoCommand(), m_roi(roi), m_oldSliceROIs(m_roi.m_sliceROIs)
  {}

  void undo() override
  { m_roi.changeSliceROIs(m_oldSliceROIs, m_changedSlices); }

protected:
  ZROI& m_roi;
  std::map<int, ZSliceROI> m_oldSliceROIs;
  std::set<int> m_changedSlices;
};

class ZROIRotateControlPointsCommand : public ZROICommand
{
public:
  ZROIRotateControlPointsCommand(ZROI& roi, const std::vector<ZROIControlPoint>& controlPoints, double angle)
    : ZROICommand(roi), m_controlPoints(controlPoints), m_angle(angle)
  {
    setText("Rotate Control Points");
  }

  void redo() override
  { m_changedSlices = m_roi.rotateROIControlPoints_Impl(m_controlPoints, m_angle); }

protected:
  std::vector<ZROIControlPoint> m_controlPoints;
  double m_angle;
};

class ZROIDeleteControlPointsCommand : public ZROICommand
{
public:
  ZROIDeleteControlPointsCommand(ZROI& roi, const std::vector<ZROIControlPoint>& controlPoints)
    : ZROICommand(roi), m_controlPoints(controlPoints)
  {
    setText("Delete Control Points");
  }

  void redo() override
  { m_changedSlices = m_roi.deleteROIControlPoints_Impl(m_controlPoints); }

protected:
  std::vector<ZROIControlPoint> m_controlPoints;
};

class ZROIDeleteSliceROICommand : public ZROICommand
{
public:
  ZROIDeleteSliceROICommand(ZROI& roi, int slice)
    : ZROICommand(roi), m_slice(slice)
  {
    setText("Delete Slice ROI");
  }

  void redo() override
  {
    m_roi.deleteSliceROI_Impl(m_slice);
    m_changedSlices.insert(m_slice);
  }

protected:
  int m_slice;
};

class ZROISliceAddControlPointCommand : public ZROICommand
{
public:
  ZROISliceAddControlPointCommand(ZROI& roi, int slice, const QPointF& pt, int shapeID = -1)
    : ZROICommand(roi), m_slice(slice), m_controlPoint(pt), m_shapeID(shapeID)
  {
    setText("Add Control Points");
  }

  void redo() override
  {
    m_roi.sliceAddCtrlPoint_Impl(m_slice, m_controlPoint, m_shapeID);
    m_changedSlices.insert(m_slice);
  }

protected:
  int m_slice;
  QPointF m_controlPoint;
  int m_shapeID;
};

class ZROISliceMoveSelectedControlPointsCommand : public ZROICommand
{
public:
  explicit ZROISliceMoveSelectedControlPointsCommand(ZROI& roi)
    : ZROICommand(roi), m_firstRun(true)
  {
    setText("Move Selected Control Points");
  }

  void setNewSliceROIs(const std::map<int, ZSliceROI>& nsr)
  { m_newSliceROIs = nsr; }

  void setChangedSlices(const std::set<int>& cs)
  { m_changedSlices = cs; }

  void redo() override;

protected:
  bool m_firstRun;
  std::map<int, ZSliceROI> m_newSliceROIs;
};

class ZROIMergeROICommand : public ZROICommand
{
public:
  ZROIMergeROICommand(ZROI& roi, const std::map<int, ZSliceROI>& otherSliceROIs, int64_t slice = -1, int64_t shapeID = -1)
    : ZROICommand(roi), m_otherSliceROIs(otherSliceROIs), m_slice(slice), m_shapeID(shapeID)
  {
    setText("Merge ROI");
  }

  void redo() override
  { m_changedSlices = m_roi.mergeWith_Impl(m_otherSliceROIs, m_slice, m_shapeID); }

protected:
  std::map<int, ZSliceROI> m_otherSliceROIs;
  int64_t m_slice;
  int64_t m_shapeID;
};

} // namespace nim

