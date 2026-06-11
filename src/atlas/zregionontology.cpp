#include "zregionontology.h"

#include "zexception.h"
#include "zjson.h"
#include "zlog.h"
#include "zsysteminfo.h"
#include "znaturalcubicspline2d.h"
// #include <CGAL/Surface_mesh_default_triangulation_3.h>
// #include <CGAL/Surface_mesh_default_criteria_3.h>
// #include <CGAL/Complex_2_in_triangulation_3.h>
// #include <CGAL/make_surface_mesh.h>
// #include <CGAL/Gray_level_image_3.h>
// #include <CGAL/Implicit_surface_3.h>
// #include <CGAL/exceptions.h>
#include <opencv2/geometry.hpp>
#include <opencv2/imgproc.hpp>
#include <vtkDiscreteMarchingCubes.h>
#include <vtkDiscreteFlyingEdges3D.h>
#include <vtkWindowedSincPolyDataFilter.h>
#include <vtkMaskFields.h>
#include <vtkThreshold.h>
#include <vtkGeometryFilter.h>
#include <vtkImageData.h>
#include <vtkSmartPointer.h>
#include <vtkPointData.h>
#include <vtkUnstructuredGrid.h>
#include <vtkCellArray.h>
#include <vtkQuadricDecimation.h>
#include <vtkPolyDataNormals.h>
#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <vector>

