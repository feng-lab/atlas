#include "zregionannotation.h"

#include <QStandardPaths>
#include "zexception.h"
#include "zioutils.h"
#include "QsLog.h"
#include "zimgconnectedcomponents.h"
#include "zimgsigneddistancemap.h"
#include "zimgfillhole.h"
#include "zbenchtimer.h"
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QTemporaryDir>

//#include <CGAL/Surface_mesh_default_triangulation_3.h>
//#include <CGAL/Surface_mesh_default_criteria_3.h>
//#include <CGAL/Complex_2_in_triangulation_3.h>
//#include <CGAL/make_surface_mesh.h>
//#include <CGAL/Gray_level_image_3.h>
//#include <CGAL/Implicit_surface_3.h>
//#include <CGAL/exceptions.h>

#include <opencv2/imgproc.hpp>

#include <vtkDiscreteMarchingCubes.h>
#include <vtkWindowedSincPolyDataFilter.h>
#include <vtkMaskFields.h>
#include <vtkThreshold.h>
#include <vtkGeometryFilter.h>
#include <vtkSmartPointer.h>
#include <vtkImageData.h>
#include <vtkPointData.h>
#include <vtkUnstructuredGrid.h>
#include <vtkCellArray.h>
#include <vtkQuadricDecimation.h>

