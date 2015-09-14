#ifndef ZROI_H
#define ZROI_H

#include <QObject>
#include <QPointF>
#include <QPainterPath>
#include <QUndoStack>
#include <map>
#include "zimg.h"
#include "zimgsigneddistancemap.h"
#include <H5Cpp.h>

namespace nim {

enum class ROIType {
  Rect,
  Ellipse,
  Polygon,
  Spline
};

struct ZROIShapeOperation {
  ZROIShapeOperation(bool isAdd, ROIType type, const QRectF& rect)
    : isAdd(isAdd), type(type)
  {
    poly.push_back(rect.topLeft());
    poly.push_back(rect.bottomRight());
  }
  ZROIShapeOperation(bool isAdd, ROIType type, const QPolygonF& poly)
    : isAdd(isAdd), type(type), poly(poly)
  {
    assert(poly.isClosed());
  }

  void translate(double x, double y) { poly.translate(x, y); }
  QRectF rect() const { assert(poly.size() == 2); return QRectF(poly[0], poly[1]); }
  void setRect(const QRectF &rect)
  {
    poly.clear();
    poly.push_back(rect.topLeft());
    poly.push_back(rect.bottomRight());
  }

  bool isAdd;
  ROIType type;
  QPolygonF poly;
};

class ZSliceROI
{
public:
  bool isEmpty() const { return m_roi.isEmpty(); }

  void updateROI(bool moveOnly);
  void addRect(const QRectF& rect);
  void addEllipse(const QRectF& ellipse);
  void addPolygon(const QPolygonF& poly);
  void addSpline(const QPolygonF& spline);

  void subtractRect(const QRectF& rect);
  void subtractEllipse(const QRectF& ellipse);
  void subtractPolygon(const QPolygonF& poly);
  void subtractSpline(const QPolygonF& spline);

  void deleteCtrlPoints(const std::map<size_t, std::set<size_t>> &shapeOpIndexToPointIndices);
  bool addCtrlPoint(const QPointF &pt);

  void mergeWith(const ZSliceROI &other);

  void setTopLeft(double x, double y);

  const QPainterPath& paintPath() const { return m_roi; }

  void load(H5::Group &grp);
  void save(H5::Group &grp) const;

  bool hasPolyOrSpline() const;

protected:
  friend class ZROI;

  QPainterPath m_roi;
  QList<ZROIShapeOperation> m_shapeOperations;
};

struct ZROIControlPoint {
  enum class Pos {
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
  ZROIControlPoint(int slice, size_t shapeOperationIndex, Pos pos, size_t pointIndex = 0)
    : slice(slice), shapeOperationIndex(shapeOperationIndex), pos(pos), pointIndex(pointIndex)
  {}
  int slice;
  size_t shapeOperationIndex;
  Pos pos;
  size_t pointIndex;
};

class ZROISliceMoveSelectedControlPointsCommand;

class ZROI : public QObject
{
  Q_OBJECT
public:
  typedef std::map<int, ZSliceROI>::const_iterator const_iterator;

  explicit ZROI(QUndoStack *undoStack = nullptr, QObject *parent = nullptr);

  ZImg toMaskImg(int outWidth = 0, int outHeight = 0, int outDepth = -1, bool doInterpolation = true) const;

  const std::vector<int>& boundBox() const { return m_boundBox; }
  QUndoStack* undoStack() { return m_undoStack; }

  bool isEmpty() const { return m_sliceROIs.empty(); }
  void clear();
  void deleteSliceROI(int slice);
  void deleteSliceROI_Impl(int slice) { m_sliceROIs.erase(slice); emit roiDeleted(slice); }
  void addRect(int slice, const QRectF& rect) { m_sliceROIs[slice].addRect(rect); onSliceROIUpdated(slice); }
  void addEllipse(int slice, const QRectF& ellipse) { m_sliceROIs[slice].addEllipse(ellipse); onSliceROIUpdated(slice); }
  void addPolygon(int slice, const QPolygonF& poly) { m_sliceROIs[slice].addPolygon(poly); onSliceROIUpdated(slice); }
  void addSpline(int slice, const QPolygonF& spline) { m_sliceROIs[slice].addSpline(spline); onSliceROIUpdated(slice); }

  void subtractRect(int slice, const QRectF& rect) { m_sliceROIs[slice].subtractRect(rect); onSliceROIUpdated(slice); }
  void subtractEllipse(int slice, const QRectF& ellipse) { m_sliceROIs[slice].subtractEllipse(ellipse); onSliceROIUpdated(slice); }
  void subtractPolygon(int slice, const QPolygonF& poly) { m_sliceROIs[slice].subtractPolygon(poly); onSliceROIUpdated(slice); }
  void subtractSpline(int slice, const QPolygonF& spline) { m_sliceROIs[slice].subtractSpline(spline); onSliceROIUpdated(slice); }

  void mergeWith(const ZROI &other);
  std::set<int> mergeWith_Impl(const std::map<int, ZSliceROI> &sliceROIs);

  static QPainterPath splineToPainterPath(const QPolygonF& spline, bool makeCloseIfNot = false);

  const_iterator cbegin() const { return m_sliceROIs.cbegin(); }
  const_iterator cend() const { return m_sliceROIs.cend(); }
  size_t numSlices() const { return m_sliceROIs.size(); }