namespace nim {

void readOntology(const json::object& obj,
                  ZTree<RegionNode>::Iterator& parentIt,
                  const QStringList& regionAbbrevs,
                  ZTree<RegionNode>& ontology)
{
  RegionNode node;
  const json::array* children = nullptr;
  for (const auto& [key, value] : obj) {
    if (key == "id") {
      node.id = json::value_to<int64_t>(value);
      for (const auto& nd : ontology) {
        if (nd.id == node.id) {
          throw ZException(fmt::format("id {} used more than once", nd.id));
        }
      }
    } else if (key == "parent_structure_id") {
      if (!value.is_number()) {
        node.parentID = 0;
      } else {
        node.parentID = json::value_to<int64_t>(value);
      }
    } else if (key == "acronym") {
      node.abbreviation = asQString(value);
      if (node.abbreviation.isEmpty()) {
        throw ZException("node acronym can not be empty");
      }
    } else if (key == "name") {
      node.name = asQString(value);
      if (node.name.isEmpty()) {
        throw ZException("node name can not be empty");
      }
    } else if (key == "color_hex_triplet") {
      QString colorStr = asQString(value);
      if (colorStr.size() != 6) {
        throw ZException(fmt::format("wrong color string: {}", colorStr));
      }
      bool ok;
      node.red = colorStr.mid(0, 2).toInt(&ok, 16);
      if (!ok) {
        throw ZException(fmt::format("wrong color string: {}", colorStr));
      }
      node.green = colorStr.mid(2, 2).toInt(&ok, 16);
      if (!ok) {
        throw ZException(fmt::format("wrong color string: {}", colorStr));
      }
      node.blue = colorStr.mid(4, 2).toInt(&ok, 16);
      if (!ok) {
        throw ZException(fmt::format("wrong color string: {}", colorStr));
      }
    } else if (key == "children") {
      if (!value.is_array()) {
        throw ZException("children is not array");
      }
      children = &value.as_array();
    }
  }
  if (!ZTree<RegionNode>::isNull(parentIt)) {
    if (node.parentID != parentIt->id) {
      throw ZException(
        fmt::format("node {} has wrong parent id {} (should be {})", node.id, node.parentID, parentIt->id));
    }
    ZTree<RegionNode>::Iterator currIt = ontology.appendChild(parentIt, node);
    if (!children) {
      return;
    }
    for (const auto& child : *children) {
      if (!child.is_object()) {
        throw ZException("child is not object");
      }
      readOntology(child.as_object(), currIt, regionAbbrevs, ontology);
    }
  } else {
    ZTree<RegionNode>::Iterator currIt;
    if (regionAbbrevs.empty() || regionAbbrevs.contains(node.abbreviation, Qt::CaseInsensitive)) {
      currIt = ontology.appendRoot(node);
    }
    if (!children) {
      return;
    }
    for (const auto& child : *children) {
      if (!child.is_object()) {
        throw ZException("child is not object");
      }
      readOntology(child.as_object(), currIt, regionAbbrevs, ontology);
    }
  }
}

void readMouseBrainAtlasOntology(ZTree<RegionNode>& ontology)
{
  ontology.clear();
  QString ontologyFilename = ZSystemInfo::resourcesDirPath() + "/ontology/lemur_atlas_ontology_v5.json";
  auto loadObj = loadJsonObject(ontologyFilename);
  if (!loadObj.contains("msg") || !loadObj.at("msg").is_array() || loadObj.at("msg").as_array().empty() ||
      !loadObj.at("msg").as_array()[0].is_object()) {
    throw ZException("File is not ontology format");
  }
  auto& rootObj = loadObj.at("msg").as_array()[0].as_object();
  ZTree<RegionNode>::Iterator nullIt;
  readOntology(rootObj, nullIt, QStringList(), ontology);
}

void readMouseBrainAtlasOntology(const QStringList& regionAbbrevs, ZTree<RegionNode>& ontology)
{
  ontology.clear();
  // QString ontologyFilename = ZSystemInfo::resourcesDirPath() + "/ontology/mouse_brain_atlas.json";
  QString ontologyFilename = ZSystemInfo::resourcesDirPath() + "/ontology/lemur_atlas_ontology_v5.json";
  auto loadObj = loadJsonObject(ontologyFilename);
  if (!loadObj.contains("msg") || !loadObj.at("msg").is_array() || loadObj.at("msg").as_array().empty() ||
      !loadObj.at("msg").as_array()[0].is_object()) {
    throw ZException("File is not ontology format");
  }
  auto& rootObj = loadObj.at("msg").as_array()[0].as_object();
  ZTree<RegionNode>::Iterator nullIt;
  readOntology(rootObj, nullIt, regionAbbrevs, ontology);

  RegionNode node;
  ontology.appendRoot(node);
}

int64_t idOfRegionAbbreviation(const QString& abbreviation, const ZTree<RegionNode>& ontology)
{
  for (const auto& node : ontology) {
    if (node.abbreviation == abbreviation) {
      return node.id;
    }
  }
  throw ZException(fmt::format("can not find region {}", abbreviation));
}

std::vector<int64_t> allIDsWithinRegionAbbreviation(const QString& abbreviation, const ZTree<RegionNode>& ontology)
{
  std::vector<int64_t> res;
  for (auto it = ontology.cbegin(); it != ontology.cend(); ++it) {
    if (it->abbreviation == abbreviation) {
      for (auto ait = ontology.cbegin(it); ait != ontology.cend(it); ++ait) {
        res.push_back(ait->id);
      }
    }
  }
  if (res.empty()) {
    throw ZException(fmt::format("can not find region {}", abbreviation));
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

  using Tr = typename C2t3::Triangulation;
  using Finite_facets_iterator = typename Tr::Finite_facets_iterator;
  using Finite_vertices_iterator = typename Tr::Finite_vertices_iterator;
  using Facet = typename Tr::Facet;
  using Edge = typename Tr::Edge;
  using Vertex_handle = typename Tr::Vertex_handle;

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
            oriented_set.contains(*fit) ||

            oriented_set.contains(c2t3.opposite_facet(*fit)) )
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
          if (!oriented_set.contains(fn)) {
            if(!oriented_set.contains(c2t3.opposite_facet(fn)))
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

  ZImgConnectedComponents connComp;
  ConnComp CC = connComp.runLabel(img, 0, 1);
  ZImgSignedDistanceMap distMap;
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
  std::memcpy(image3.data(), img.channelData(0), img.channelByteNumber());

  // default triangulation for Surface_mesher
  using Tr = CGAL::Surface_mesh_default_triangulation_3;
  // c2t3
  using C2t3 = CGAL::Complex_2_in_triangulation_3<Tr>;
  using GT = Tr::Geom_traits;
  using Gray_level_image = CGAL::Gray_level_image_3<GT::FT, GT::Point_3>;
  using Surface_3 = CGAL::Implicit_surface_3<GT, Gray_level_image>;

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
      LOG(INFO) << centers[i] << " " << squaredRadius[i];
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
      LOG(ERROR) << e.what();
    }
  }

  msh.clear();
  msh.setVertices(allVertices);
  msh.setIndices(allIndices);
  msh.generateNormals();
}
#endif