namespace nim {

using namespace nim;

#if 0
template<typename C2t3>
void cgalMeshToVerticesIndices(const C2t3 &c2t3, std::vector<glm::vec3> &vertices, std::vector<GLuint> &indices)
{
  vertices.clear();
  indices.clear();
  using CGAL::Surface_mesher::number_of_facets_on_surface;

  typedef typename C2t3::Triangulation Tr;
  typedef typename Tr::Finite_facets_iterator Finite_facets_iterator;
  typedef typename Tr::Finite_vertices_iterator Finite_vertices_iterator;
  typedef typename Tr::Facet Facet;
  typedef typename Tr::Edge Edge;
  typedef typename Tr::Vertex_handle Vertex_handle;

  // Header.
  const Tr& tr = c2t3.triangulation();

  CGAL_assertion(c2t3.number_of_facets() == number_of_facets_on_surface(tr));

  // Finite vertices coordinates.
  std::map<Vertex_handle, int> V;
  int inum = 0;
  for(Finite_vertices_iterator vit = tr.finite_vertices_begin();
      vit != tr.finite_vertices_end();
      ++vit)
  {
    V[vit] = inum++;
    vertices.push_back(glm::vec3(vit->point().x(), vit->point().y(), vit->point().z()));
  }

  Finite_facets_iterator fit = tr.finite_facets_begin();
  std::set<Facet> oriented_set;
  std::stack<Facet> stack;

  typename Tr::size_type number_of_facets = c2t3.number_of_facets();

  //CGAL_assertion_code(typename Tr::size_type nb_facets = 0; )

  while (oriented_set.size() != number_of_facets)
  {
    while ( fit->first->is_facet_on_surface(fit->second) == false ||
            oriented_set.find(*fit) != oriented_set.end() ||

            oriented_set.find(c2t3.opposite_facet(*fit)) !=
            oriented_set.end() )
    {
      ++fit;
    }
    oriented_set.insert(*fit);
    stack.push(*fit);
    while(! stack.empty() )
    {
      Facet f = stack.top();
      stack.pop();
      for(int ih = 0 ; ih < 3 ; ++ih) {
        const int i1  = tr.vertex_triple_index(f.second, tr. cw(ih));
        const int i2  = tr.vertex_triple_index(f.second, tr.ccw(ih));

        const typename C2t3::Face_status face_status
            = c2t3.face_status(Edge(f.first, i1, i2));
        if(face_status == C2t3::REGULAR) {
          Facet fn = c2t3.neighbor(f, ih);
          if (oriented_set.find(fn) == oriented_set.end()) {
            if(oriented_set.find(c2t3.opposite_facet(fn)) == oriented_set.end())
            {
              oriented_set.insert(fn);
              stack.push(fn);
            }
            else {
             // non-orientable
            }
          }
        }
        else if(face_status != C2t3::BOUNDARY) {
          // non manifold, thus non-orientable
        }
      } // end "for each neighbor of f"
    } // end "stack non empty"
  } // end "oriented_set not full"

  for(typename std::set<Facet>::const_iterator fit =
      oriented_set.begin();
      fit != oriented_set.end();
      ++fit)
  {
    const typename Tr::Cell_handle cell = fit->first;
    const int& index = fit->second;
    const int index1 = V[cell->vertex(tr.vertex_triple_index(index, 0))];
    const int index2 = V[cell->vertex(tr.vertex_triple_index(index, 1))];
    const int index3 = V[cell->vertex(tr.vertex_triple_index(index, 2))];
    indices.push_back(index1);
    indices.push_back(index3);
    indices.push_back(index2);
    //CGAL_assertion_code(++nb_facets);
  }

  //CGAL_assertion(nb_facets == number_of_facets);
}

void binaryImgToMesh1(const ZImg &img, ZMesh &msh)
{
  std::vector<glm::dvec3> centers;
  std::vector<double> squaredRadius;

  ZImgConnectedComponents<> connComp;
  ConnComp CC = connComp.runLabel(img, 0, 1);
  ZImgSignedDistanceMap<> distMap;
  distMap.setUseSquaredDistance(true);
  ZImg distImg = distMap.run<float>(img, false);

  // get center and radius for each region
  for (size_t i=0; i<CC.voxelIdxList.size(); ++i) {
    ZVoxelCoordinate minCoord(ZVoxelCoordinate::Init::Maximum);
    ZVoxelCoordinate maxCoord(ZVoxelCoordinate::Init::Minimum);
    float minDist = std::numeric_limits<float>::max();
    ZVoxelCoordinate minDistCoord;
    for (size_t j=0; j<CC.voxelIdxList[i].size(); ++j) {
      float dist = distImg.value<float>(CC.voxelIdxList[i][j]);
      ZVoxelCoordinate coord = img.indexToCoord(CC.voxelIdxList[i][j]);
      if (dist < minDist) {
        minDist = dist;
        minDistCoord = coord;
      }
      minCoord = nim::min(minCoord, coord);
      maxCoord = nim::max(maxCoord, coord);
    }
    centers.push_back(glm::dvec3(minDistCoord.x, minDistCoord.y, minDistCoord.z));
    double radius = std::max(maxCoord.z - minCoord.z + 1, maxCoord.y - minCoord.y + 1);
    radius = std::max(static_cast<double>(maxCoord.x - minCoord.x + 1), radius);
    squaredRadius.push_back(radius * radius);
  }

  CGAL::Image_3 image3(_createImage(img.width(), img.height(), img.depth(), 1,
                                   //img.voxelSizeX(), img.voxelSizeY(), img.voxelSizeZ(),
                                    1, 1, 1,
                                   img.voxelByteNumber(), WK_FIXED, (img.voxelFormat() == VoxelFormat::Unsigned) ? SGN_UNSIGNED : SGN_SIGNED));
  memcpy(image3.data(), img.channelData(0), img.channelByteNumber());

  // default triangulation for Surface_mesher
  typedef CGAL::Surface_mesh_default_triangulation_3 Tr;
  // c2t3
  typedef CGAL::Complex_2_in_triangulation_3<Tr> C2t3;
  typedef Tr::Geom_traits GT;
  typedef CGAL::Gray_level_image_3<GT::FT, GT::Point_3> Gray_level_image;
  typedef CGAL::Implicit_surface_3<GT, Gray_level_image> Surface_3;

  std::vector<glm::vec3> allVertices;
  std::vector<GLuint> allIndices;

  Tr tr;            // 3D-Delaunay triangulation
  C2t3 c2t3(tr);   // 2D-complex in 3D-Delaunay triangulation
  // the 'function' is a 3D gray level image
  Gray_level_image image(image3, 1);
  // defining meshing criteria
  CGAL::Surface_mesh_default_criteria_3<Tr> criteria(5., 1., 1.);

  for (size_t i=0; i<centers.size(); ++i) {
    try {
      // Carefully choosen bounding sphere: the center must be inside the
      // surface defined by 'image' and the radius must be high enough so that
      // the sphere actually bounds the whole image.
      LINFO() << toQString(centers[i]) << squaredRadius[i];
      GT::Point_3 bounding_sphere_center(centers[i].x, centers[i].y, centers[i].z);
      GT::FT bounding_sphere_squared_radius = squaredRadius[i];
      GT::Sphere_3 bounding_sphere(bounding_sphere_center, bounding_sphere_squared_radius);
      // definition of the surface, with 10^-5 as relative precision
      Surface_3 surface(image, bounding_sphere, 1e-3);
      // meshing surface, with the "manifold without boundary" algorithm
      CGAL::make_surface_mesh(c2t3, surface, criteria, CGAL::Manifold_tag());

      std::vector<glm::vec3> vertices;
      std::vector<GLuint> indices;
      cgalMeshToVerticesIndices(c2t3, vertices, indices);
      GLuint startIndex = allVertices.size();
      for (GLuint& v : indices)
        v += startIndex;
      allVertices.insert(allVertices.end(), vertices.begin(), vertices.end());
      allIndices.insert(allIndices.end(), indices.begin(), indices.end());
    }
    catch (const CGAL::Failure_exception& e) {
      LERROR() << e.what();
    }
  }

  msh.clear();
  msh.setVertices(allVertices);
  msh.setIndices(allIndices);
  msh.generateNormals();
}
#endif

void binaryImgToMesh(const ZImg &img, ZMesh &msh)
{
  vtkSmartPointer<vtkImageData> vimg = vtkSmartPointer<vtkImageData>::New();
  vimg->SetExtent(0, img.width(), 0, img.height(), 0, img.depth());
  vimg->SetSpacing(1, 1, 1);
  vimg->SetOrigin(0, 0, 0);
  vimg->AllocateScalars(VTK_UNSIGNED_CHAR, 1);
  memset(vimg->GetScalarPointer(), 0, (img.width()+1)*(img.height()+1)*(img.depth()+1));

  for (size_t z=0; z<img.depth(); ++z) {
    for (size_t y=0; y<img.height(); ++y) {
      memcpy(static_cast<uint8_t*>(vimg->GetScalarPointer(0,y,z)), img.rowData(y,z,0,0), img.rowByteNumber());
    }
  }

  vtkSmartPointer<vtkDiscreteMarchingCubes> discreteCubes = vtkSmartPointer<vtkDiscreteMarchingCubes>::New();
  discreteCubes->SetInputData(vimg);
  discreteCubes->GenerateValues(1, 1, 1);

  vtkSmartPointer<vtkWindowedSincPolyDataFilter> smoother = vtkSmartPointer<vtkWindowedSincPolyDataFilter>::New();
  smoother->SetInputConnection(discreteCubes->GetOutputPort());
  smoother->SetNumberOfIterations(15);
  smoother->BoundarySmoothingOff();
  smoother->FeatureEdgeSmoothingOff();
  smoother->SetFeatureAngle(120);
  smoother->SetPassBand(0.001);
  smoother->NonManifoldSmoothingOn();
  smoother->NormalizeCoordinatesOn();
  smoother->Update();

  vtkSmartPointer<vtkThreshold> selector = vtkSmartPointer<vtkThreshold>::New();
  selector->SetInputConnection(smoother->GetOutputPort());
  selector->SetInputArrayToProcess(0, 0, 0,
                                   vtkDataObject::FIELD_ASSOCIATION_CELLS,
                                   vtkDataSetAttributes::SCALARS);
  selector->ThresholdBetween(1, 1);

  // Strip the scalars from the output
  vtkSmartPointer<vtkMaskFields> scalarsOff = vtkSmartPointer<vtkMaskFields>::New();
  scalarsOff->SetInputConnection(selector->GetOutputPort());
  scalarsOff->CopyAttributeOff(vtkMaskFields::POINT_DATA,
                               vtkDataSetAttributes::SCALARS);
  scalarsOff->CopyAttributeOff(vtkMaskFields::CELL_DATA,
                               vtkDataSetAttributes::SCALARS);

  vtkSmartPointer<vtkGeometryFilter> geometry = vtkSmartPointer<vtkGeometryFilter>::New();
  geometry->SetInputConnection(scalarsOff->GetOutputPort());
  geometry->Update();

  vtkPolyData* outputPolydata = geometry->GetOutput();
  size_t numTriangles = outputPolydata->GetNumberOfPolys();
  double baseRate = 0.05;
  if (numTriangles * baseRate > 250000) {
    baseRate = 250000. / numTriangles;
  }
  if (numTriangles * baseRate < 200) {
    baseRate = std::min(1., 200. / numTriangles);
  }

  LINFO() << baseRate << numTriangles;
  vtkSmartPointer<vtkQuadricDecimation> decimate = vtkSmartPointer<vtkQuadricDecimation>::New();
  decimate->SetInputConnection(geometry->GetOutputPort());
  decimate->SetTargetReduction(1.0 - baseRate);
  decimate->Update();

  outputPolydata = decimate->GetOutput();
  vtkPoints* points = outputPolydata->GetPoints();
  vtkCellArray* polys = outputPolydata->GetPolys();

  std::vector<glm::dvec3> vertices(points->GetNumberOfPoints());
  std::vector<GLuint> indices;
  for (vtkIdType id = 0; id < points->GetNumberOfPoints(); ++id) {
    points->GetPoint(id, &vertices[id][0]);
  }
  vtkIdType npts;
  vtkIdType *pts;
  for(int i=0; i<outputPolydata->GetNumberOfPolys(); ++i) {
    int h = polys->GetNextCell(npts, pts);
    if (h==0) {
      break;
    }
    if (npts==3) {
      indices.push_back(pts[0]);
      indices.push_back(pts[1]);
      indices.push_back(pts[2]);
    }
  }

  msh.clear();
  msh.setVertices(vertices);
  msh.setIndices(indices);
  msh.generateNormals();
}

struct ContourNode {
  int index;
  int parentIndex;
};

void binaryImgToROI(const ZImg &img, ZROI &roi)
{
  roi.clear();

  for (size_t s = 0; s < img.depth(); ++s) {
    int64_t min;
    int64_t max;
    ZImg simg = img.extractPlane(s, 0, 0);
    simg.computeMinMax(min, max);
    if (max == 0)
      continue;

    try {
      cv::Mat mat(simg.height(), simg.width(), CV_8UC1, simg.channelData(0));
      std::vector<std::vector<cv::Point>> contours;
      std::vector<cv::Vec4i> hierarchy;
      cv::findContours(mat, contours, hierarchy, cv::RETR_TREE, cv::CHAIN_APPROX_NONE);

      ZTree<ContourNode> contoursTree;
      std::map<int, ContourNode> nodeMap;

      for (size_t i=0; i < hierarchy.size(); ++i) {
        ContourNode node;
        node.index = i;
        node.parentIndex = hierarchy[i][3];
        nodeMap[node.index] = node;
      }

      std::map<int, ZTree<ContourNode>::Iterator> itMap;
      std::map<int, ContourNode>::iterator tmp;
      while (!nodeMap.empty()) {
        std::map<int, ContourNode>::iterator it = nodeMap.begin();
        while (it != nodeMap.end()) {
          int parentID = it->second.parentIndex;
          std::map<int, ZTree<ContourNode>::Iterator>::const_iterator nodeIt = itMap.find(parentID);
          if (nodeIt != itMap.end()) {
            itMap[it->first] = contoursTree.appendChild(nodeIt->second, it->second);
            tmp = it;
            ++it;
            nodeMap.erase(tmp);
          } else if (nodeMap.find(parentID) == nodeMap.end()) {
            itMap[it->first] = contoursTree.appendRoot(it->second);
            tmp = it;
            ++it;
            nodeMap.erase(tmp);
          } else {
            ++it;
          }
        }
      }

      for (auto it = contoursTree.cbeginBreadthFirst(); it != contoursTree.cendBreadthFirst(); ++it) {
        //LINFO() << it->index << contours[it->index].size();
        size_t c = it->index;
        if (contours[c].size() < 15) {
          continue;
        } else {
          size_t dst = std::max(size_t(1), std::min(size_t(30), contours[c].size() / 20));
          QPolygonF poly;
          for (size_t p=0; p<contours[c].size(); p+=dst) {
            poly.push_back(QPointF(contours[c][p].x, contours[c][p].y));
          }
          if (!poly.isClosed()) {
            poly.push_back(poly[0]);
          }
          if (contoursTree.numAncestors(it) % 2 == 0) {
            roi.addSpline(s, poly);
          } else {
            roi.subtractSpline(s, poly);
          }
        }
      }
    }
    catch (const cv::Exception& e) {
      LERROR() << e.what();
    }
  }
}

struct ChangeValue {
  ChangeValue(int64_t from, int64_t to)
    : from(from), to(to)
  {}
  template<typename TVoxel>
  TVoxel operator()(TVoxel current) const
  {
    return (static_cast<int64_t>(current) == from) ? static_cast<TVoxel>(to) : current;
  }
  int64_t from;
  int64_t to;
};

struct MarkAsIfOtherEqualsOtherWiseZero {
  MarkAsIfOtherEqualsOtherWiseZero(int64_t as, int64_t equal)
    : as(as), equal(equal)
  {}
  template<typename TVoxel, typename TVoxelOther>
  TVoxel operator()(TVoxel, TVoxelOther otherVoxel) const
  {
    return (static_cast<int64_t>(otherVoxel) == equal) ? as : 0;
  }
  int64_t as;
  int64_t equal;
};

struct CopyAsIfOtherIsNotZero {
  CopyAsIfOtherIsNotZero(int64_t as)
    : as(as)
  {}
  template<typename TVoxel, typename TVoxelOther>
  TVoxel operator()(TVoxel current, TVoxelOther otherVoxel) const
  {
    return (otherVoxel != 0) ? static_cast<TVoxel>(as) : current;
  }
  int64_t as;
};

}

