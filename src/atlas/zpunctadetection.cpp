#include "zpunctadetection.h"
#include "zimg.h"
#include "zeigenutils.h"
#include "zvbgmm.h"
#include <boost/math/distributions/chi_squared.hpp>
#include <boost/geometry/geometry.hpp>
#include <boost/geometry/geometries/point_xy.hpp>
#include <boost/geometry/geometries/polygon.hpp>
#include <boost/geometry/geometries/adapted/c_array.hpp>
#include <boost/geometry/multi/geometries/multi_polygon.hpp>
#include <limits>
#include <algorithm>
#include <QThread>
#include "zassignpuncta.h"
#include <QFileInfo>
#include <QFile>
#include "zimgautothreshold.h"
#include "zimgitkinterface.h"
#include "zimage2dutils.h"
#include "zimgsigneddistancemap.h"
#include "zimgconnectedcomponents.h"
#include "zimgregionalextrema.h"
#include "zimgneighborhooditerator.h"

#include <itkImage.h>
#include <itkImageRegionIterator.h>
#include <itkSliceBySliceImageFilter.h>
#include <itkThresholdImageFilter.h>
#include <itkFlatStructuringElement.h>
#include <itkBinaryDilateImageFilter.h>
#include <itkBinaryMorphologicalOpeningImageFilter.h>
#include <itkBinaryThresholdImageFilter.h>

namespace {

bool vectorIsEmpty(const std::vector<int>& i)
{
  return i.empty();
}

using point_2d = boost::geometry::model::d2::point_xy<double>;
using polygon_2d = boost::geometry::model::polygon<point_2d>;
using box_2d = boost::geometry::model::box<point_2d>;

polygon_2d errorEllipseToPolygon(Eigen::RowVectorXd m, Eigen::MatrixXd cov, double k)
{
  polygon_2d poly;
  const int n = 100;  // number of points around half ellipse
  using namespace boost::math::double_constants;
  double step = pi / n;
  double coor[n * 2 + 1][2];
  double p = 0;  // angles around a circle
  Eigen::SelfAdjointEigenSolver<Eigen::MatrixXd> es(cov.topLeftCorner<2, 2>(), Eigen::ComputeEigenvectors);
  double sqtlmd1 = std::sqrt(es.eigenvalues()(0));
  double sqtlmd2 = std::sqrt(es.eigenvalues()(1));
  // eigenvector 1
  double ev1x = es.eigenvectors()(0, 0);
  double ev1y = es.eigenvectors()(1, 0);
  // eigenvector 2
  double ev2x = es.eigenvectors()(0, 1);
  double ev2y = es.eigenvectors()(1, 1);

  for (int i = 0; i < 2 * n + 1; ++i) {
    coor[i][0] = (std::cos(p) * sqtlmd1 * ev1x + std::sin(p) * sqtlmd2 * ev2x) * k + m(0);
    coor[i][1] = (std::cos(p) * sqtlmd1 * ev1y + std::sin(p) * sqtlmd2 * ev2y) * k + m(1);
    p += step;
  }
  boost::geometry::assign_points(poly, coor);
  boost::geometry::correct(poly);
  return poly;
}

// return mean shifted centroids. if z is not -1, data is 2D and z is the data slice in stack.
template<typename T>
Eigen::MatrixXd meanShiftGaussianCenters(const nim::ZVBGMM<T, double>& vbgmm, const nim::ZImg& img, int z = -1)
{
  Eigen::MatrixXd res = vbgmm.centroids();

  int dimension = 3;
  if (z != -1) {
    dimension = 2;
  }
  boost::math::chi_squared dist(dimension);
  double k = std::sqrt(boost::math::quantile(dist, 0.9));

  for (size_t i = 0; i < vbgmm.numOfClusters(); ++i) {
    Eigen::Vector3i m;
    for (int d = 0; d < dimension; ++d)
      m[d] = nim::roundTo<int>(res(i, d));
    Eigen::MatrixXd cov = vbgmm.covar(i);
    Eigen::SelfAdjointEigenSolver<Eigen::MatrixXd> es(cov, Eigen::EigenvaluesOnly);
    double radius = std::sqrt(es.eigenvalues()(dimension - 2)); // short axis or middle axis
    if (radius < 1)
      radius = 1;
    if (radius > 3)
      radius = 3;
    int w = nim::roundTo<int>(radius * k);
    int iter = 0;
    int maxIter = 40;
    double epsx = 1.;
    double epsy = 1.;
    double epsz = 1.;
    if (dimension == 3) {
      while ((std::abs(epsx) > 0.5 || std::abs(epsy) > 0.5 || std::abs(epsz) > 0.5) && iter < maxIter) {
        std::vector<double> values;
        std::vector<double> x_shifts;
        std::vector<double> y_shifts;
        std::vector<double> z_shifts;
        for (int lz = -w; lz <= w; ++lz)  // local z
          for (int y = -w; y <= w; ++y)
            for (int x = -w; x <= w; ++x) {
              if (x * x + y * y + lz * lz <= w * w &&
                  m.x() + x >= 0 && m.x() + x < static_cast<int>(img.width()) &&
                  m.y() + y >= 0 && m.y() + y < static_cast<int>(img.height()) &&
                  m.z() + lz >= 0 && m.z() + lz < static_cast<int>(img.depth())) {
                values.push_back(img.value<double>(m.x() + x, m.y() + y, m.z() + lz));
                x_shifts.push_back(x);
                y_shifts.push_back(y);
                z_shifts.push_back(lz);
              }
            }
        double value_sum = std::accumulate(values.begin(), values.end(), 0);
        epsx = std::inner_product(values.begin(), values.end(), x_shifts.begin(), 0) / value_sum;
        epsy = std::inner_product(values.begin(), values.end(), y_shifts.begin(), 0) / value_sum;
        epsz = std::inner_product(values.begin(), values.end(), z_shifts.begin(), 0) / value_sum;
        if (std::abs(epsx) > 0.5)
          m.x() += epsx > 0 ? 1 : -1;
        if (std::abs(epsy) > 0.5)
          m.y() += epsy > 0 ? 1 : -1;
        if (std::abs(epsz) > 0.5)
          m.z() += epsz > 0 ? 1 : -1;
        ++iter;
      }
      res(i, 0) = m.x() + epsx;
      res(i, 1) = m.y() + epsy;
      res(i, 2) = m.z() + epsz;
    } else {
      while ((std::abs(epsx) > 0.5 || std::abs(epsy) > 0.5 || std::abs(epsz) > 0.5) && iter < maxIter) {
        std::vector<double> values;
        std::vector<double> x_shifts;
        std::vector<double> y_shifts;
        for (int y = -w; y <= w; ++y)
          for (int x = -w; x <= w; ++x) {
            if (x * x + y * y <= w * w && m.x() + x >= 0 && m.x() + x < static_cast<int>(img.width()) &&
                m.y() + y >= 0 && m.y() + y < static_cast<int>(img.height())) {
              values.push_back(img.value<double>(m.x() + x, m.y() + y, z));
              x_shifts.push_back(x);
              y_shifts.push_back(y);
            }
          }
        double value_sum = std::accumulate(values.begin(), values.end(), 0);
        epsx = std::inner_product(values.begin(), values.end(), x_shifts.begin(), 0) / value_sum;
        epsy = std::inner_product(values.begin(), values.end(), y_shifts.begin(), 0) / value_sum;
        if (std::abs(epsx) > 0.5)
          m.x() += epsx > 0 ? 1 : -1;
        if (std::abs(epsy) > 0.5)
          m.y() += epsy > 0 ? 1 : -1;
        ++iter;
      }
      res(i, 0) = m.x() + epsx;
      res(i, 1) = m.y() + epsy;
    }
  }

  return res;
}

}  // anonymous namespace