void binaryImgToMesh(const ZImg& img, ZMesh& msh, double scaleX, double scaleY, double scaleZ)
{
  CHECK(img.isType<uint8_t>() && !img.isTimeSeries() && !img.isMultiChannelsImg());
  vtkSmartPointer<vtkImageData> vimg = vtkSmartPointer<vtkImageData>::New();
  vimg->SetExtent(-1, img.width(), -1, img.height(), -1, img.depth());
  vimg->SetSpacing(1, 1, 1);
  vimg->SetOrigin(-1, -1, -1);
  vimg->AllocateScalars(VTK_UNSIGNED_CHAR, 1);
  std::memset(vimg->GetScalarPointer(), 0, (img.width() + 2) * (img.height() + 2) * (img.depth() + 2));

  for (size_t z = 0; z < img.depth(); ++z) {
    for (size_t y = 0; y < img.height(); ++y) {
      std::memcpy(vimg->GetScalarPointer(0, y, z), img.rowData(y, z, 0, 0), img.rowByteNumber());
    }
  }

#if 0
  auto discreteCubes = vtkSmartPointer<vtkDiscreteFlyingEdges3D>::New();
#else
  auto discreteCubes = vtkSmartPointer<vtkDiscreteMarchingCubes>::New();
#endif
  discreteCubes->SetInputData(vimg);
  discreteCubes->GenerateValues(1, 1, 1);

  vtkSmartPointer<vtkWindowedSincPolyDataFilter> smoother = vtkSmartPointer<vtkWindowedSincPolyDataFilter>::New();
  smoother->SetInputConnection(discreteCubes->GetOutputPort());
  smoother->SetNumberOfIterations(100);
  smoother->BoundarySmoothingOn();
  smoother->FeatureEdgeSmoothingOn();
  smoother->SetFeatureAngle(180);
  smoother->SetPassBand(0.001);
  smoother->NonManifoldSmoothingOn();
  // smoother->NormalizeCoordinatesOn();   // todo: VTK bug
  smoother->Update();

  vtkSmartPointer<vtkThreshold> selector = vtkSmartPointer<vtkThreshold>::New();
  selector->SetInputConnection(smoother->GetOutputPort());
  selector->SetInputArrayToProcess(0, 0, 0, vtkDataObject::FIELD_ASSOCIATION_CELLS, vtkDataSetAttributes::SCALARS);
  selector->SetThresholdFunction(vtkThreshold::THRESHOLD_BETWEEN);
  selector->SetLowerThreshold(1);
  selector->SetUpperThreshold(1);

  // Strip the scalars from the output
  vtkSmartPointer<vtkMaskFields> scalarsOff = vtkSmartPointer<vtkMaskFields>::New();
  scalarsOff->SetInputConnection(selector->GetOutputPort());
  scalarsOff->CopyAttributeOff(vtkMaskFields::POINT_DATA, vtkDataSetAttributes::SCALARS);
  scalarsOff->CopyAttributeOff(vtkMaskFields::CELL_DATA, vtkDataSetAttributes::SCALARS);

  vtkSmartPointer<vtkGeometryFilter> geometry = vtkSmartPointer<vtkGeometryFilter>::New();
  geometry->SetInputConnection(scalarsOff->GetOutputPort());
  geometry->Update();

  vtkPolyData* outputPolydata = geometry->GetOutput();
#if 1
  size_t numTriangles = outputPolydata->GetNumberOfPolys();
  double baseRate = 0.05;
  if (numTriangles * baseRate > 250000) {
    baseRate = 250000. / numTriangles;
  }
  if (numTriangles * baseRate < 200) {
    baseRate = std::min(1., 200. / numTriangles);
  }

  LOG(INFO) << baseRate << " " << numTriangles;
  vtkSmartPointer<vtkQuadricDecimation> decimate = vtkSmartPointer<vtkQuadricDecimation>::New();
  decimate->SetInputConnection(geometry->GetOutputPort());
  decimate->SetTargetReduction(1.0 - baseRate);
  decimate->Update();

  outputPolydata = decimate->GetOutput();
#endif

  vtkPoints* points = outputPolydata->GetPoints();
  vtkCellArray* polys = outputPolydata->GetPolys();
  //  vtkDataArray* pointsNormals = outputPolydata->GetPointData()->GetNormals();
  //  if (!pointsNormals) {
  //    // Generate normals
  //    vtkSmartPointer<vtkPolyDataNormals> normalGenerator = vtkSmartPointer<vtkPolyDataNormals>::New();
  //    normalGenerator->SetInputData(outputPolydata);
  //    normalGenerator->ComputePointNormalsOn();
  //    normalGenerator->ComputeCellNormalsOff();   //todo: check other options
  //    normalGenerator->Update();

  //    outputPolydata = normalGenerator->GetOutput();
  //    pointsNormals = outputPolydata->GetPointData()->GetNormals();
  //  }

  std::vector<glm::dvec3> vertices(points->GetNumberOfPoints());
  // std::vector<glm::dvec3> normals(pointsNormals->GetNumberOfTuples());
  // CHECK(vertices.size() == normals.size());
  std::vector<uint32_t> indices;
  for (vtkIdType id = 0; id < points->GetNumberOfPoints(); ++id) {
    points->GetPoint(id, &vertices[id][0]);
    // pointsNormals->GetTuple(id, &normals[id][0]);
  }
  vtkIdType npts;
  const vtkIdType* pts;
  for (vtkIdType i = 0; i < outputPolydata->GetNumberOfPolys(); ++i) {
    auto h = polys->GetNextCell(npts, pts);
    if (h == 0) {
      break;
    }
    if (npts == 3) {
      indices.push_back(pts[0]);
      indices.push_back(pts[1]);
      indices.push_back(pts[2]);
    }
  }

  msh.clear();
  for (auto& v : vertices) {
    v *= glm::dvec3(scaleX, scaleY, scaleZ);
  }
  msh.setVertices(vertices);
  msh.setIndices(indices);
  // msh.setNormals(normals);
  msh.generateNormals();
}