  std::vector<ZROIControlPoint> sliceControlPoints(int slice) const;
  void deleteROIControlPoints(const std::vector<ZROIControlPoint> &controlPoints);
  std::set<int> deleteROIControlPoints_Impl(const std::vector<ZROIControlPoint> &controlPoints);
  QPointF controlPointCoord(const ZROIControlPoint &ctrlPt) const;
  QPointF setControlPointCoord(const ZROIControlPoint &ctrlPt, const QPointF &coord);
  const ZROIShapeOperation& controlPointShapeOp(const ZROIControlPoint &ctrlPt) const;

  bool sliceHasPolyOrSpline(int slice) const { return m_sliceROIs.at(slice).hasPolyOrSpline(); }
  const QPainterPath& slicePaintPath(int slice) const { return m_sliceROIs.at(slice).paintPath(); }
  void sliceAddCtrlPoint(int slice, const QPointF &pt);
  void sliceAddCtrlPoint_Impl(int slice, const QPointF &pt) { if (m_sliceROIs.at(slice).addCtrlPoint(pt)) onSliceROIUpdated(slice); }
  void sliceSetTopLeft(int slice, double x, double y) { m_sliceROIs.at(slice).setTopLeft(x, y); onSliceROIMoved(slice); }

  void startMoveSelectedControlPointsCommand();
  void endMoveSelectedControlPointsCommand();

  void changeSliceROIs(const std::map<int, ZSliceROI> &sliceROIs, const std::set<int> &changedSlices);

  // qt style read write name filter for filedialog
  static bool canReadFile(const QString& filename) { return filename.endsWith(".nimroi", Qt::CaseInsensitive); }
  static bool canWriteFile(const QString& filename) { return filename.endsWith(".nimroi", Qt::CaseInsensitive); }
  static QString getQtReadNameFilter() { return QString("ROI files (*.nimroi)"); }
  static QString getQtWriteNameFilter()  { return QString("ROI files (*.nimroi)"); }
  // might throw ZIOException
  void load(const QString &filename);
  void save(const QString &filename) const;

  void load(H5::Group &grp);
  void save(H5::Group &grp) const;

signals:
  void roiChanged(int slice);
  void roiDeleted(int slice);
  void roiMoved(int slice);
  void boundBoxChanged();

public slots:

protected:
  void resetBoundBox();

protected slots:
  void onSliceROIUpdated(int slice);
  void onSliceROIMoved(int slice);

protected:
  friend class ZROICommand;

  std::map<int, ZSliceROI> m_sliceROIs;
  std::vector<int> m_boundBox;

  QUndoStack *m_undoStack;
  ZROISliceMoveSelectedControlPointsCommand *m_moveSelectedControlPointsCommand;
  std::set<int> m_changedSlices;
};

class ZROICommand : public QUndoCommand
{
public:
  ZROICommand(ZROI &roi)
    : QUndoCommand(), m_roi(roi), m_oldSliceROIs(m_roi.m_sliceROIs)
  {}
  void undo() override { m_roi.changeSliceROIs(m_oldSliceROIs, m_changedSlices); }
protected:
  ZROI& m_roi;
  std::map<int, ZSliceROI> m_oldSliceROIs;
  std::set<int> m_changedSlices;
};

class ZROIDeleteControlPointsCommand : public ZROICommand
{
public:
  ZROIDeleteControlPointsCommand(ZROI &roi, const std::vector<ZROIControlPoint> &controlPoints)
    : ZROICommand(roi), m_controlPoints(controlPoints)
  {
    setText("Delete Control Points");
  }
  void redo() override { m_changedSlices = m_roi.deleteROIControlPoints_Impl(m_controlPoints); }
protected:
  std::vector<ZROIControlPoint> m_controlPoints;
};

class ZROIDeleteSliceROICommand : public ZROICommand
{
public:
  ZROIDeleteSliceROICommand(ZROI &roi, int slice)
    : ZROICommand(roi), m_slice(slice)
  {
    setText("Delete Slice ROI");
  }
  void redo() override { m_roi.deleteSliceROI_Impl(m_slice); m_changedSlices.insert(m_slice); }
protected:
  int m_slice;
};

class ZROISliceAddControlPointCommand : public ZROICommand
{
public:
  ZROISliceAddControlPointCommand(ZROI &roi, int slice, const QPointF &pt)
    : ZROICommand(roi), m_slice(slice), m_controlPoint(pt)
  {
    setText("Add Control Points");
  }
  void redo() override { m_roi.sliceAddCtrlPoint_Impl(m_slice, m_controlPoint); m_changedSlices.insert(m_slice); }
protected:
  int m_slice;
  QPointF m_controlPoint;
};

class ZROISliceMoveSelectedControlPointsCommand : public ZROICommand
{
public:
  ZROISliceMoveSelectedControlPointsCommand(ZROI &roi)
    : ZROICommand(roi), m_firstRun(true)
  {
    setText("Move Selected Control Points");
  }
  void setNewSliceROIs(const std::map<int, ZSliceROI> &nsr) { m_newSliceROIs = nsr; }
  void setChangedSlices(const std::set<int> &cs) { m_changedSlices = cs; }
  void redo() override;
protected:
  bool m_firstRun;
  std::map<int, ZSliceROI> m_newSliceROIs;
};

class ZROIMergeROICommand : public ZROICommand
{
public:
  ZROIMergeROICommand(ZROI &roi, const std::map<int, ZSliceROI> &otherSliceROIs)
    : ZROICommand(roi), m_otherSliceROIs(otherSliceROIs)
  {
    setText("Merge ROI");
  }
  void redo() override { m_changedSlices = m_roi.mergeWith_Impl(m_otherSliceROIs); }
protected:
  std::map<int, ZSliceROI> m_otherSliceROIs;
};

} // namespace nim

#endif // ZROI_H