namespace nim {

ZPunctaDetection::ZPunctaDetection(const ZImg& img, size_t punctaChannel, size_t t)
  : ZImgProcess()
  , m_img(img)
  , m_punctaChannel(punctaChannel)
  , m_t(t)
  , m_punctaThreshold(-1)
  , m_splitSizeThreshold(20)
  , m_confRadius(0.95)
  , m_confOverlapArea(0.8)
  , m_overlapRateThreshold(0.8)
  , m_seedSizeThreshold(6)
  , m_dendriteChannel(-1)
  , m_maxDendriteTubeRadius(2.6)
  , m_dendriteThreshold(100)
  , m_maxDistToBranch(2.5)
  , m_ambiguousFactor(1.0)
{
  if (!m_img.isType<uint8_t>()) {
    throw ZImgException("puncta detecion only support uint8_t img");
  }
  m_useMultithreading = true;
}

ZPunctaDetection::~ZPunctaDetection()
{
  cleanup();
}

void ZPunctaDetection::doWork()
{
  cleanup();

  LOG(INFO) << "";
  LOG(INFO) << "Start Detect Puncta";
  LOG(INFO) << "";

  if (m_punctaChannel < m_img.numChannels()) {
    LOG(INFO) << "Puncta Channel: " << m_punctaChannel + 1 << " (start from 1)";
  } else {
    throw ZImgException(QString("Wrong puncta channel: %1. Abort.").arg(m_punctaChannel));
  }

  if (m_dendriteChannel != -1) {
    if (m_dendriteChannel >= 0 && m_dendriteChannel < static_cast<int>(m_img.numChannels())) {
      LOG(INFO) << "Dendrite Channel: " << m_dendriteChannel + 1 << " (start from 1)";
      LOG(INFO) << "Dendrite Threshold: " << m_dendriteThreshold;
      LOG(INFO) << "Max Dendrite Tube Radius: " << m_maxDendriteTubeRadius << "um";
    } else {
      throw ZImgException(QString("Wrong dendrite channel: %1. Abort.").arg(m_dendriteChannel));
    }
    if (m_img.info().voxelSizeUnit == VoxelSizeUnit::none) {
      throw ZImgException(
        "Voxel Size not set (need by soma detection and puncta-tree matching), Abort Puncta Detection.");
    }
  } else {
    LOG(INFO) << "No Dendrite Channel.";
  }

  clearRegisteredSubOperations();
  double totalSubWeight = .1;
  if (m_punctaThreshold == -1)
    totalSubWeight += .1;
  if (m_dendriteChannel != -1)
    totalSubWeight += .45;
  if (!m_swcTrees.empty() && m_dendriteChannel != -1)
    totalSubWeight += .025;

  setTotalSubOperationWeight(totalSubWeight);

  // get threshold
  if (m_punctaThreshold == -1) {
    ZImgAutoThreshold<true> imgAutoThre;
    registerSubOperation(&imgAutoThre, .1);
    m_punctaThreshold = imgAutoThre.triangleThre<uint8_t>(m_img, m_punctaChannel, m_t) + 3;
  }
  LOG(INFO) << "Voxel Size X: " << m_img.voxelSizeXInUm() << "um";
  LOG(INFO) << "Voxel Size Y: " << m_img.voxelSizeYInUm() << "um";
  LOG(INFO) << "Voxel Size Z: " << m_img.voxelSizeZInUm() << "um";
  LOG(INFO) << "Use puncta threshold: " << m_punctaThreshold;
  LOG(INFO) << "Use split size threshold: " << m_splitSizeThreshold;
  LOG(INFO) << "Confidence Region for Radius Estimate: " << m_confRadius;
  LOG(INFO) << "Confidence Region for Gaussian Models Overlap Rate: " << m_confOverlapArea;
  LOG(INFO) << "Overlap Rate Threshold: " << m_overlapRateThreshold;
  LOG(INFO) << "Watershed Seed Size Threshold: " << m_seedSizeThreshold;
  LOG(INFO) << "Multithreading: " << m_useMultithreading;
  LOG(INFO) << "Ambiguous Factor: " << m_ambiguousFactor;
  LOG(INFO) << "Saturate Intensity: " << 255;

  ZImg img = m_img.extractChannel(m_punctaChannel, m_t);
  if (m_dendriteChannel != -1) {
    ZImg somaImg;
    Eigen::RowVectorXi minLoc;
    LOG(INFO) << "Start Soma Detection";
    int somaThre = cropOutSomaImg(img, somaImg, minLoc);
    LOG(INFO) << "End Soma Detection";
    if (somaThre != -1) {
      LOG(INFO) << "Use puncta threshold in soma area: " << somaThre;
      LOG(INFO) << "Start Detect Puncta in Soma";
      detectImpl(somaImg, somaThre, m_detectedSomaPuncta, m_filteredSomaPuncta, minLoc, 0.1, 0.0);
      LOG(INFO) << "End Detect Puncta in Soma";
      LOG(INFO) << "";
    } else {
      LOG(INFO) << "Can not detect any soma!";
      LOG(INFO) << "";
    }
    detectImpl(img, m_punctaThreshold, m_detectedPuncta, m_filteredPuncta, Eigen::RowVectorXi::Zero(3), 0.9, 0.1);
  } else {
    detectImpl(img, m_punctaThreshold, m_detectedPuncta, m_filteredPuncta, Eigen::RowVectorXi::Zero(3), 1.0, 0.0);
  }

  LOG(INFO) << "";
  LOG(INFO) << "End Detect Puncta";
  LOG(INFO) << "";

  if (!m_detectedPunctaFileName.isEmpty()) {
    m_detectedPuncta.save(m_detectedPunctaFileName);
    m_filteredPuncta.save(getFilteredPunctaFilename());
  }
  if (m_dendriteChannel != -1 && !m_detectedSomaPunctaFileName.isEmpty()) {
    m_detectedSomaPuncta.save(m_detectedSomaPunctaFileName);
    m_filteredSomaPuncta.save(getFilteredSomaPunctaFilename());
  }

  if (!m_swcTrees.empty() && m_dendriteChannel != -1) {
    ZAssignPuncta assignPuncta(m_img, m_dendriteChannel, m_t);
    assignPuncta.addSwcTrees(m_swcTrees);
    assignPuncta.setAmbiguousFactor(m_ambiguousFactor);
    assignPuncta.setMaxDistToBranchInUm(m_maxDistToBranch);
    assignPuncta.setPuncta(m_detectedPuncta);
    assignPuncta.setSomaPuncta(m_detectedSomaPuncta);
    registerSubOperation(&assignPuncta, .025);
    assignPuncta.run();
    for (size_t i = 0; i < m_swcTrees.size(); ++i) {
      ZPuncta puncta = assignPuncta.getPunctaOfTree(m_swcTrees[i]);
      QString fn = getPunctaOutputFilename(m_swcPaths[i]);
      puncta.save(fn);
      puncta = assignPuncta.getSomaPunctaOfTree(m_swcTrees[i]);
      fn = getSomaPunctaOutputFilename(m_swcPaths[i]);
      puncta.save(fn);
    }
    ZPuncta puncta = assignPuncta.getAmbiguousPuncta();
    if (puncta.size() > 0) {
      QString fn = getAmbiguousPunctaOutputFileName();
      puncta.save(fn);
    }
  }
}

void ZPunctaDetection::setSwcTrees(const std::vector<ZSwc*>& trees, const QStringList& treePaths)
{
  m_swcTrees = trees;
  m_swcPaths = treePaths;
}

void ZPunctaDetection::setSwcTrees(std::vector<ZSwc>& trees, const QStringList& treePaths)
{
  m_swcTrees.clear();
  for (size_t i = 0; i < trees.size(); ++i)
    m_swcTrees.push_back(&trees[i]);
  m_swcPaths = treePaths;
}

double
ZPunctaDetection::getOverlapRateOfTwoErrorEllipse(Eigen::RowVectorXd m1, Eigen::MatrixXd cov1, Eigen::RowVectorXd m2,
                                                  Eigen::MatrixXd cov2, double conf)
{
  boost::math::chi_squared dist(2);  // two dimension
  double k = std::sqrt(boost::math::quantile(dist, conf));

  polygon_2d poly1 = errorEllipseToPolygon(m1, cov1, k);
  polygon_2d poly2 = errorEllipseToPolygon(m2, cov2, k);
  if (boost::geometry::within(point_2d(m1(0), m1(1)), poly2) ||
      boost::geometry::within(point_2d(m2(0), m2(1)), poly1))
    return 1.0;
  std::vector<polygon_2d> v;
  boost::geometry::intersection(poly1, poly2, v);
  if (v.size() > 1) {
    LOG(ERROR) << "Two Ellipse can not have " << v.size() << " intersection area. Something is wrong.";
  }
  double overlapArea = 0.0;
  if (!v.empty()) {
    overlapArea = boost::geometry::area(*(v.begin()));
    return std::max(overlapArea / boost::geometry::area(poly1), overlapArea / boost::geometry::area(poly2));
  } else {
    return 0.0;
  }
}

void ZPunctaDetection::detectImpl(ZImg& img, int thre, ZPuncta& resList, ZPuncta& filteredList,
                                  const Eigen::RowVectorXi& minLocIn, double weight, double baseWeight)
{
  double saturatedIntensity = 255;

  for (size_t z = 0; z < img.depth(); ++z) {
    image2DGaussianFilter(img.planeData<uint8_t>(z), img.width(), img.height(), 1., 1.,
                          img.planeData<uint8_t>(z), 3, 3, PadOption::Constant, 0_u8, m_useMultithreading);
    reportProgress(baseWeight + weight * .5 * z / img.depth());
  }
  reportProgress(baseWeight + weight * .5);

  img.thresholdBelow(thre, ZImg::ThresholdMode::IncludeThreshold, 0);

  ZImgConnectedComponents<true> imgConnComp;
  registerSubOperation(&imgConnComp, .05);
  ConnComp CC = imgConnComp.run(img);

  ZImgRegionalExtrema<true> imgRegionalExtrema;
  registerSubOperation(&imgRegionalExtrema, .05);
  ZImg locmax = imgRegionalExtrema.regionalMax(img);

  img.clear();

  size_t nAllObjects = CC.voxelIdxList.size();

  for (size_t objectIdx = 0; objectIdx < nAllObjects; ++objectIdx) {
    LOG(INFO) << "";
    reportProgress(baseWeight + weight * (.5 + .5 * objectIdx / nAllObjects));
    LOG(INFO) << "Start Connected Component " << (objectIdx + 1);

    Eigen::MatrixXi voxels(CC.voxelIdxList[objectIdx].size(), 3);
    for (size_t pixelId = 0; pixelId < CC.voxelIdxList[objectIdx].size(); ++pixelId) {
      ZVoxelCoordinate coord = locmax.indexToCoord(CC.voxelIdxList[objectIdx][pixelId]);
      voxels(pixelId, 0) = coord[0] + minLocIn(0);
      voxels(pixelId, 1) = coord[1] + minLocIn(1);
      voxels(pixelId, 2) = coord[2] + minLocIn(2);
    }

    LOG(INFO) << "  Voxel Number of Connected Component " << (objectIdx + 1) << " : "
              << CC.voxelIdxList[objectIdx].size();

    if (voxels.rows() < m_splitSizeThreshold) {  // no split, save to punctum
      LOG(INFO) << "  No split for this Connected Component, save to punctum.";
      Eigen::VectorXd voxelIntensities = getVoxelIntensities(voxels, m_img, m_punctaChannel, m_t);
      ZPunctum punc;
      punc.setVoxelIntensities(voxelIntensities);
      punc.setVoxelLocations(voxels);
      punc.updateFromVoxelsList(m_confRadius);
      LOG(INFO) << "    Punctum: " << punc.x() << " " << punc.y() << " " << punc.z() << " "
                << punc.maxIntensity() << " " << punc.volSize() << " " << punc.meanIntensity();
      resList.push_back(punc);
    } else {  // go to watershed
      LOG(INFO) << "  Start Watershed Split";
      Eigen::RowVectorXi minLoc;
      Eigen::RowVectorXi size;
      getVoxelRange(voxels, minLoc, size);
      ZImg cropImg = cropZImg(voxels, m_img, m_punctaChannel, m_t, minLoc, size);
      std::vector<Eigen::MatrixXi> wsObjects = watershedSplit(cropImg);
      LOG(INFO) << "    Number of Watershed Components: " << wsObjects.size();
      for (size_t wsObjIdx = 0; wsObjIdx < wsObjects.size(); ++wsObjIdx) {
        LOG(INFO) << "    Voxel Number of Watershed Component " << wsObjIdx + 1 << " : " << wsObjects[wsObjIdx].rows();
        Eigen::VectorXd wsObjVoxelIntens = getVoxelIntensities(wsObjects[wsObjIdx], cropImg, 0, 0);
        Eigen::MatrixXi wsObjVoxelLocs = wsObjects[wsObjIdx];
        wsObjVoxelLocs.rowwise() += minLoc;
        if (wsObjVoxelIntens.size() < m_splitSizeThreshold) { // no split, save to punctum
          LOG(INFO) << "    No split for Watershed Component " << wsObjIdx + 1 << ", save to punctum.";
          ZPunctum punc;
          punc.setVoxelIntensities(wsObjVoxelIntens);
          punc.setVoxelLocations(wsObjVoxelLocs);
          punc.updateFromVoxelsList(m_confRadius);
          LOG(INFO) << "      Punctum: " << punc.x() << " " << punc.y() << " " << punc.z() << " "
                    << punc.maxIntensity() << " " << punc.volSize() << " " << punc.meanIntensity();
          resList.push_back(punc);
        } else { // go to vbgmm
          LOG(INFO) << "    Start VBGMM Split for Watershed Component " << wsObjIdx + 1;
          int numCenter = getNumCenters(wsObjVoxelLocs, wsObjVoxelIntens, locmax, saturatedIntensity, minLocIn);
          LOG(INFO) << "      Number of voxels: " << wsObjVoxelIntens.size();
          LOG(INFO) << "      Initial number of centers: " << numCenter;
          if (numCenter == 1) {
            LOG(INFO) << "        Only one component, skip vbgmm, save to punctum.";
            ZPunctum punc;
            punc.setVoxelIntensities(wsObjVoxelIntens);
            punc.setVoxelLocations(wsObjVoxelLocs);
            punc.updateFromVoxelsList(m_confRadius);
            LOG(INFO) << "          Punctum: " << punc.x() << " " << punc.y() << " " << punc.z() << " "
                      << punc.maxIntensity() << " " << punc.volSize() << " " << punc.meanIntensity();
            resList.push_back(punc);
          } else {
            vbgmmSplit(wsObjects[wsObjIdx], wsObjVoxelIntens, numCenter, cropImg, resList,
                       m_confRadius, m_confOverlapArea, m_overlapRateThreshold, minLoc);
          }
          LOG(INFO) << "    End VBGMM Split for Watershed Component " << wsObjIdx + 1;
        }
      }
      LOG(INFO) << "  End Watershed Split";
    }
    LOG(INFO) << "End Connected Component " << objectIdx + 1;
  }
  CC.clear();
  locmax.clear();
  LOG(INFO) << "";
  LOG(INFO) << "Detected " << resList.size() << " Puncta.";

  ZPuncta::iterator pIt = resList.begin();
  while (pIt != resList.end()) {
    if (pIt->volSize() <= 5.0 && pIt->maxIntensity() < thre + .1 * saturatedIntensity) {
      filteredList.push_back(*pIt);
      pIt = resList.erase(pIt);
    } else
      ++pIt;
  }
  LOG(INFO) << "Number of Puncta After Filtering: " << resList.size();
  reportProgress(baseWeight + weight * 1.0);
}

#if 0
template <typename Image3DType>
MatrixXi ZPunctaDetection::detectSomaPuncta(const Image3DType *preprocessedImage)
{
  ZSomaDetection somaDetection(m_img, m_dendriteChannel);
  somaDetection.setVoxelSizeXInUm(m_voxelSizeX);
  somaDetection.setVoxelSizeYInUm(m_voxelSizeY);
  somaDetection.setVoxelSizeZInUm(m_voxelSizeZ);
  somaDetection.setMaxTubeRadiusInUm(m_maxDendriteTubeRadius);
  somaDetection.setTubeThreshold(m_dendriteThreshold);
  somaDetection.setNumThreads(m_numThreads);
  registerOperation(&somaDetection, 3.5);

  somaDetection.run();
  MatrixXi somaVoxels = somaDetection.getSomaVoxelList();

  if (somaVoxels.rows() == 0) {
    setTotalWeight(getTotalWeight() - 1.0);
    return somaVoxels;
  }

  // detect puncta in soma area
  RowVectorXi borderWidth(3);
  borderWidth << 100, 100, 100;

  RowVectorXi minLoc;
  RowVectorXi size;

  Uint8Image3DType::Pointer somaArea = constructBinaryITKImage(somaVoxels, m_img, borderWidth, &minLoc, &size);

  using LabelType = itk::SizeValueType;
  using LabelMapType = itk::LabelMap<itk::LabelObject<LabelType, 3>>;
  using LabelImageType = itk::Image<LabelType, 3>;

  typename Image3DType::Pointer somaPunctaImage = cropITKImage(preprocessedImage, minLoc, size);

  //writeITKImage(somaPunctaImage.GetPointer(), "/Users/feng/Downloads/test_itk.tif");
  //writeITKImage(somaArea.GetPointer(), "/Users/feng/Downloads/test_itk2.tif");

  using MaskFilterType = itk::MaskImageFilter<Uint8Image3DType, Image3DType>;
  typename MaskFilterType::Pointer maskFilter = MaskFilterType::New();
  maskFilter->SetInput(somaArea);
  maskFilter->SetMaskImage(somaPunctaImage);
  maskFilter->SetNumberOfThreads(m_numThreads);
  registerOperation(maskFilter.GetPointer(), .25);

  using StructuringElementType = itk::FlatStructuringElement<3>;
  StructuringElementType::RadiusType openElementRadius;
  openElementRadius[0] = std::floor(.22 / m_voxelSizeX);
  openElementRadius[1] = std::floor(.22 / m_voxelSizeY);
  openElementRadius[2] = std::floor(.52 / m_voxelSizeZ);
  StructuringElementType openStructuringElement = StructuringElementType::Box(openElementRadius);

  using BinaryMorphologicalOpeningImageFilterType =
      itk::BinaryMorphologicalOpeningImageFilter<Uint8Image3DType, Uint8Image3DType, StructuringElementType>;
  BinaryMorphologicalOpeningImageFilterType::Pointer openFilter
      = BinaryMorphologicalOpeningImageFilterType::New();
  openFilter->SetKernel(openStructuringElement);
  openFilter->SetInput(maskFilter->GetOutput());
  openFilter->SetNumberOfThreads(m_numThreads);
  registerOperation(openFilter.GetPointer(), .25);

  using BinaryImageToLabelMapFilterType = itk::BinaryImageToLabelMapFilter<Uint8Image3DType, LabelMapType>;
  BinaryImageToLabelMapFilterType::Pointer binaryImageToLabelMapFilter
      = BinaryImageToLabelMapFilterType::New();
  binaryImageToLabelMapFilter->SetInput(openFilter->GetOutput());
  binaryImageToLabelMapFilter->SetFullyConnected(true);
  binaryImageToLabelMapFilter->SetNumberOfThreads(m_numThreads);
  registerOperation(binaryImageToLabelMapFilter.GetPointer(), .5);

  //  try {
  binaryImageToLabelMapFilter->Update();

  LabelMapType::ConstIterator labelObjectIt(binaryImageToLabelMapFilter->GetOutput());
  while (!labelObjectIt.IsAtEnd()) {
    const LabelMapType::LabelObjectType* labelObject = labelObjectIt.GetLabelObject();
    ++labelObjectIt;

    MatrixXi voxels(labelObject->Size(), 3);
    for(unsigned int pixelId=0; pixelId<labelObject->Size(); pixelId++) {
      LabelMapType::LabelObjectType::IndexType idx = labelObject->GetIndex(pixelId);
      voxels(pixelId, 0) = idx[0] + minLoc(0);
      voxels(pixelId, 1) = idx[1] + minLoc(1);
      voxels(pixelId, 2) = idx[2] + minLoc(2);
    }

    VectorXd voxelIntensities = getVoxelIntensities(voxels, m_img, m_punctaChannel);
    Punctum punc;
    punc.setVoxelIntensities(voxelIntensities);
    punc.setVoxelLocations(voxels);
    punc.updateFromVoxelsList(m_confRadius);
    if (somaDetection.containPunctum(punc))
      m_detectedSomaPuncta.push_back(punc);
  }

  LOG(INFO) << "Detected " << m_detectedSomaPuncta.size() << " puncta in soma.";
  //  }
  //  catch (itk::ExceptionObject & excp)
  //  {
  //    LOG(ERROR) << "Caught itk exception: " << excp.GetDescription();
  //  }

  return somaVoxels;
}
#endif

void ZPunctaDetection::cleanup()
{
  m_detectedPuncta.clear();
  m_detectedSomaPuncta.clear();
  m_filteredSomaPuncta.clear();
  m_filteredPuncta.clear();
}

QString ZPunctaDetection::getPunctaOutputFilename(const QString& swcPath)
{
  QFileInfo fi(swcPath);
  return fi.path() + "/" + fi.baseName() + "_puncta.nimp";
}

QString ZPunctaDetection::getSomaPunctaOutputFilename(const QString& swcPath)
{
  QFileInfo fi(swcPath);
  return fi.path() + "/" + fi.baseName() + "_soma_puncta.nimp";
}

QString ZPunctaDetection::getAmbiguousPunctaOutputFileName()
{
  QFileInfo fi(m_swcPaths[0]);
  return fi.path() + "/" + fi.baseName() + "_ambiguous_puncta.nimp";
}

QString ZPunctaDetection::getFilteredPunctaFilename()
{
  if (m_detectedPunctaFileName.isEmpty()) {
    return QString();
  } else {
    QFileInfo fi(m_detectedPunctaFileName);
    return fi.path() + "/" + fi.baseName() + "_filtered_puncta.nimp";
  }
}

QString ZPunctaDetection::getFilteredSomaPunctaFilename()
{
  if (m_detectedSomaPunctaFileName.isEmpty()) {
    return QString();
  } else {
    QFileInfo fi(m_detectedSomaPunctaFileName);
    return fi.path() + "/" + fi.baseName() + "_filtered_soma_puncta.nimp";
  }
}

void ZPunctaDetection::detectSomaMask(Eigen::MatrixXi& small, Eigen::MatrixXi& big)
{
  size_t numThreads = m_useMultithreading ? QThread::idealThreadCount() : 1;
  using Image3DType = itk::Image<uint8_t, 3>;
  using BinaryImage3DType = itk::Image<bool, 3>;
  Image3DType::Pointer image = wrapZImgChannelAsITKImg<uint8_t>(m_img, m_dendriteChannel, m_t);

  using BinaryThresholdImageFilterType = itk::BinaryThresholdImageFilter<Image3DType, BinaryImage3DType>;
  BinaryThresholdImageFilterType::Pointer thresholdFilter
    = BinaryThresholdImageFilterType::New();
  thresholdFilter->SetInput(image);
  thresholdFilter->SetLowerThreshold(m_dendriteThreshold);
  thresholdFilter->SetNumberOfThreads(numThreads);
  registerSubOperation(thresholdFilter.GetPointer(), .45 * .2);

  double tubeRadiusX = std::floor(m_maxDendriteTubeRadius / m_img.voxelSizeXInUm());
  double tubeRadiusY = std::floor(m_maxDendriteTubeRadius / m_img.voxelSizeYInUm());

  using StructuringElement2DType = itk::Neighborhood<bool, 2>;
  StructuringElement2DType::SizeType radius;
  radius[0] = tubeRadiusX;
  radius[1] = tubeRadiusY;
  StructuringElement2DType structuringElement;
  structuringElement.SetRadius(radius);
  itk::Offset<2> offset;
  for (int y = 0; y < tubeRadiusY * 2 + 1; ++y) {
    for (int x = 0; x < tubeRadiusX * 2 + 1; ++x) {
      offset[0] = x - tubeRadiusX;
      offset[1] = y - tubeRadiusY;
      structuringElement[offset] = (offset[0] * offset[0] / (tubeRadiusX * tubeRadiusX)
                                    + offset[1] * offset[1] / (tubeRadiusY * tubeRadiusY)
                                    <= 1.0);
    }
  }

  using BinaryImage2DType = itk::Image<bool, 2>;
  using BinaryMorphologicalOpeningImageFilterType
    = itk::BinaryMorphologicalOpeningImageFilter<BinaryImage2DType, BinaryImage2DType, StructuringElement2DType>;
  BinaryMorphologicalOpeningImageFilterType::Pointer openFilter
    = BinaryMorphologicalOpeningImageFilterType::New();
  openFilter->SetKernel(structuringElement);
  openFilter->SetNumberOfThreads(numThreads);

  using SliceBySliceImageFilterType = itk::SliceBySliceImageFilter<BinaryImage3DType, BinaryImage3DType>;
  SliceBySliceImageFilterType::Pointer sliceBySliceImageFilter
    = SliceBySliceImageFilterType::New();
  sliceBySliceImageFilter->SetFilter(openFilter);
  sliceBySliceImageFilter->SetInput(thresholdFilter->GetOutput());
  sliceBySliceImageFilter->SetNumberOfThreads(numThreads);
  registerSubOperation(sliceBySliceImageFilter.GetPointer(), .45 * .5);

  using StructuringElementType = itk::FlatStructuringElement<3>;
  StructuringElementType::RadiusType dlElementRadius;
  dlElementRadius[0] = std::max(1., std::floor(.32 / m_img.voxelSizeXInUm()));
  dlElementRadius[1] = std::max(1., std::floor(.32 / m_img.voxelSizeYInUm()));
  dlElementRadius[2] = std::max(1., std::floor(1.2 / m_img.voxelSizeZInUm()));
  StructuringElementType dlStructuringElement = StructuringElementType::Box(dlElementRadius);

  using BinaryDilateImageFilterType
    = itk::BinaryDilateImageFilter<BinaryImage3DType, BinaryImage3DType, StructuringElementType>;
  BinaryDilateImageFilterType::Pointer dilateFilter
    = BinaryDilateImageFilterType::New();
  dilateFilter->SetInput(sliceBySliceImageFilter->GetOutput());
  dilateFilter->SetKernel(dlStructuringElement);
  dilateFilter->SetNumberOfThreads(numThreads);
  registerSubOperation(dilateFilter.GetPointer(), .45 * .3);

  dilateFilter->Update();
  using ConstIteratorType = itk::ImageRegionConstIterator<BinaryImage3DType>;
  std::vector<int> coords;

  ConstIteratorType maskIt(dilateFilter->GetOutput(), dilateFilter->GetOutput()->GetLargestPossibleRegion());
  maskIt.GoToBegin();
  coords.clear();
  while (!maskIt.IsAtEnd()) {
    if (maskIt.Value()) {
      BinaryImage3DType::IndexType idx = maskIt.GetIndex();
      coords.push_back(idx[0]);
      coords.push_back(idx[1]);
      coords.push_back(idx[2]);
    }
    ++maskIt;
  }

  small = Eigen::MatrixXi();
  if (!coords.empty()) {
    small.resize(coords.size() / 3, 3);
    for (size_t i = 0; i < coords.size(); i += 3) {
      small(i / 3, 0) = coords[i];
      small(i / 3, 1) = coords[i + 1];
      small(i / 3, 2) = coords[i + 2];
    }
  }

  dlElementRadius[0] = std::max(1., std::floor(.42 / m_img.voxelSizeXInUm()));
  dlElementRadius[1] = std::max(1., std::floor(.42 / m_img.voxelSizeYInUm()));
  dlElementRadius[2] = std::max(1., std::floor(1.8 / m_img.voxelSizeZInUm()));
  dlStructuringElement = StructuringElementType::Box(dlElementRadius);
  dilateFilter->SetKernel(dlStructuringElement);
  dilateFilter->Update();

  ConstIteratorType maskIt2(dilateFilter->GetOutput(), dilateFilter->GetOutput()->GetLargestPossibleRegion());
  maskIt2.GoToBegin();
  coords.clear();
  while (!maskIt2.IsAtEnd()) {
    if (maskIt2.Value()) {
      BinaryImage3DType::IndexType idx = maskIt2.GetIndex();
      coords.push_back(idx[0]);
      coords.push_back(idx[1]);
      coords.push_back(idx[2]);
    }
    ++maskIt2;
  }

  big = Eigen::MatrixXi();
  if (!coords.empty()) {
    big.resize(coords.size() / 3, 3);
    for (size_t i = 0; i < coords.size(); i += 3) {
      big(i / 3, 0) = coords[i];
      big(i / 3, 1) = coords[i + 1];
      big(i / 3, 2) = coords[i + 2];
    }
  }
}

int ZPunctaDetection::cropOutSomaImg(ZImg& img, ZImg& somaImg, Eigen::RowVectorXi& minLoc)
{
  Eigen::MatrixXi somaMaskVoxelList;
  Eigen::MatrixXi bigSomaMaskVoxelList;
  detectSomaMask(somaMaskVoxelList, bigSomaMaskVoxelList);
  if (somaMaskVoxelList.rows() == 0) {
    return -1;
  }
  Eigen::RowVectorXi size;
  getVoxelRange(somaMaskVoxelList, minLoc, size);
  somaImg = cropZImg(somaMaskVoxelList, img, 0, 0, minLoc, size);

  ZImgAutoThreshold<> imgAutoThre;
  int somaThre = imgAutoThre.triangleThre<uint8_t>(somaImg, 0, 0);
  somaThre = std::max(somaThre, m_punctaThreshold);

  getVoxelRange(bigSomaMaskVoxelList, minLoc, size);
  somaImg = cropZImg(bigSomaMaskVoxelList, img, 0, 0, minLoc, size);

  for (int i = 0; i < somaMaskVoxelList.rows(); ++i) {
    *img.data<uint8_t>(somaMaskVoxelList(i, 0), somaMaskVoxelList(i, 1), somaMaskVoxelList(i, 2)) = 0;
  }

  return somaThre;
}

std::vector<Eigen::MatrixXi> ZPunctaDetection::watershedSplit(const ZImg& imgIn) const
{
  ZImg img = imgIn;
  int minIntensity;
  int maxIntensity;
  img.computeMinMax(minIntensity, maxIntensity);

  int m_floodStep = 1;
  int m_stopLevel = 1;
  int m_startLevel = std::max(maxIntensity - 10, m_stopLevel);

  std::vector<size_t> m_barrierVoxels;
  ConnComp CC;
  ZImg labelImg;
  uint8_t* imgData = img.channelData<uint8_t>(0, 0);
  LOG(INFO) << img.toQString();
  for (int level = m_startLevel; level >= m_stopLevel; level -= m_floodStep) {
    ZImg bim = img.binarized(level, ZImg::ThresholdMode::IncludeThreshold);

    ZImgConnectedComponents<> connComp;
    CC = connComp.runLabelModifyInput(bim);

    CC.removeSmallObject(m_seedSizeThreshold, true);

    //LOG(INFO) << level << " " << CC.voxelIdxList.size() << " " << CC.toatalNumVoxels();

    bool sep = false;
    if (!labelImg.isEmpty()) {
      uint32_t* labelData = labelImg.channelData<uint32_t>(0, 0);
      for (size_t obj = 0; obj < CC.voxelIdxList.size(); ++obj) {
        std::set<uint32_t> containedLabels;
        std::list<size_t> currentLevelVoxels;
        for (size_t v = 0; v < CC.voxelIdxList[obj].size(); ++v) {
          size_t idx = CC.voxelIdxList[obj][v];
          if (labelData[idx] > 0)
            containedLabels.insert(labelData[idx]);
          else
            currentLevelVoxels.push_back(idx);
        }

        if (containedLabels.size() > 1) {
          sep = true;
          // separate touched regions, set barrier voxels to 0
          while (!currentLevelVoxels.empty()) {
            std::list<size_t>::iterator lit;
            ZImgNeighborhoodConstIterator<uint32_t> nit(26, labelImg);
            for (lit = currentLevelVoxels.begin(); lit != currentLevelVoxels.end();) {
              nit.goToIndex(*lit);
              std::set<uint32_t> neighborLabels;
              for (size_t nb = 0; nb < nit.numNeighbors(); ++nb) {
                if (nit.isInBound(nb) && nit.valueRef(nb) > 0)
                  neighborLabels.insert(nit.valueRef(nb));
              }

              if (neighborLabels.empty()) {   // not yet
                ++lit;
              } else {
                if (neighborLabels.size() > 1) { // is barrier voxel, set voxel in image to 0
                  imgData[*lit] = 0;
                  m_barrierVoxels.push_back(*lit);
                }
                // assign center to first label
                labelData[*lit] = *neighborLabels.begin();
                lit = currentLevelVoxels.erase(lit);
              }
            }
          }
        }
      }
    }

    if (sep) {
      bim = img.binarized(level, ZImg::ThresholdMode::IncludeThreshold);
      CC = connComp.runLabelModifyInput(bim);
      CC.removeSmallObject(m_seedSizeThreshold, true);

      //LOG(INFO) << "sep " << level << " " << CC.voxelIdxList.size() << " " << CC.toatalNumVoxels();
    }
    labelImg = CC.createTypedLabelImg<uint32_t>();
  }

  uint32_t* labelData = labelImg.channelData<uint32_t>(0, 0);
  ZImgNeighborhoodConstIterator<uint32_t> nit(26, labelImg);
  for (size_t i = 0; i < m_barrierVoxels.size(); ++i) {
    nit.goToIndex(m_barrierVoxels[i]);
    CHECK(*nit == 0);
    for (size_t nb = 0; nb < nit.numNeighbors(); ++nb) {
      if (nit.isInBound(nb) && nit.valueRef(nb) > 0) {
        CC.voxelIdxList[nit.valueRef(nb) - 1].push_back(m_barrierVoxels[i]);
        labelData[m_barrierVoxels[i]] = nit.valueRef(nb);
        break;
      }
    }
  }

  LOG(INFO) << labelImg.toQString();
  LOG(INFO) << "number of voxels after watershed: " << CC.toatalNumVoxels() << " " << m_barrierVoxels.size();

  std::vector<Eigen::MatrixXi> m_labelObjects;
  for (size_t obj = 0; obj < CC.voxelIdxList.size(); ++obj) {
    Eigen::MatrixXi allVoxels(CC.voxelIdxList[obj].size(), 3);
    for (size_t v = 0; v < CC.voxelIdxList[obj].size(); ++v) {
      ZVoxelCoordinate coord = img.indexToCoord(CC.voxelIdxList[obj][v]);
      allVoxels(v, 0) = coord[0];
      allVoxels(v, 1) = coord[1];
      allVoxels(v, 2) = coord[2];
    }
    m_labelObjects.push_back(allVoxels);
  }

  return m_labelObjects;
}

void ZPunctaDetection::getVoxelRange(const Eigen::MatrixXi& voxelLocations, Eigen::RowVectorXi& minLoc,
                                     Eigen::RowVectorXi& size)
{
  minLoc = Eigen::RowVectorXi::Ones(3) * (-1);
  size = minLoc;
  if (voxelLocations.rows() > 0) {
    minLoc = voxelLocations.row(0);
    Eigen::RowVectorXi maxLoc = voxelLocations.row(0);
    for (int i = 1; i < voxelLocations.rows(); ++i) {
      minLoc = minLoc.cwiseMin(voxelLocations.row(i));
      maxLoc = maxLoc.cwiseMax(voxelLocations.row(i));
    }
    size = maxLoc - minLoc + Eigen::RowVectorXi::Ones(3);
  }
}

Eigen::VectorXd
ZPunctaDetection::getVoxelIntensities(const Eigen::MatrixXi& voxelLocations, const ZImg& img, size_t c, size_t t)
{
  Eigen::VectorXd voxelIntensities(voxelLocations.rows());
  for (int i = 0; i < voxelLocations.rows(); ++i) {
    voxelIntensities(i) = *img.data<uint8_t>(voxelLocations(i, 0), voxelLocations(i, 1), voxelLocations(i, 2), c, t);
  }
  return voxelIntensities;
}

ZImg ZPunctaDetection::cropZImg(const Eigen::MatrixXi& voxelLocations, const ZImg& img, size_t c, size_t t,
                                const Eigen::RowVectorXi& minLoc, const Eigen::RowVectorXi& size)
{
  ZImg res(nim::ZImgInfo(size(0), size(1), size(2)));
  size_t numZeros = 0;
  ZVoxelCoordinate minCoord = ZVoxelCoordinate(minLoc(0), minLoc(1), minLoc(2), c, t);
  for (int i = 0; i < voxelLocations.rows(); ++i) {
    ZVoxelCoordinate stackCoord = ZVoxelCoordinate(voxelLocations(i, 0), voxelLocations(i, 1), voxelLocations(i, 2), c,
                                                   t);
    ZVoxelCoordinate resCoord = stackCoord - minCoord;
    if (res.isCoordValid(resCoord)) {
      *res.data<uint8_t>(resCoord) = *img.data<uint8_t>(stackCoord);
      if (*res.data<uint8_t>(resCoord) == 0)
        numZeros++;
    }
  }
  LOG(INFO) << "number of zero in region: " << numZeros;
  return res;
}

ZImg ZPunctaDetection::cropZImg(const ZImg& img, size_t c, size_t t, const Eigen::RowVectorXi& minLoc,
                                const Eigen::RowVectorXi& size)
{
  ZImgRegion rgn(minLoc(0), minLoc(0) + size(0), minLoc(1), minLoc(1) + size(1), minLoc(2), minLoc(2) + size(2), c,
                 c + 1, t, t + 1);
  return img.crop(rgn);
}

size_t ZPunctaDetection::getNumCenters(const Eigen::MatrixXi& voxelLocations, const Eigen::VectorXd& voxelIntensities,
                                       const ZImg& locmax, double saturatedIntensity,
                                       const Eigen::RowVectorXi& minLocIn)
{
  size_t numCenter = 0;
  size_t numSaturateCenter = 0;
  for (int i = 0; i < voxelLocations.rows(); ++i) {
    if (*(locmax.data<uint8_t>(voxelLocations(i, 0) - minLocIn(0), voxelLocations(i, 1) - minLocIn(1),
                               voxelLocations(i, 2) - minLocIn(2))) > 0) {
      ++numCenter;
      if (voxelIntensities(i) == saturatedIntensity)
        ++numSaturateCenter;
    }
  }
  size_t numNotSaturatedCenter = numCenter - numSaturateCenter;
  if (numSaturateCenter > 11) {   // too many, use distance map center
    Eigen::RowVectorXi minLoc;
    Eigen::RowVectorXi size;
    getVoxelRange(voxelLocations, minLoc, size);
    ZImg cropCenter = cropZImg(voxelLocations, m_img, m_punctaChannel, m_t, minLoc, size);
    // leave only saturated center
    cropCenter = cropCenter.binarized(saturatedIntensity, ZImg::ThresholdMode::IncludeThreshold);
    cropCenter = cropCenter.projectAlongDim(Dimension::Z, ZImg::CombineMode::Max);

    ZImgSignedDistanceMap<> signedDM;
    signedDM.setInsideIsPositive(true);
    signedDM.setUseSquaredDistance(true);
    ZImg dmim = signedDM.run<int>(cropCenter, false);

    ZImgRegionalExtrema<> regionalExtrema;
    dmim = regionalExtrema.regionalMax(dmim);

    ZImgConnectedComponents<> connComp;
    ConnComp CC = connComp.runLabelModifyInput(dmim);
    size_t numDistCenter = CC.voxelIdxList.size();

    numCenter = numDistCenter + numNotSaturatedCenter;
  }
  // clamp
  if (numCenter < 1)
    numCenter = 1;
  if (numCenter > 12)
    numCenter = 12;
  return numCenter;
}

void
ZPunctaDetection::vbgmmSplit(const Eigen::MatrixXi& voxelLocs, const Eigen::VectorXd& voxelIntens, size_t numCenter,
                             const ZImg& img, ZPuncta& detectedPunctaList, double confRadius,
                             double confOverlapArea, double overlapRateThreshold, Eigen::RowVectorXi minLoc)
{
  Eigen::MatrixXd data = voxelLocs.cast<double>();
  int z = -1;
  if ((data.col(2).array() == data(0, 2)).all()) {
    LOG(INFO) << "      Data is 2D";
    z = data(0, 2);
    data.conservativeResize(Eigen::NoChange, 2);
  }
  Eigen::VectorXd weight = voxelIntens;
  Eigen::RowVectorXd dataCentre = ZEigenUtils::featureMean(data, weight);
  Eigen::MatrixXd m = dataCentre.colwise().replicate(numCenter);

  ZVBGMM<double, double> vbgmm(data, weight, numCenter, 10, m, 0.001,
                               ZTermCriteria<double>(200, 1e-5), IterAlgorithmLogLevel::Off);
  vbgmm.runEM();
  LOG(INFO) << "      Number of components after vbgmm: " << vbgmm.numOfClusters();
  // check if we can merge some models
  Eigen::MatrixXd centroids = meanShiftGaussianCenters(vbgmm, img, z);
  Eigen::VectorXi labels = vbgmm.labels();

  std::vector<std::vector<int>> modelGroups;
  std::vector<int> group1;
  group1.push_back(0);
  modelGroups.push_back(group1);
  for (size_t i = 1; i < vbgmm.numOfClusters(); ++i) {
    size_t currentModel = i;
    int currentModelGroup = -1;
    for (size_t g = 0; g < modelGroups.size(); ++g) {
      bool overlap = false;
      std::vector<int>& testGroup = modelGroups[g];
      for (size_t t = 0; !overlap && t < testGroup.size(); ++t) {
        int testModel = testGroup[t];

        //        LOG(INFO) << currentModel << " " << testModel << " " << getOverlapRateOfTwoErrorEllipse(centroids.row(currentModel), vbgmm.covar(currentModel),
        //                                                                                centroids.row(testModel), vbgmm.covar(testModel),
        //                                                                                confOverlapArea);

        if (getOverlapRateOfTwoErrorEllipse(centroids.row(currentModel), vbgmm.covar(currentModel),
                                            centroids.row(testModel), vbgmm.covar(testModel),
                                            confOverlapArea) >= overlapRateThreshold) {
          if (currentModelGroup == -1) {
            currentModelGroup = static_cast<int>(g);
            testGroup.push_back(currentModel);
          } else {  // currentmodel overlap with two group, merge these two group
            modelGroups[currentModelGroup].insert(modelGroups[currentModelGroup].end(),
                                                  testGroup.begin(), testGroup.end());
            testGroup.clear();
          }
          overlap = true;
        }
      }
    }
    // remove empty group
    modelGroups.erase(std::remove_if(modelGroups.begin(), modelGroups.end(), vectorIsEmpty), modelGroups.end());
    // create new group
    if (currentModelGroup == -1) {
      std::vector<int> newGroup;
      newGroup.push_back(currentModel);
      modelGroups.push_back(newGroup);
    }
  }

  LOG(INFO) << "      Number of components after merging vbgmm models: " << modelGroups.size();
  LOG(INFO) << "      Total number of voxels: " << voxelIntens.size();
  size_t numTotalVoxels = 0;
  for (size_t g = 0; g < modelGroups.size(); ++g) {
    Eigen::MatrixXi vbVoxelLocs(voxelIntens.size(), 3);
    Eigen::VectorXd vbVoxelIntens(voxelIntens.size());
    size_t numVoxel = 0;
    for (Eigen::Index l = 0; l < labels.rows(); ++l) {
      if (std::find(modelGroups[g].begin(), modelGroups[g].end(), labels(l)) != modelGroups[g].end()) {
        vbVoxelLocs.row(numVoxel) = voxelLocs.row(l) + minLoc;
        vbVoxelIntens(numVoxel++) = voxelIntens(l);
      }
    }
    vbVoxelLocs.conservativeResize(numVoxel, Eigen::NoChange);
    vbVoxelIntens.conservativeResize(numVoxel);
    numTotalVoxels += vbVoxelIntens.size();
    LOG(INFO) << "      Number of voxels in VBGMM component " << g + 1 << " : " << vbVoxelIntens.size();
    if (vbVoxelIntens.size() == 0) {
      LOG(ERROR) << vbgmm.labels();
    }
    ZPunctum punc;
    punc.setVoxelIntensities(vbVoxelIntens);
    punc.setVoxelLocations(vbVoxelLocs);
    punc.updateFromVoxelsList(confRadius);
    LOG(INFO) << "        Punctum: " << punc.x() << " " << punc.y() << " " << punc.z()
              << punc.maxIntensity() << punc.volSize() << punc.meanIntensity();
    detectedPunctaList.push_back(punc);
  }
  if (numTotalVoxels != static_cast<size_t>(voxelIntens.size())) {
    LOG(ERROR) << "voxel number doesn't match";
  }
}

} // namespace nim