struct ContourNode
{
  int index;
  int parentIndex;
};

namespace {

constexpr size_t kPreferredInitialClosedSplineKnots = 8;
constexpr size_t kMinimumInitialClosedSplineKnots = 4;
constexpr double kSplineValidationFlattenToleranceFactor = 0.25;

struct ContourShape
{
  ROIType type = ROIType::Spline;
  QPolygonF poly;
};

struct DistanceResult
{
  double distanceSq = 0.0;
  size_t pointIndex = 0;
};

struct SplineFitEvaluation
{
  double maxErrorSq = std::numeric_limits<double>::infinity();
  size_t worstContourIndex = 0;
  glm::dvec2 worstSplinePoint{0.0, 0.0};
  bool hasWorstSplinePoint = false;
  std::vector<glm::dvec2> splinePolyline;
};

void validateMaskToROIOptions(const ZMaskToROIOptions& options, double scaleX, double scaleY, double scaleZ)
{
  if (!std::isfinite(options.epsilonPx) || options.epsilonPx <= 0.0) {
    throw ZException("mask-to-ROI boundary tolerance must be positive");
  }
  if (!std::isfinite(options.minKnotSpacingPx) || options.minKnotSpacingPx < 0.0) {
    throw ZException("mask-to-ROI minimum knot spacing must be non-negative");
  }
  if (options.sampledSplineTargetPoints <= 0) {
    throw ZException("sampled spline target point count must be positive");
  }
  if (options.sampledSplineMaxPointSpacing <= 0) {
    throw ZException("sampled spline maximum point spacing must be positive");
  }
  if (!std::isfinite(scaleX) || scaleX <= 0.0 || !std::isfinite(scaleY) || scaleY <= 0.0 || !std::isfinite(scaleZ) ||
      scaleZ <= 0.0) {
    throw ZException("mask-to-ROI scale values must be positive and finite");
  }
}

[[nodiscard]] std::vector<glm::dvec2> contourPoints(const std::vector<cv::Point>& contour)
{
  std::vector<glm::dvec2> points;
  points.reserve(contour.size());
  for (const cv::Point& p : contour) {
    points.emplace_back(static_cast<double>(p.x), static_cast<double>(p.y));
  }
  ZNaturalCubicSpline2D::compactConsecutiveDuplicatePointsInPlace(points);
  if (points.size() > 1 && points.front() == points.back()) {
    points.pop_back();
  }
  return points;
}

[[nodiscard]] QPolygonF
closedPolygonFromPoints(const std::vector<glm::dvec2>& points, double scaleX = 1.0, double scaleY = 1.0)
{
  QPolygonF poly;
  poly.reserve(static_cast<int>(points.size() + 1));
  for (const glm::dvec2& p : points) {
    poly.push_back(QPointF(p.x * scaleX, p.y * scaleY));
  }
  if (!poly.isEmpty() && !poly.isClosed()) {
    poly.push_back(poly.front());
  }
  return poly;
}

[[nodiscard]] double pointSegmentDistanceSq(const glm::dvec2& p, const glm::dvec2& a, const glm::dvec2& b)
{
  const glm::dvec2 ab = b - a;
  const double denom = glm::dot(ab, ab);
  if (denom <= 0.0) {
    const glm::dvec2 d = p - a;
    return glm::dot(d, d);
  }
  const double t = std::clamp(glm::dot(p - a, ab) / denom, 0.0, 1.0);
  const glm::dvec2 projection = a + t * ab;
  const glm::dvec2 d = p - projection;
  return glm::dot(d, d);
}

[[nodiscard]] double pointPolylineDistanceSq(const glm::dvec2& p, const std::vector<glm::dvec2>& polyline)
{
  CHECK(polyline.size() >= 2);
  double best = std::numeric_limits<double>::infinity();
  for (size_t i = 1; i < polyline.size(); ++i) {
    best = std::min(best, pointSegmentDistanceSq(p, polyline[i - 1], polyline[i]));
  }
  return best;
}

[[nodiscard]] DistanceResult maxDistanceToPolylineSq(const std::vector<glm::dvec2>& points,
                                                     const std::vector<glm::dvec2>& polyline)
{
  CHECK(!points.empty());
  CHECK(polyline.size() >= 2);
  DistanceResult result;
  for (size_t i = 0; i < points.size(); ++i) {
    const double d = pointPolylineDistanceSq(points[i], polyline);
    if (d > result.distanceSq) {
      result.distanceSq = d;
      result.pointIndex = i;
    }
  }
  return result;
}

[[nodiscard]] bool
cubicFlatEnough(const glm::dvec2& p0, const glm::dvec2& p1, const glm::dvec2& p2, const glm::dvec2& p3, double tol)
{
  const double tolSq = tol * tol;
  return pointSegmentDistanceSq(p1, p0, p3) <= tolSq && pointSegmentDistanceSq(p2, p0, p3) <= tolSq;
}

void flattenCubicBezier(const glm::dvec2& p0,
                        const glm::dvec2& p1,
                        const glm::dvec2& p2,
                        const glm::dvec2& p3,
                        double tol,
                        std::vector<glm::dvec2>& out)
{
  struct Segment
  {
    glm::dvec2 p0;
    glm::dvec2 p1;
    glm::dvec2 p2;
    glm::dvec2 p3;
  };

  std::vector<Segment> stack;
  stack.push_back(Segment{p0, p1, p2, p3});
  while (!stack.empty()) {
    const Segment segment = stack.back();
    stack.pop_back();

    if (cubicFlatEnough(segment.p0, segment.p1, segment.p2, segment.p3, tol)) {
      out.push_back(segment.p3);
      continue;
    }

    const glm::dvec2 p01 = 0.5 * (segment.p0 + segment.p1);
    const glm::dvec2 p12 = 0.5 * (segment.p1 + segment.p2);
    const glm::dvec2 p23 = 0.5 * (segment.p2 + segment.p3);
    const glm::dvec2 p012 = 0.5 * (p01 + p12);
    const glm::dvec2 p123 = 0.5 * (p12 + p23);
    const glm::dvec2 p0123 = 0.5 * (p012 + p123);

    stack.push_back(Segment{p0123, p123, p23, segment.p3});
    stack.push_back(Segment{segment.p0, p01, p012, p0123});
  }
}

[[nodiscard]] std::vector<glm::dvec2> flattenSplinePolyline(std::vector<glm::dvec2> closedKnots, double tol)
{
  std::vector<glm::dvec2> polyline;
  const auto beziers = ZNaturalCubicSpline2D::fitChordLength(std::move(closedKnots));
  if (beziers.empty()) {
    return polyline;
  }

  polyline.push_back(beziers.front().p0);
  for (const auto& b : beziers) {
    flattenCubicBezier(b.p0, b.p1, b.p2, b.p3, tol, polyline);
  }
  if (!polyline.empty() && polyline.front() != polyline.back()) {
    polyline.push_back(polyline.front());
  }
  return polyline;
}

[[nodiscard]] std::vector<double> contourCumulativeLengths(const std::vector<glm::dvec2>& contour)
{
  CHECK(contour.size() >= 2);
  std::vector<double> cumulative(contour.size() + 1, 0.0);
  for (size_t i = 0; i < contour.size(); ++i) {
    cumulative[i + 1] = cumulative[i] + glm::length(contour[(i + 1) % contour.size()] - contour[i]);
  }
  return cumulative;
}

[[nodiscard]] double arcDistance(size_t a, size_t b, const std::vector<double>& cumulative)
{
  CHECK(!cumulative.empty());
  if (a > b) {
    std::swap(a, b);
  }
  const double perimeter = cumulative.back();
  const double forward = cumulative[b] - cumulative[a];
  return std::min(forward, perimeter - forward);
}

[[nodiscard]] bool isAllowedKnotInsertion(size_t index,
                                          const std::set<size_t>& knotIndices,
                                          const std::vector<double>& cumulative,
                                          double minKnotSpacing)
{
  if (knotIndices.contains(index)) {
    return false;
  }
  if (minKnotSpacing <= 0.0) {
    return true;
  }
  return std::all_of(knotIndices.begin(), knotIndices.end(), [index, &cumulative, minKnotSpacing](size_t knotIndex) {
    return arcDistance(index, knotIndex, cumulative) >= minKnotSpacing;
  });
}

[[nodiscard]] std::set<size_t> initialClosedSplineKnotIndices(size_t contourPointCount)
{
  CHECK(contourPointCount >= 3);
  // Starting heuristic only: adaptive refinement may still insert every contour
  // point if that is required to satisfy the requested tolerance.
  const size_t targetCount = std::min(
    contourPointCount,
    std::max(kMinimumInitialClosedSplineKnots, std::min(kPreferredInitialClosedSplineKnots, contourPointCount)));

  std::set<size_t> indices;
  for (size_t i = 0; i < targetCount; ++i) {
    indices.insert((i * contourPointCount) / targetCount);
  }
  indices.insert(0);
  return indices;
}

[[nodiscard]] std::vector<glm::dvec2> closedKnotsFromIndices(const std::vector<glm::dvec2>& contour,
                                                             const std::set<size_t>& knotIndices)
{
  std::vector<glm::dvec2> knots;
  knots.reserve(knotIndices.size() + 1);
  for (size_t index : knotIndices) {
    CHECK(index < contour.size());
    knots.push_back(contour[index]);
  }
  if (!knots.empty()) {
    knots.push_back(knots.front());
  }
  return knots;
}

[[nodiscard]] SplineFitEvaluation
evaluateSplineFit(const std::vector<glm::dvec2>& contour, const std::vector<glm::dvec2>& closedKnots, double epsilon)
{
  SplineFitEvaluation evaluation;
  const double flattenTolerance = epsilon * kSplineValidationFlattenToleranceFactor;
  evaluation.splinePolyline = flattenSplinePolyline(closedKnots, flattenTolerance);
  if (evaluation.splinePolyline.size() < 2) {
    return evaluation;
  }

  const DistanceResult contourToSpline = maxDistanceToPolylineSq(contour, evaluation.splinePolyline);
  evaluation.maxErrorSq = contourToSpline.distanceSq;
  evaluation.worstContourIndex = contourToSpline.pointIndex;

  std::vector<glm::dvec2> closedContour = contour;
  closedContour.push_back(contour.front());
  for (const glm::dvec2& p : evaluation.splinePolyline) {
    const double d = pointPolylineDistanceSq(p, closedContour);
    if (d > evaluation.maxErrorSq) {
      evaluation.maxErrorSq = d;
      evaluation.worstSplinePoint = p;
      evaluation.hasWorstSplinePoint = true;
    }
  }
  return evaluation;
}

[[nodiscard]] std::optional<size_t> nearestAllowedContourIndex(const std::vector<glm::dvec2>& contour,
                                                               const glm::dvec2& point,
                                                               const std::set<size_t>& knotIndices,
                                                               const std::vector<double>& cumulative,
                                                               double minKnotSpacing)
{
  std::optional<size_t> bestIndex;
  double bestDistanceSq = std::numeric_limits<double>::infinity();
  for (size_t i = 0; i < contour.size(); ++i) {
    if (!isAllowedKnotInsertion(i, knotIndices, cumulative, minKnotSpacing)) {
      continue;
    }
    const glm::dvec2 d = contour[i] - point;
    const double distanceSq = glm::dot(d, d);
    if (distanceSq < bestDistanceSq) {
      bestDistanceSq = distanceSq;
      bestIndex = i;
    }
  }
  return bestIndex;
}

[[nodiscard]] std::optional<size_t> chooseSplineKnotInsertionIndex(const std::vector<glm::dvec2>& contour,
                                                                   const SplineFitEvaluation& evaluation,
                                                                   const std::set<size_t>& knotIndices,
                                                                   const std::vector<double>& cumulative,
                                                                   double minKnotSpacing)
{
  if (evaluation.splinePolyline.size() < 2) {
    for (size_t i = 0; i < contour.size(); ++i) {
      if (isAllowedKnotInsertion(i, knotIndices, cumulative, minKnotSpacing)) {
        return i;
      }
    }
    return std::nullopt;
  }

  if (isAllowedKnotInsertion(evaluation.worstContourIndex, knotIndices, cumulative, minKnotSpacing)) {
    return evaluation.worstContourIndex;
  }

  if (evaluation.hasWorstSplinePoint) {
    if (const auto nearest =
          nearestAllowedContourIndex(contour, evaluation.worstSplinePoint, knotIndices, cumulative, minKnotSpacing)) {
      return nearest;
    }
  }

  std::optional<size_t> bestIndex;
  double bestDistanceSq = -1.0;
  for (size_t i = 0; i < contour.size(); ++i) {
    if (!isAllowedKnotInsertion(i, knotIndices, cumulative, minKnotSpacing)) {
      continue;
    }
    const double d = pointPolylineDistanceSq(contour[i], evaluation.splinePolyline);
    if (d > bestDistanceSq) {
      bestDistanceSq = d;
      bestIndex = i;
    }
  }
  return bestIndex;
}

[[nodiscard]] ContourShape
polygonShapeFromContour(const std::vector<cv::Point>& contour, double scaleX, double scaleY, double epsilon)
{
  std::vector<cv::Point2f> sourcePoints;
  sourcePoints.reserve(contour.size());
  for (const cv::Point& p : contour) {
    sourcePoints.emplace_back(static_cast<float>(p.x), static_cast<float>(p.y));
  }

  std::vector<cv::Point2f> simplified;
  cv::approxPolyDP(sourcePoints, simplified, epsilon, true);

  std::vector<glm::dvec2> points;
  points.reserve(simplified.size());
  if (simplified.size() >= 3) {
    for (const cv::Point2f& p : simplified) {
      points.emplace_back(static_cast<double>(p.x), static_cast<double>(p.y));
    }
  } else {
    points = contourPoints(contour);
  }

  ContourShape shape;
  shape.type = ROIType::Polygon;
  shape.poly = closedPolygonFromPoints(points, scaleX, scaleY);
  return shape;
}

[[nodiscard]] ContourShape sampledSplineShapeFromContour(const std::vector<cv::Point>& contour,
                                                         const ZMaskToROIOptions& options,
                                                         double scaleX,
                                                         double scaleY)
{
  const int stride = std::max(1,
                              std::min(options.sampledSplineMaxPointSpacing,
                                       static_cast<int>(contour.size()) / options.sampledSplineTargetPoints));

  QPolygonF poly;
  poly.reserve(static_cast<int>((contour.size() / static_cast<size_t>(stride)) + 2));
  for (size_t p = 0; p < contour.size(); p += static_cast<size_t>(stride)) {
    poly.push_back(QPointF(static_cast<double>(contour[p].x) * scaleX, static_cast<double>(contour[p].y) * scaleY));
  }
  if (!poly.isEmpty() && !poly.isClosed()) {
    poly.push_back(poly.front());
  }

  ContourShape shape;
  shape.type = ROIType::Spline;
  shape.poly = std::move(poly);
  return shape;
}

[[nodiscard]] ContourShape splineShapeFromContour(const std::vector<cv::Point>& contour,
                                                  const ZMaskToROIOptions& options,
                                                  double scaleX,
                                                  double scaleY)
{
  const std::vector<glm::dvec2> points = contourPoints(contour);
  if (points.size() < 3) {
    return ContourShape{};
  }

  const std::vector<double> cumulative = contourCumulativeLengths(points);
  std::set<size_t> knotIndices = initialClosedSplineKnotIndices(points.size());
  const double targetErrorSq = options.epsilonPx * options.epsilonPx;

  QPolygonF bestPoly;
  double bestErrorSq = std::numeric_limits<double>::infinity();

  while (true) {
    const std::vector<glm::dvec2> closedKnots = closedKnotsFromIndices(points, knotIndices);
    const SplineFitEvaluation evaluation = evaluateSplineFit(points, closedKnots, options.epsilonPx);
    if (evaluation.maxErrorSq < bestErrorSq) {
      bestErrorSq = evaluation.maxErrorSq;
      bestPoly = closedPolygonFromPoints(closedKnots, scaleX, scaleY);
    }

    if (evaluation.maxErrorSq <= targetErrorSq) {
      return ContourShape{ROIType::Spline, closedPolygonFromPoints(closedKnots, scaleX, scaleY)};
    }

    if (knotIndices.size() == points.size()) {
      break;
    }

    const auto nextIndex =
      chooseSplineKnotInsertionIndex(points, evaluation, knotIndices, cumulative, options.minKnotSpacingPx);
    if (!nextIndex) {
      break;
    }
    knotIndices.insert(*nextIndex);
  }

  if (options.splineFallback == ZMaskToROISplineFallback::KeepBestSpline && !bestPoly.isEmpty()) {
    LOG(WARNING) << "Mask-to-ROI spline import kept best spline with max boundary error " << std::sqrt(bestErrorSq)
                 << " px, above requested tolerance " << options.epsilonPx << " px";
    return ContourShape{ROIType::Spline, bestPoly};
  }

  ContourShape fallback = polygonShapeFromContour(contour, scaleX, scaleY, options.epsilonPx);
  const double bestErrorPx =
    std::isfinite(bestErrorSq) ? std::sqrt(bestErrorSq) : std::numeric_limits<double>::infinity();
  LOG(WARNING) << "Mask-to-ROI spline import fell back to polygon; best spline max boundary error was " << bestErrorPx
               << " px, requested tolerance " << options.epsilonPx << " px";
  return fallback;
}

[[nodiscard]] ContourShape contourShapeFromMaskContour(const std::vector<cv::Point>& contour,
                                                       const ZMaskToROIOptions& options,
                                                       double scaleX,
                                                       double scaleY)
{
  switch (options.outputType) {
    case ZMaskToROIOutputType::Polygon:
      return polygonShapeFromContour(contour, scaleX, scaleY, options.epsilonPx);
    case ZMaskToROIOutputType::SampledSpline:
      return sampledSplineShapeFromContour(contour, options, scaleX, scaleY);
    case ZMaskToROIOutputType::Spline:
      return splineShapeFromContour(contour, options, scaleX, scaleY);
  }
  CHECK(false);
  return ContourShape{};
}

void appendContourShapeToROI(ZROI& roi, int slice, int depth, const ContourShape& shape)
{
  if (shape.poly.size() < 4 || !shape.poly.isClosed()) {
    return;
  }

  if (depth == 0) {
    if (shape.type == ROIType::Polygon) {
      roi.newPolygon(slice, shape.poly);
    } else {
      roi.newSpline(slice, shape.poly);
    }
    return;
  }

  const bool isAdd = (depth % 2) == 0;
  if (shape.type == ROIType::Polygon) {
    if (isAdd) {
      roi.addPolygon(slice, shape.poly);
    } else {
      roi.subtractPolygon(slice, shape.poly);
    }
  } else {
    if (isAdd) {
      roi.addSpline(slice, shape.poly);
    } else {
      roi.subtractSpline(slice, shape.poly);
    }
  }
}

} // namespace

