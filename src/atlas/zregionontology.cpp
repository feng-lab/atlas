#include "zregionontology.h"

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QObject>
#include "zexception.h"

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

void readOntology(const QJsonObject &obj, ZTree<RegionNode>::Iterator &parentIt,
                  const QStringList& regionAbbrevs, ZTree<RegionNode> &ontology)
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
  if (!ontology.isNull(parentIt)) {
    ZTree<RegionNode>::Iterator currIt = ontology.appendChild(parentIt, node);
    for (QJsonArray::const_iterator it = children.constBegin(); it != children.constEnd(); ++it) {
      assert((*it).isObject());
      readOntology((*it).toObject(), currIt, regionAbbrevs, ontology);
    }
  } else {
    ZTree<RegionNode>::Iterator currIt;
    if (regionAbbrevs.contains(node.abbreviation, Qt::CaseInsensitive)) {
      currIt = ontology.appendRoot(node);
    }
    for (QJsonArray::const_iterator it = children.constBegin(); it != children.constEnd(); ++it) {
      assert((*it).isObject());
      readOntology((*it).toObject(), currIt, regionAbbrevs, ontology);
    }
  }
}

void readMouseBrainAtlasOntology(ZTree<RegionNode> &ontology)
{
  ontology.clear();
  QString ontologyFilename = ":/Resources/ontology/mouse_brain_atlas.json";
  QFile file(ontologyFilename);
  if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
    throw ZIOException(QObject::tr("Can not open ontology file"));
  }

  QByteArray saveData = file.readAll();
  QJsonDocument loadDoc(QJsonDocument::fromJson(saveData));
  if (loadDoc.isNull() || loadDoc.isEmpty() || !loadDoc.isObject()) {
    throw ZIOException(QObject::tr("File format is incorrect"));
  }
  QJsonObject loadObj = loadDoc.object();
  if (!loadObj.contains("msg") || !loadObj["msg"].isArray() || !loadObj["msg"].toArray().first().isObject()) {
    throw ZIOException(QObject::tr("File is not %1 format").arg("ontology"));
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
  ZTree<RegionNode>::Iterator currIt = ontology.appendRoot(node);
  for (QJsonArray::const_iterator it = children.constBegin(); it != children.constEnd(); ++it) {
    assert((*it).isObject());
    readOntology((*it).toObject(), currIt, QStringList(), ontology);
  }
}

void readMouseBrainAtlasOntology(const QStringList &regionAbbrevs, ZTree<RegionNode> &ontology)
{
  ontology.clear();
  QString ontologyFilename = ":/Resources/ontology/mouse_brain_atlas.json";
  QFile file(ontologyFilename);
  if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
    throw ZIOException(QObject::tr("Can not open ontology file"));
  }

  QByteArray saveData = file.readAll();
  QJsonDocument loadDoc(QJsonDocument::fromJson(saveData));
  if (loadDoc.isNull() || loadDoc.isEmpty() || !loadDoc.isObject()) {
    throw ZIOException(QObject::tr("File format is incorrect"));
  }
  QJsonObject loadObj = loadDoc.object();
  if (!loadObj.contains("msg") || !loadObj["msg"].isArray() || !loadObj["msg"].toArray().first().isObject()) {
    throw ZIOException(QObject::tr("File is not %1 format").arg("ontology"));
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
  ZTree<RegionNode>::Iterator nullIt;
  for (QJsonArray::const_iterator it = children.constBegin(); it != children.constEnd(); ++it) {
    assert((*it).isObject());
    readOntology((*it).toObject(), nullIt, regionAbbrevs, ontology);
  }
}

int64_t idOfRegionAbbreviation(const QString &abbreviation, const ZTree<RegionNode> &ontology)
{
  for (auto it = ontology.begin(); it != ontology.end(); ++it) {
    if (it->abbreviation == abbreviation) {
      return it->id;
    }
  }
  throw ZException(QString("can not find region %1").arg(abbreviation));
  return 0;
}

std::vector<int64_t> allIDsWithinRegionAbbreviation(const QString &abbreviation, const ZTree<RegionNode> &ontology)
{
  std::vector<int64_t> res;
  for (auto it = ontology.begin(); it != ontology.end(); ++it) {
    if (it->abbreviation == abbreviation) {
      for (auto ait = ontology.begin(it); ait != ontology.end(it); ++ait) {
        res.push_back(ait->id);
      }
    }
  }
  if (res.empty()) {
    throw ZException(QString("can not find region %1").arg(abbreviation));
  }
  return res;
}

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
  assert(img.isType<uint8_t>() && !img.isTimeSeries() && !img.isMultiChannelsImg());
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
  assert(img.isType<uint8_t>() && !img.isTimeSeries() && !img.isMultiChannelsImg());
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

} // namespace nim
