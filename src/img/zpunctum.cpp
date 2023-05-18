#include "zpunctum.h"

#include "zgmm.h"
#include "zrandom.h"
#include <boost/math/distributions/chi_squared.hpp>

namespace nim {

ZPunctum::ZPunctum(double x, double y, double z, double r)
  : m_x(x)
  , m_y(y)
  , m_z(z)
  , m_radius(r)
{
  updateVolSize();
  updateMass();
}

ZPunctum::ZPunctum(const Eigen::MatrixXi& loc, const Eigen::VectorXd& inten)
{
  m_voxelLocations = loc;
  m_voxelIntensities = inten;
  updateFromVoxelsList();
}

void ZPunctum::updateFromVoxelsList(double conf)
{
  CHECK(m_voxelIntensities.size() == m_voxelLocations.rows());
  if (m_voxelIntensities.size() <= 0) {
    LOG(ERROR) << "Zero element in voxel list.";
    return;
  }

  m_volSize = m_voxelLocations.rows();
  m_mass = m_voxelIntensities.sum();
  //  m_meanIntensity = m_mass / m_volSize;
  //  m_sDevOfIntensity =
  //    standardDeviation(m_voxelIntensities.data(), m_voxelIntensities.data() + m_voxelIntensities.size());
  std::tie(m_meanIntensity, m_sDevOfIntensity) =
    parallel_mean_and_sample_variance(m_voxelIntensities.data(), m_voxelIntensities.data() + m_voxelIntensities.size());
  m_maxIntensity = m_voxelIntensities.maxCoeff();

  Eigen::MatrixXd locs = m_voxelLocations.cast<double>();
  Eigen::VectorXd centroid = ZEigenUtils::featureMean(locs, m_voxelIntensities);
  m_x = centroid(0);
  m_y = centroid(1);
  m_z = centroid(2);
  Eigen::MatrixXd cov = ZEigenUtils::featureCovariance(locs, m_voxelIntensities, true);
  if (m_volSize == 1) {
    m_radius = .5;
  } else {
    Eigen::SelfAdjointEigenSolver<Eigen::MatrixXd> es(cov, Eigen::EigenvaluesOnly);
    boost::math::chi_squared dist(3);
    double k = std::sqrt(boost::math::quantile(dist, conf));
    m_radius = k * std::sqrt(es.eigenvalues().sum() / 3.);
  }

  // compute detection score (correlation coeff of image data and gaussian data)
  // This version only uses voxels belong to punctum. It is different from matlab version.
  // In matlab version, original image and all voxels inside conf region are used.
  if (m_volSize < 5) {
    m_score = 1.0; // too small
  } else {
    // double confRegion = 0.90;  // for detection score
    // double k2 = boost::math::quantile(dist, confRegion);
    //  move origin to centroid
    locs.rowwise() -= centroid.transpose();
    Eigen::MatrixXd cvs(locs.rows(), 2);
    cvs.col(0) = m_voxelIntensities;
    for (Eigen::Index r = 0; r < locs.rows(); ++r) {
      cvs(r, 1) = std::exp(-0.5 * locs.row(r) * cov * locs.row(r).transpose());
    }
    Eigen::MatrixXd corrcoef = ZEigenUtils::corrcoef(cvs);
    m_score = corrcoef(0, 1);
  }
}

void ZPunctum::mergeWith(const ZPunctum& other, double conf)
{
  if (containsSignal() && !other.containsSignal()) {
    return;
  }

  if (containsSignal()) {
    m_voxelIntensities.conservativeResize(m_voxelIntensities.size() + other.m_voxelIntensities.size());
    m_voxelIntensities.tail(other.m_voxelIntensities.size()) = other.m_voxelIntensities;
    m_voxelLocations.conservativeResize(m_voxelLocations.rows() + other.m_voxelLocations.rows(), Eigen::NoChange);
    m_voxelLocations.bottomRows(other.m_voxelLocations.rows()) = other.m_voxelLocations;
    updateFromVoxelsList(conf);
  } else {
    m_x *= m_volSize;
    m_y *= m_volSize;
    m_z *= m_volSize;

    m_volSize += other.m_volSize;
    m_sDevOfIntensity = std::max(other.m_sDevOfIntensity, m_sDevOfIntensity); // no better way..
    m_x += other.m_x * other.m_volSize;
    m_y += other.m_y * other.m_volSize;
    m_z += other.m_z * other.m_volSize;
    m_maxIntensity = std::max(other.m_maxIntensity, m_maxIntensity);
    m_mass += other.m_mass;

    m_x /= m_volSize;
    m_y /= m_volSize;
    m_z /= m_volSize;
    m_meanIntensity = m_mass / m_volSize;
    m_score = 1.0;
    updateRadius();
  }
}

std::list<ZPunctum> ZPunctum::split(size_t num, double conf) const
{
  std::list<ZPunctum> res;
  if (containsSignal()) {
    Eigen::MatrixXd locs = m_voxelLocations.cast<double>();
    ZGMM<double, double> gmm(locs,
                             m_voxelIntensities,
                             num,
                             true,
                             ZGMM<double, double>::CovarianceType::Full,
                             ZTermCriteria<double>(200, 1e-5),
                             IterAlgorithmLogLevel::Off);
    gmm.runEM();
    for (size_t i = 0; i < gmm.numOfClusters(); ++i) {
      ZPunctum p(*this);
      p.m_voxelIntensities = gmm.weight(i);
      p.m_voxelLocations = gmm.data(i).cast<int>();
      p.updateFromVoxelsList(conf);
      res.push_back(p);
    }
  }
  return res;
}

} // namespace nim