void binaryImgToROI(const ZImg& img,
                    ZROI& roi,
                    const ZMaskToROIOptions& options,
                    double scaleX,
                    double scaleY,
                    double scaleZ)
{
  CHECK(img.isType<uint8_t>() && !img.isTimeSeries() && !img.isMultiChannelsImg());
  validateMaskToROIOptions(options, scaleX, scaleY, scaleZ);
  roi.clear();

  for (size_t tmps = 0; tmps < img.depth(); ++tmps) {
    auto s = static_cast<size_t>(std::round(scaleZ * tmps));
    int64_t min;
    int64_t max;
    ZImg simg = img.extractPlane(s, 0, 0);
    simg.computeMinMax(min, max);
    if (max == 0) {
      continue;
    }

    try {
      cv::Mat mat(simg.height(), simg.width(), CV_8UC1, simg.channelData(0));
      std::vector<std::vector<cv::Point>> contours;
      std::vector<cv::Vec4i> hierarchy;
      cv::findContours(mat,
                       contours,
                       hierarchy,
                       options.preserveHoles ? cv::RETR_TREE : cv::RETR_EXTERNAL,
                       cv::CHAIN_APPROX_NONE);

      ZTree<ContourNode> contoursTree;
      std::map<int, ContourNode> nodeMap;

      for (size_t i = 0; i < hierarchy.size(); ++i) {
        ContourNode node{};
        node.index = i;
        node.parentIndex = hierarchy[i][3];
        nodeMap[node.index] = node;
      }

      std::map<int, ZTree<ContourNode>::Iterator> itMap;
      while (!nodeMap.empty()) {
        auto it = nodeMap.begin();
        while (it != nodeMap.end()) {
          auto parentID = it->second.parentIndex;
          auto nodeIt = itMap.find(parentID);
          if (nodeIt != itMap.end()) {
            itMap[it->first] = contoursTree.appendChild(nodeIt->second, it->second);
            it = nodeMap.erase(it);
          } else if (!nodeMap.contains(parentID)) {
            itMap[it->first] = contoursTree.appendRoot(it->second);
            it = nodeMap.erase(it);
          } else {
            ++it;
          }
        }
      }

      for (auto rit = contoursTree.cbeginRoot(); rit != contoursTree.cendRoot(); ++rit) {
        for (auto it = contoursTree.cbeginBreadthFirst(rit); it != contoursTree.cendBreadthFirst(rit); ++it) {
          // VLOG(1) << it->index << " " << contours[it->index].size();
          const size_t c = static_cast<size_t>(it->index);

          if (contours[c].size() < 3) {
            if (contoursTree.numAncestors(it) == 0) {
              break;
            } else {
              continue;
            }
          }

          const int depth = contoursTree.numAncestors(it);
          const ContourShape shape = contourShapeFromMaskContour(contours[c], options, scaleX, scaleY);
          appendContourShapeToROI(roi, static_cast<int>(s), depth, shape);
        }
      }
    }
    catch (const cv::Exception& e) {
      LOG(ERROR) << e.what();
    }
  }
}

} // namespace nim
