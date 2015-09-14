#ifndef ZREGIONANNOTATION_H
#define ZREGIONANNOTATION_H

#include "zglobal.h"
#include "ztree.hpp"
#include <QStringList>
#include <map>
#include "zroi.h"
#include <memory>
#include "zmesh.h"
#include <QObject>
#include <QJsonObject>
#include <QUndoStack>

namespace nim {

struct RegionNode
{
  RegionNode(int64_t id = -1, int64_t parentID = -1, int red = 0, int green = 0, int blue = 0,
             QString name = "", QString abbreviation = "")
    : id(id), parentID(parentID), red(red), green(green), blue(blue)
    , name(name), abbreviation(abbreviation)
  {}

  int64_t id;
  int64_t parentID;
  int red;
  int green;
  int blue;
  QString name;
  QString abbreviation;
  std::shared_ptr<ZROI> roi;
  std::shared_ptr<ZMesh> mesh;

  bool operator==(const RegionNode &rhs) const
  {
    return id == rhs.id &&
        parentID == rhs.parentID &&
        red == rhs.red &&
        green == rhs.green &&
        blue == rhs.blue &&
        name == rhs.name &&
        abbreviation == rhs.abbreviation;
  }
  inline bool operator!=(const RegionNode &rhs) const { return !(*this == rhs); }
};

class ZRegionAnnotation : public QObject
{
  Q_OBJECT
public:
  // might throw ZIOException
  ZRegionAnnotation(QObject *parent = nullptr);
  explicit ZRegionAnnotation(const QString &filename, QObject *parent = nullptr);
  ~ZRegionAnnotation();

  void clear();

#ifndef _USE_MSVC2013_
  ZRegionAnnotation(ZRegionAnnotation&&) = default;
  ZRegionAnnotation& operator=(ZRegionAnnotation&&) = default;
#endif
  ZRegionAnnotation(const ZRegionAnnotation&) = default;
  ZRegionAnnotation& operator=(const ZRegionAnnotation&) = default;

  void importLabelImage(const QString &fn, FileFormat format, bool createMesh = true, bool createROI = true);
  void exportLabelImage(const QString &fn, FileFormat format, Compression comp) const;

  size_t numRegions() const { return m_ontology.size(); }

  void mergeROIToRegion(const ZROI &roi, int64_t regionID);

  // return nullptr if not exist
  const ZMesh* meshOfRegion(int64_t regionID);
  const ZROI* roiOfRegion(int64_t regionID);

  const std::vector<int>& boundBox() const { return m_boundBox; }
  const ZTree<RegionNode>& annotationTree() const { return m_ontology; }
  ZTree<RegionNode>& annotationTree() { return m_ontology; }

  QUndoStack* undoStack() { return &m_undoStack; }

  // qt style read write name filter for filedialog
  static QString fileExtension() { return ".reganno"; }
  static bool canReadFile(const QString& filename) { return filename.endsWith(".reganno", Qt::CaseInsensitive); }
  static bool canWriteFile(const QString& filename) { return filename.endsWith(".reganno", Qt::CaseInsensitive); }
  static QString getQtReadNameFilter() { return QString("Region Annotation files (*.reganno)"); }
  static QString getQtWriteNameFilter()  { return QString("Region Annotation files (*.reganno)"); }
  // might throw ZIOException
  void load(const QString &filename);
  void save(const QString &filename) const;

signals:
  void boundBoxChanged();
  //void modified();
  void regionROIAdded(int64_t id, ZROI *roi);
  void allMeshChanged();
  void allROIChanged();

  void undoStackCleanChanged(bool clean);

public slots:
  // update Mesh after editing contours
  void updateMesh();
  void updateMesh_Impl(const ZTree<RegionNode> &newOntology);

private:
  void readOntology(bool readAll = true);
  void updateBoundBox();

  void readOntology(const QJsonObject &obj, ZTree<RegionNode>::Iterator &parentIt);

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
  ZRegionAnnotationUpdateMeshCommand(ZRegionAnnotation &ra)
    : QUndoCommand(), m_regionAnnotation(ra), m_oldOntology(m_regionAnnotation.m_ontology), m_firstRun(true)
  {}
  void setNewOntology(const ZTree<RegionNode> &no) { m_newOntology = no; }
  void undo() override { m_regionAnnotation.updateMesh_Impl(m_oldOntology); }
  void redo() override;
protected:
  ZRegionAnnotation& m_regionAnnotation;
  ZTree<RegionNode> m_oldOntology;
  ZTree<RegionNode> m_newOntology;
  bool m_firstRun;
};

} // namespace nim

#endif // ZREGIONANNOTATION_H
