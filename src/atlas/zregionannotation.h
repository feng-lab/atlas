#ifndef ZREGIONANNOTATION_H
#define ZREGIONANNOTATION_H

#include "zglobal.h"
#include <map>
#include <QObject>
#include <QUndoStack>
#include "zregionontology.h"

namespace nim {

class ZRegionAnnotation : public QObject
{
Q_OBJECT
public:
  // might throw ZIOException
  ZRegionAnnotation(QObject* parent = nullptr);

  explicit ZRegionAnnotation(const QString& filename, QObject* parent = nullptr);

  ~ZRegionAnnotation();

  void clear();

  ZRegionAnnotation(ZRegionAnnotation&&) = default;

  ZRegionAnnotation& operator=(ZRegionAnnotation&&) = default;

  ZRegionAnnotation(const ZRegionAnnotation&) = default;

  ZRegionAnnotation& operator=(const ZRegionAnnotation&) = default;

  void importLabelImage(const QString& fn, FileFormat format, bool createMesh = true, bool createROI = true);

  void exportLabelImage(const QString& fn, FileFormat format, Compression comp) const;

  size_t numRegions() const
  { return m_ontology.size(); }

  void mergeROIToRegion(const ZROI& roi, int64_t regionID);

  // return nullptr if not exist
  const ZMesh* meshOfRegion(int64_t regionID);

  const ZROI* roiOfRegion(int64_t regionID);

  const std::vector<int>& boundBox() const
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

signals:

  void boundBoxChanged();

  //void modified();
  void regionROIAdded(int64_t id, ZROI* roi);

  void allMeshChanged();

  void allROIChanged();

  void undoStackCleanChanged(bool clean);

private:
  void updateMesh_Impl(const ZTree<RegionNode>& newOntology);

  void updateBoundBox();

private:
  friend class ZRegionAnnotationUpdateMeshCommand;

  int m_width;
  int m_height;
  int m_depth;
  double m_voxelSizeX; //todo : these fields should always be available
  double m_voxelSizeY;
  double m_voxelSizeZ;
  ZTree<RegionNode> m_ontology;
  std::vector<int> m_boundBox;

  QUndoStack m_undoStack;
};

class ZRegionAnnotationUpdateMeshCommand : public QUndoCommand
{
public:
  ZRegionAnnotationUpdateMeshCommand(ZRegionAnnotation& ra)
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

} // namespace nim

#endif // ZREGIONANNOTATION_H
