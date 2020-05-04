#pragma once

#include "zglobal.h"
#include "zregionontology.h"
#include <QObject>
#include <QUndoStack>
#include <map>

namespace nim {

class ZRegionAnnotation : public QObject
{
Q_OBJECT
public:
  // might throw ZIOException
  explicit ZRegionAnnotation(QObject* parent = nullptr);

  explicit ZRegionAnnotation(const QString& filename, QObject* parent = nullptr);

  ~ZRegionAnnotation() override;

  void clear();

//  ZRegionAnnotation(ZRegionAnnotation&&) = default;
//
//  ZRegionAnnotation& operator=(ZRegionAnnotation&&) = default;
//
//  ZRegionAnnotation(const ZRegionAnnotation&) = default;
//
//  ZRegionAnnotation& operator=(const ZRegionAnnotation&) = default;

  void importLabelImage(const QString& fn, FileFormat format, bool createMesh = true, bool createROI = true);

  void exportLabelImage(const QString& fn, FileFormat format, const ZImgWriteParameters& paras) const;

  size_t numRegions() const
  { return m_ontology.size(); }

  void mergeROIToRegion(const ZROI& roi, int64_t regionID);

  void mergeROIToRegion(const ZROI& roi, int slice, size_t id, int64_t regionID);

  void changeROIRegion(ZROI& roi, int slice, size_t id, int64_t regionID);

  // return nullptr if not exist
  const ZMesh* meshOfRegion(int64_t regionID) const;

  const ZROI* roiOfRegion(int64_t regionID) const;

  QString nameOfRegion(int64_t regionID) const;

  const ZBBox<glm::ivec4>& boundBox() const
  { return m_boundBox; }

  const ZTree<RegionNode>& annotationTree() const
  { return m_ontology; }

  ZTree<RegionNode>& annotationTree()
  { return m_ontology; }

  QUndoStack* undoStack()
  { return &m_undoStack; }

  // qt style read write name filter for filedialog
  static QString fileExtension()
  { return ".reganno"; }

  static bool canReadFile(const QString& filename)
  { return filename.endsWith(".reganno", Qt::CaseInsensitive); }

  static bool canWriteFile(const QString& filename)
  { return filename.endsWith(".reganno", Qt::CaseInsensitive); }

  static QString getQtReadNameFilter()
  { return QString("Region Annotation files (*.reganno)"); }

  static QString getQtWriteNameFilter()
  { return QString("Region Annotation files (*.reganno)"); }

  // might throw ZIOException
  void load(const QString& filename);

  void save(const QString& filename) const;

  // update Mesh after editing contours
  void updateMesh();

  // apply transformation to mesh
  void transformMesh(const glm::mat4& transformation);

  ZBBox<glm::ivec4> copiedItemBoundBox() const;

signals:

  void boundBoxChanged();

  //void modified();
  void regionROIAdded(int64_t id, ZROI* roi);

  void allMeshChanged();

  void allROIChanged();

  void undoStackCleanChanged(bool clean);

private:
  void updateMesh_Impl(const ZTree<RegionNode>& newOntology);

  void transformMesh_Impl(const glm::mat4& trans);

  ZTree<RegionNode> copyAnnotationTreeWithDeepCopyedMesh() const;

  void updateBoundBox();

  std::shared_ptr<ZROI> createROI();

private:
  friend class ZRegionAnnotationUpdateMeshCommand;
  friend class ZRegionAnnotationTransformMeshCommand;

  double m_voxelSizeX; //todo : these fields should always be available
  double m_voxelSizeY;
  double m_voxelSizeZ;
  ZTree<RegionNode> m_ontology;
  ZBBox<glm::ivec4> m_boundBox;

  QUndoStack m_undoStack;
};

class ZRegionAnnotationUpdateMeshCommand : public QUndoCommand
{
public:
  explicit ZRegionAnnotationUpdateMeshCommand(ZRegionAnnotation& ra)
    : QUndoCommand(), m_regionAnnotation(ra), m_oldOntology(m_regionAnnotation.m_ontology), m_firstRun(true)
  {}

  void setNewOntology(const ZTree<RegionNode>& no)
  { m_newOntology = no; }

  void undo() override
  { m_regionAnnotation.updateMesh_Impl(m_oldOntology); }

  void redo() override;

protected:
  ZRegionAnnotation& m_regionAnnotation;
  ZTree<RegionNode> m_oldOntology;
  ZTree<RegionNode> m_newOntology;
  bool m_firstRun;
};

class ZRegionAnnotationTransformMeshCommand : public QUndoCommand
{
public:
  explicit ZRegionAnnotationTransformMeshCommand(ZRegionAnnotation& ra, const glm::mat4& trans)
    : QUndoCommand(), m_regionAnnotation(ra), m_trans(trans)
    , m_oldOntology(m_regionAnnotation.copyAnnotationTreeWithDeepCopyedMesh())
  {}

  void undo() override
  { m_regionAnnotation.updateMesh_Impl(m_oldOntology); }

  void redo() override;

protected:
  ZRegionAnnotation& m_regionAnnotation;
  glm::mat4 m_trans;
  ZTree<RegionNode> m_oldOntology;
};

} // namespace nim