namespace nim {

ZRegionAnnotation::ZRegionAnnotation(QObject *parent)
  : QObject(parent)
{
  clear();
  readOntology(false);
  connect(&m_undoStack, SIGNAL(cleanChanged(bool)),
          this, SIGNAL(undoStackCleanChanged(bool)));
}

ZRegionAnnotation::ZRegionAnnotation(const QString &filename, QObject *parent)
  : QObject(parent)
{
  load(filename);
  connect(&m_undoStack, SIGNAL(cleanChanged(bool)),
          this, SIGNAL(undoStackCleanChanged(bool)));
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

void ZRegionAnnotation::importLabelImage(const QString &fn, FileFormat format, bool createMesh, bool createROI)
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
  //LINFO() << origLabelImg.info().toQString();
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
  for (size_t i=0; i<origLabelImg.channelVoxelNumber(); ++i) {
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

  LINFO() << "Importing Label Image...";
  for (auto it = m_ontology.beginPost(); it != m_ontology.endPost(); ++it) {
    LINFO() << "Processing region" << it->abbreviation << it->id << "...";
    if (it->id > maxPossibleLabelInImg || it->id < minPossibleLabelInImg ||
        labels.find(it->id) == labels.end()) {
      continue;
    }

    // create binary image
    binaryImg.binaryOperation(labelImg, MarkAsIfOtherEqualsOtherWiseZero(1, it->id));
    if (it->id == 997) {
      for (size_t z=0; z<binaryImg.depth(); ++z) {
        ZImg simg = binaryImg.createView(z, 0, 0);
        ZImg fimg = imFill.run(simg);
        binaryImg.pasteImg(fimg, ZVoxelCoordinate(0,0,z));
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
      //connect(it->roi.get(), SIGNAL(roiChanged(int)), this, SIGNAL(modified()));
      //connect(it->roi.get(), SIGNAL(roiDeleted(int)), this, SIGNAL(modified()));
      //connect(it->roi.get(), SIGNAL(roiMoved(int)), this, SIGNAL(modified()));
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
  LINFO() << "Finish importing label image";

  bt.stopAndPrint();

  if (createMesh) {
    emit allMeshChanged();
  }
  if (createROI) {
    emit allROIChanged();
  }
}

void ZRegionAnnotation::exportLabelImage(const QString &fn, FileFormat format, Compression comp) const
{
  LINFO() << "Exporting Label Image...";
  ZImgInfo info(m_width, m_height, m_depth, 1, 1, 2);
  info.voxelSizeUnit = VoxelSizeUnit::um;
  info.voxelSizeX = m_voxelSizeX;
  info.voxelSizeY = m_voxelSizeY;
  info.voxelSizeZ = m_voxelSizeZ;
  ZImg res(info);
  for (auto it = m_ontology.cbeginBreadthFirst(); it != m_ontology.cendBreadthFirst(); ++it) {
    LINFO() << "Processing region" << it->abbreviation << it->id << "...";
    if (it->roi) {
      ZImg regionBinaryImg = it->roi->toMaskImg(res.width(), res.height(), res.depth(), false);
      res.binaryOperation(regionBinaryImg, CopyAsIfOtherIsNotZero(it->id));
    }
  }
  res.save(fn, format, comp);
  LINFO() << "Finish exporting label image";
}

void ZRegionAnnotation::mergeROIToRegion(const ZROI &roi, int64_t regionID)
{
  for (auto it = m_ontology.begin(); it != m_ontology.end(); ++it) {
    if (it->id == regionID) {
      if (!it->roi) {
        it->roi = std::make_shared<ZROI>(undoStack());
        //connect(it->roi.get(), SIGNAL(roiChanged(int)), this, SIGNAL(modified()));
        //connect(it->roi.get(), SIGNAL(roiDeleted(int)), this, SIGNAL(modified()));
        //connect(it->roi.get(), SIGNAL(roiMoved(int)), this, SIGNAL(modified()));
        emit regionROIAdded(it->id, it->roi.get());
      }
      it->roi->mergeWith(roi);

      for (auto pit = m_ontology.beginAncestor(it); pit != m_ontology.endAncestor(it); ++pit) {
        if (!pit->roi) {
          pit->roi = std::make_shared<ZROI>(undoStack());
          //connect(pit->roi.get(), SIGNAL(roiChanged(int)), this, SIGNAL(modified()));
          //connect(pit->roi.get(), SIGNAL(roiDeleted(int)), this, SIGNAL(modified()));
          //connect(pit->roi.get(), SIGNAL(roiMoved(int)), this, SIGNAL(modified()));
          emit regionROIAdded(it->id, it->roi.get());
        }
        pit->roi->mergeWith(roi);
      }

      return;
    }
  }
}

const ZMesh *ZRegionAnnotation::meshOfRegion(int64_t regionID)
{
  for (auto it = m_ontology.begin(); it != m_ontology.end(); ++it) {
    if (it->id == regionID) {
      return it->mesh.get();
    }
  }
  return nullptr;
}

const ZROI *ZRegionAnnotation::roiOfRegion(int64_t regionID)
{
  for (auto it = m_ontology.begin(); it != m_ontology.end(); ++it) {
    if (it->id == regionID) {
      return it->roi.get();
    }
  }
  return nullptr;
}

void ZRegionAnnotation::load(const QString &filename)
{
  clear();

  try {
    H5::Exception::dontPrint();

    H5::H5File file(qPrintable(filename), H5F_ACC_RDONLY);

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
    for (int i=0; i<numRegion; ++i) {
      H5::Group regionGrp = allGrp.openGroup(qPrintable(QString("Region%1").arg(i+1)));

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
        //connect(p.roi.get(), SIGNAL(roiChanged(int)), this, SIGNAL(modified()));
        //connect(p.roi.get(), SIGNAL(roiDeleted(int)), this, SIGNAL(modified()));
        //connect(p.roi.get(), SIGNAL(roiMoved(int)), this, SIGNAL(modified()));
      }

      if (H5Lexists(regionGrp.getId(), "Mesh", H5P_DEFAULT) > 0) {
        H5::Group meshGrp = regionGrp.openGroup("Mesh");
        p.mesh = std::make_shared<ZMesh>();
        p.mesh->load(meshGrp);
      }

      nodeMap[p.id] = p;
    }

    std::map<int64_t, ZTree<RegionNode>::Iterator> itMap;
    std::map<int64_t, RegionNode>::iterator tmp;
    while (!nodeMap.empty()) {
      std::map<int64_t, RegionNode>::iterator it = nodeMap.begin();
      while (it != nodeMap.end()) {
        int64_t parentID = it->second.parentID;
        std::map<int64_t, ZTree<RegionNode>::Iterator>::const_iterator nodeIt = itMap.find(parentID);
        if (nodeIt != itMap.end()) {
          itMap[it->first] = m_ontology.appendChild(nodeIt->second, it->second);
          tmp = it;
          ++it;
          nodeMap.erase(tmp);
        } else if (nodeMap.find(parentID) == nodeMap.end()) {
          itMap[it->first] = m_ontology.appendRoot(it->second);
          tmp = it;
          ++it;
          nodeMap.erase(tmp);
        } else {
          ++it;
        }
      }
    }
  }
  catch(H5::Exception const & e)
  {
    throw ZIOException(QString("hdf5:%1").arg(e.getDetailMsg().c_str()));
  }

  emit allMeshChanged();
  emit allROIChanged();
}

void ZRegionAnnotation::save(const QString &filename) const
{
  try {
    H5::Exception::dontPrint();

    H5::H5File file(qPrintable(filename), H5F_ACC_TRUNC);

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
      H5::Group regionGrp = allGrp.createGroup(qPrintable(QString("Region%1").arg(idx+1)));
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
  catch(H5::Exception const & e)
  {
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
    ZRegionAnnotationUpdateMeshCommand *cmd = new ZRegionAnnotationUpdateMeshCommand(*this);
    importLabelImage(fn, FileFormat::MetaImage, true, false);
    cmd->setNewOntology(m_ontology);
    m_undoStack.push(cmd);
    //emit modified();
  } else {
    throw ZException(QString("can not create temporary file for mesh updating"));
  }
}

void ZRegionAnnotation::updateMesh_Impl(const ZTree<RegionNode> &newOntology)
{
  auto it = m_ontology.begin();
  auto itn = newOntology.begin();
  for ( ; it != m_ontology.end(); ++it, ++itn) {
    it->mesh = itn->mesh;
  }
  emit allMeshChanged();
}

void ZRegionAnnotation::readOntology(bool readAll)
{
  QString ontologyFilename = ":/Resources/ontology/mouse_brain_atlas.json";
  QFile file(ontologyFilename);
  if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
    throw ZIOException(tr("Can not open ontology file"));
  }

  QByteArray saveData = file.readAll();
  QJsonDocument loadDoc(QJsonDocument::fromJson(saveData));
  if (loadDoc.isNull() || loadDoc.isEmpty() || !loadDoc.isObject()) {
    throw ZIOException(tr("File format is incorrect"));
  }
  QJsonObject loadObj = loadDoc.object();
  if (!loadObj.contains("msg") || !loadObj["msg"].isArray() || !loadObj["msg"].toArray().first().isObject()) {
    throw ZIOException(tr("File is not %1 format").arg("ontology"));
  }
  QJsonObject rootObj = loadObj["msg"].toArray().first().toObject();
  RegionNode node;
  QJsonArray children;
  for (QJsonObject::const_iterator it = rootObj.constBegin(); it != rootObj.constEnd(); ++it) {
    if (it.key() == "id") {
      node.id = it.value().toInt(-1);
    } else if (it.key() == "parent_structure_id") {
      node.parentID = -1;
    } else if (it.key() == "acronym") {
      node.abbreviation = it.value().toString();
    } else if (it.key() == "name") {
      node.name = it.value().toString();
    } else if (it.key() == "color_hex_triplet") {
      QString colorStr = it.value().toString();
      assert(colorStr.size() == 6);
      bool ok;
      node.red = colorStr.mid(0,2).toInt(&ok, 16);
      assert(ok);
      node.green = colorStr.mid(2,2).toInt(&ok, 16);
      assert(ok);
      node.blue = colorStr.mid(4,2).toInt(&ok, 16);
      assert(ok);
    } else if (it.key() == "children") {
      assert(it.value().isArray());
      children = it.value().toArray();
    }
  }
  ZTree<RegionNode>::Iterator currIt;
  if (readAll) {
    currIt = m_ontology.appendRoot(node);
  }
  for (QJsonArray::const_iterator it = children.constBegin(); it != children.constEnd(); ++it) {
    assert((*it).isObject());
    readOntology((*it).toObject(), currIt);
  }
}

void ZRegionAnnotation::updateBoundBox()
{
  if (m_width <= 0 || m_height <= 0 || m_depth <= 0) {
    m_boundBox[0] = m_boundBox[2] = m_boundBox[4] = m_boundBox[6] = std::numeric_limits<int>::max();
    m_boundBox[1] = m_boundBox[3] = m_boundBox[5] = m_boundBox[7] = std::numeric_limits<int>::min();
  } else {
    m_boundBox[0] = 0;
    m_boundBox[1] = m_width-1;
    m_boundBox[2] = 0;
    m_boundBox[3] = m_height-1;
    m_boundBox[4] = 0;
    m_boundBox[5] = (m_depth-1);
    m_boundBox[6] = 0;
    m_boundBox[7] = 0;
  }
  emit boundBoxChanged();
}

void ZRegionAnnotation::readOntology(const QJsonObject &obj, ZTree<RegionNode>::Iterator &parentIt)
{
  RegionNode node;
  QJsonArray children;
  for (QJsonObject::const_iterator it = obj.constBegin(); it != obj.constEnd(); ++it) {
    if (it.key() == "id") {
      node.id = it.value().toInt(-1);
    } else if (it.key() == "parent_structure_id") {
      node.parentID = it.value().toInt(-1);
    } else if (it.key() == "acronym") {
      node.abbreviation = it.value().toString();
    } else if (it.key() == "name") {
      node.name = it.value().toString();
    } else if (it.key() == "color_hex_triplet") {
      QString colorStr = it.value().toString();
      assert(colorStr.size() == 6);
      bool ok;
      node.red = colorStr.mid(0,2).toInt(&ok, 16);
      assert(ok);
      node.green = colorStr.mid(2,2).toInt(&ok, 16);
      assert(ok);
      node.blue = colorStr.mid(4,2).toInt(&ok, 16);
      assert(ok);
    } else if (it.key() == "children") {
      assert(it.value().isArray());
      children = it.value().toArray();
    }
  }
  if (!m_ontology.isNull(parentIt)) {
    ZTree<RegionNode>::Iterator currIt = m_ontology.appendChild(parentIt, node);
    for (QJsonArray::const_iterator it = children.constBegin(); it != children.constEnd(); ++it) {
      assert((*it).isObject());
      readOntology((*it).toObject(), currIt);
    }
  } else {
    ZTree<RegionNode>::Iterator currIt;
    if (node.abbreviation.compare("GPe", Qt::CaseInsensitive) == 0 ||
        node.abbreviation.compare("STN", Qt::CaseInsensitive) == 0 ||
        node.abbreviation.compare("SNr", Qt::CaseInsensitive) == 0 ||
        node.abbreviation.compare("STRv", Qt::CaseInsensitive) == 0 ||
        node.abbreviation.compare("STRd", Qt::CaseInsensitive) == 0 ||
        node.abbreviation.compare("GPi", Qt::CaseInsensitive) == 0 ||
        node.abbreviation.compare("SPF", Qt::CaseInsensitive) == 0) {
      currIt = m_ontology.appendRoot(node);
    }
    for (QJsonArray::const_iterator it = children.constBegin(); it != children.constEnd(); ++it) {
      assert((*it).isObject());
      readOntology((*it).toObject(), currIt);
    }
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

} // namespace nim
