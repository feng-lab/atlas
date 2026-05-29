#include "zpunctadetection.h"
#include "zeigenutils.h"
#include "zkmeans.h"
#include "zgmm.h"
#include "zvbgmm.h"
#include "ztest.h"
#include <boost/math/distributions/chi_squared.hpp>

namespace {

inline Eigen::MatrixXd getOldFaithDataMatrix()
{
  Eigen::MatrixXd res(272, 2);
  res << 3.6, 79, 1.8, 54, 3.333, 74, 2.283, 62, 4.533, 85, 2.883, 55, 4.7, 88, 3.6, 85, 1.95, 51, 4.35, 85, 1.833, 54,
    3.917, 84, 4.2, 78, 1.75, 47, 4.7, 83, 2.167, 52, 1.75, 62, 4.8, 84, 1.6, 52, 4.25, 79, 1.8, 51, 1.75, 47, 3.45, 78,
    3.067, 69, 4.533, 74, 3.6, 83, 1.967, 55, 4.083, 76, 3.85, 78, 4.433, 79, 4.3, 73, 4.467, 77, 3.367, 66, 4.033, 80,
    3.833, 74, 2.017, 52, 1.867, 48, 4.833, 80, 1.833, 59, 4.783, 90, 4.35, 80, 1.883, 58, 4.567, 84, 1.75, 58, 4.533,
    73, 3.317, 83, 3.833, 64, 2.1, 53, 4.633, 82, 2, 59, 4.8, 75, 4.716, 90, 1.833, 54, 4.833, 80, 1.733, 54, 4.883, 83,
    3.717, 71, 1.667, 64, 4.567, 77, 4.317, 81, 2.233, 59, 4.5, 84, 1.75, 48, 4.8, 82, 1.817, 60, 4.4, 92, 4.167, 78,
    4.7, 78, 2.067, 65, 4.7, 73, 4.033, 82, 1.967, 56, 4.5, 79, 4, 71, 1.983, 62, 5.067, 76, 2.017, 60, 4.567, 78,
    3.883, 76, 3.6, 83, 4.133, 75, 4.333, 82, 4.1, 70, 2.633, 65, 4.067, 73, 4.933, 88, 3.95, 76, 4.517, 80, 2.167, 48,
    4, 86, 2.2, 60, 4.333, 90, 1.867, 50, 4.817, 78, 1.833, 63, 4.3, 72, 4.667, 84, 3.75, 75, 1.867, 51, 4.9, 82, 2.483,
    62, 4.367, 88, 2.1, 49, 4.5, 83, 4.05, 81, 1.867, 47, 4.7, 84, 1.783, 52, 4.85, 86, 3.683, 81, 4.733, 75, 2.3, 59,
    4.9, 89, 4.417, 79, 1.7, 59, 4.633, 81, 2.317, 50, 4.6, 85, 1.817, 59, 4.417, 87, 2.617, 53, 4.067, 69, 4.25, 77,
    1.967, 56, 4.6, 88, 3.767, 81, 1.917, 45, 4.5, 82, 2.267, 55, 4.65, 90, 1.867, 45, 4.167, 83, 2.8, 56, 4.333, 89,
    1.833, 46, 4.383, 82, 1.883, 51, 4.933, 86, 2.033, 53, 3.733, 79, 4.233, 81, 2.233, 60, 4.533, 82, 4.817, 77, 4.333,
    76, 1.983, 59, 4.633, 80, 2.017, 49, 5.1, 96, 1.8, 53, 5.033, 77, 4, 77, 2.4, 65, 4.6, 81, 3.567, 71, 4, 70, 4.5,
    81, 4.083, 93, 1.8, 53, 3.967, 89, 2.2, 45, 4.15, 86, 2, 58, 3.833, 78, 3.5, 66, 4.583, 76, 2.367, 63, 5, 88, 1.933,
    52, 4.617, 93, 1.917, 49, 2.083, 57, 4.583, 77, 3.333, 68, 4.167, 81, 4.333, 81, 4.5, 73, 2.417, 50, 4, 85, 4.167,
    74, 1.883, 55, 4.583, 77, 4.25, 83, 3.767, 83, 2.033, 51, 4.433, 78, 4.083, 84, 1.833, 46, 4.417, 83, 2.183, 55,
    4.8, 81, 1.833, 57, 4.8, 76, 4.1, 84, 3.966, 77, 4.233, 81, 3.5, 87, 4.366, 77, 2.25, 51, 4.667, 78, 2.1, 60, 4.35,
    82, 4.133, 91, 1.867, 53, 4.6, 78, 1.783, 46, 4.367, 77, 3.85, 84, 1.933, 49, 4.5, 83, 2.383, 71, 4.7, 80, 1.867,
    49, 3.833, 75, 3.417, 64, 4.233, 76, 2.4, 53, 4.8, 94, 2, 55, 4.15, 76, 1.867, 50, 4.267, 82, 1.75, 54, 4.483, 75,
    4, 78, 4.117, 79, 4.083, 78, 4.267, 78, 3.917, 70, 4.55, 79, 4.083, 70, 2.417, 54, 4.183, 86, 2.217, 50, 4.45, 90,
    1.883, 54, 1.85, 54, 4.283, 77, 3.95, 79, 2.333, 64, 4.15, 75, 2.35, 47, 4.933, 86, 2.9, 63, 4.583, 85, 3.833, 82,
    2.083, 57, 4.367, 82, 2.133, 67, 4.35, 74, 2.2, 54, 4.45, 83, 3.567, 73, 4.5, 73, 4.15, 88, 3.817, 80, 3.917, 71,
    4.45, 83, 2, 56, 4.283, 79, 4.767, 78, 4.533, 84, 1.85, 58, 4.25, 83, 1.983, 43, 2.25, 60, 4.75, 75, 4.117, 81,
    2.15, 46, 4.417, 90, 1.817, 46, 4.467, 74;
  return res;
}

} // namespace

using namespace nim;

TEST(cluster, MeanAndCovariance)
{
  using namespace Eigen;
  using namespace nim;

  MatrixXd mat = getOldFaithDataMatrix();

  MatrixXi mati = mat.cast<int>();

  RowVectorXd epMean(2);
  epMean << 3.487783088235294, 70.897058823529406;
  MatrixXd epCov(2, 2);
  epCov << 0.013027283328495, 0.139778078467549, 0.139778078467549, 1.848233123507706;
  epCov *= 100;

  RowVectorXd epMatIMean(2);
  epMatIMean << 2.977941176470588, 70.897058823529406;
  MatrixXd epMatICov(2, 2);
  epMatICov << 0.014755263729108, 0.145401562839158, 0.145401562839158, 1.848233123507706;
  epMatICov *= 100;

  double epLWShrunkShrinkage = 0.006304004428192;
  double LWShrunkShrinkage;
  MatrixXd epLWShCov(2, 2);
  epLWShCov << 0.018742694964032, 0.138386266412360, 0.138386266412360, 1.835674842729388;
  epLWShCov *= 100;
  MatrixXd lwshcov = ZEigenUtils::featureLWShrunkCovariance(mat, &LWShrunkShrinkage);

  double epLWDiagUnbiasShrinkage = 0.002426302799654;
  MatrixXd epLWDiagShrunkUnbiasCov(2, 2);
  epLWDiagShrunkUnbiasCov << 0.013027283328495, 0.139438934524433, 0.139438934524433, 1.848233123507706;
  epLWDiagShrunkUnbiasCov *= 100;
  double LWDiagUnbiasShrinkage;
  MatrixXd lwdshubcov = ZEigenUtils::featureLWDiagShrunkUnbiasCovariance(mat, &LWDiagUnbiasShrinkage);

  EXPECT_TRUE(epMean.isApprox(ZEigenUtils::featureMean(mat), 1e-9));
  EXPECT_TRUE(epCov.isApprox(ZEigenUtils::featureCovariance(mat), 1e-9));
  EXPECT_TRUE(epMatIMean.isApprox(ZEigenUtils::featureMean(mati), 1e-9));
  EXPECT_TRUE(epMatICov.isApprox(ZEigenUtils::featureCovariance(mati), 1e-9));
  EXPECT_NEAR(epLWShrunkShrinkage, LWShrunkShrinkage, 1e-9);
  EXPECT_TRUE(epLWShCov.isApprox(lwshcov, 1e-9));
  EXPECT_NEAR(epLWDiagUnbiasShrinkage, LWDiagUnbiasShrinkage, 1e-9);
  EXPECT_TRUE(epLWDiagShrunkUnbiasCov.isApprox(lwdshubcov, 1e-9));
}

TEST(cluster, KMeans)
{
  using namespace Eigen;
  using namespace nim;

  MatrixXd mat = getOldFaithDataMatrix();

  VectorXd weight = VectorXd::Ones(mat.rows());
  weight *= 1;
  weight(6) = 200;
  weight(26) = 300;
  MatrixXd remat = ZEigenUtils::replicateFeature(mat, weight);
  ZBenchTimer bt;

  ZKMeans<double, double> kmtest0(mat,
                                  weight,
                                  3,
                                  10,
                                  ZTermCriteria<double>(),
                                  ZKMeans<double, double>::InitCentersMethod::KmeansPP,
                                  IterAlgorithmLogLevel::Final);

  BENCH_AND_LOG(bt, 1, 1, kmtest0.run(false), "PP_st_w")

  BENCH_AND_LOG(bt, 1, 1, kmtest0.run(), "pp_mt_w")

  ZKMeans<double> kmtest01(mat,
                           3,
                           10,
                           ZTermCriteria<double>(),
                           ZKMeans<double>::InitCentersMethod::KmeansPP,
                           IterAlgorithmLogLevel::Final);

  BENCH_AND_LOG(bt, 1, 1, kmtest01.run(false), "pp_st")

  BENCH_AND_LOG(bt, 1, 1, kmtest01.run(), "pp_mt")

  ZKMeans<double, double> kmtest1(mat,
                                  weight,
                                  3,
                                  10,
                                  ZTermCriteria<double>(),
                                  ZKMeans<double, double>::InitCentersMethod::Random,
                                  IterAlgorithmLogLevel::Final);

  BENCH_AND_LOG(bt, 1, 1, kmtest1.run(false), "rnd_st_w")

  BENCH_AND_LOG(bt, 1, 1, kmtest1.run(), "rnd_mt_w")

  ZKMeans<double> kmtest11(mat,
                           3,
                           10,
                           ZTermCriteria<double>(),
                           ZKMeans<double>::InitCentersMethod::Random,
                           IterAlgorithmLogLevel::Final);

  BENCH_AND_LOG(bt, 1, 1, kmtest11.run(false), "rnd_st")

  BENCH_AND_LOG(bt, 1, 1, kmtest11.run(), "rnd_mt")

  ZKMeans<double, double, ZDistanceManhattan<double>> kmtest2(
    mat,
    weight,
    3,
    10,
    ZTermCriteria<double>(),
    ZKMeans<double, double, ZDistanceManhattan<double>>::InitCentersMethod::KmeansPP,
    IterAlgorithmLogLevel::Final);

  BENCH_AND_LOG(bt, 1, 1, kmtest2.run(false), "pp_st_w_mh")

  BENCH_AND_LOG(bt, 1, 1, kmtest2.run(), "pp_mt_w_mh")

  int a[10] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 0};
  int b[10] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1};
  LOG(INFO) << weightedMedian(a, a + 10, b, b + 10);
  LOG(INFO) << weightedMedian(a, a + 10, b, b + 10, true);

  MatrixXd mat3 = mat.replicate(2, 1);

  ZKMeans<double, double, ZDistanceManhattan<double>> kmtest22(
    mat3,
    3,
    10,
    ZTermCriteria<double>(),
    ZKMeans<double, double, ZDistanceManhattan<double>>::InitCentersMethod::KmeansPP,
    IterAlgorithmLogLevel::Final);

  BENCH_AND_LOG(bt, 1, 1, kmtest22.run(false), "pp_st_mh")

  BENCH_AND_LOG(bt, 1, 1, kmtest22.run(), "pp_mt_mh")

  ZKMeans<double, double, ZDistanceManhattan<double>> kmtest21(
    mat,
    3,
    10,
    ZTermCriteria<double>(),
    ZKMeans<double, double, ZDistanceManhattan<double>>::InitCentersMethod::KmeansPP,
    IterAlgorithmLogLevel::Final);

  BENCH_AND_LOG(bt, 1, 1, kmtest21.run(false), "pp_st_mh")

  BENCH_AND_LOG(bt, 1, 1, kmtest21.run(), "pp_mt_mh")

  ZKMeans<double, double> kmtest(mat, weight, 3, 10);
  double bestCompactness = kmtest.run();
  LOG(INFO) <<  "final centroids: " << kmtest.centroids();
  LOG(INFO) << "final potential: " << bestCompactness;
}

TEST(cluster, GMM)
{
  using namespace Eigen;
  using namespace nim;

  MatrixXd mat = getOldFaithDataMatrix();

  VectorXd weight = VectorXd::Ones(mat.rows());
  weight *= 1;
  weight(6) = 21;
  weight(26) = 31;
  MatrixXd remat = ZEigenUtils::replicateFeature(mat, weight);

  MatrixXd centroids(3, 2);

  ZBenchTimer bt;

  ZGMM<double> gmmtest(mat,
                       3,
                       true,
                       ZGMM<double>::CovarianceType::Full,
                       ZTermCriteria<double>(200, 1e-5),
                       IterAlgorithmLogLevel::Iter);
  centroids << 4.34461006289308, 81.1069182389937, 2.66736170212766, 63.9361702127660, 2.00784848484849,
    51.2575757575758;

  double epLoglikhood = 1119.644766;
  MatrixXd epCentroids(3, 2);
  epCentroids << 4.32132539268025, 80.4122594340757, 2.74434515252944, 62.3836311065018, 1.97587259934501,
    53.6441015733247;

  gmmtest.setInitData(centroids);
  bt.resetAndStart("GMM");
  double loglikhood = gmmtest.runEM();
  STOP_AND_LOG(bt)

  EXPECT_NEAR(epLoglikhood, loglikhood, 1e-5);
  EXPECT_TRUE(epCentroids.isApprox(gmmtest.centroids(), 1e-9));

  centroids.resize(2, 2);
  epCentroids.resize(2, 2);
  centroids << 4.33981250000001, 81.0885416666667, 2.06494615384616, 54.8076923076923;

  epLoglikhood = 1303.428657;
  epCentroids << 4.32682223180834, 80.7269011213336, 2.01538043178313, 54.5636085935003;

  ZGMM<double, double> gmmtestw(mat,
                                weight,
                                2,
                                true,
                                ZGMM<double, double>::CovarianceType::Full,
                                ZTermCriteria<double>(200, 1e-5),
                                IterAlgorithmLogLevel::Off);
  gmmtestw.setInitData(centroids);

  bt.resetAndStart("GMM with weight");
  loglikhood = gmmtestw.runEM();
  STOP_AND_LOG(bt)

  EXPECT_NEAR(epLoglikhood, loglikhood, 1e-5);
  EXPECT_TRUE(epCentroids.isApprox(gmmtestw.centroids(), 1e-9));

  ZGMM<double, double> gmmtestr(remat,
                                2,
                                true,
                                ZGMM<double, double>::CovarianceType::Full,
                                ZTermCriteria<double>(200, 1e-5),
                                IterAlgorithmLogLevel::Off);
  gmmtestr.setInitData(centroids);

  bt.resetAndStart("GMM with repmat");
  loglikhood = gmmtestr.runEM();
  STOP_AND_LOG(bt)

  EXPECT_NEAR(epLoglikhood, loglikhood, 1e-5);
  EXPECT_TRUE(epCentroids.isApprox(gmmtestw.centroids(), 1e-9));
}

TEST(cluster, VBGMM)
{
  using namespace Eigen;
  using namespace nim;

  MatrixXd mat = getOldFaithDataMatrix();

  VectorXd weight = VectorXd::Ones(mat.rows());
  weight *= 1;
  weight(6) = 21;
  weight(26) = 31;
  MatrixXd remat = ZEigenUtils::replicateFeature(mat, weight);

  VectorXd prior(6);
  MatrixXd centroids(6, 2);
  std::vector<MatrixXd> covars;
  for (int i = 0; i < 6; ++i) {
    covars.emplace_back(2, 2);
  }
  ZBenchTimer bt;

  ZVBGMM<double>
    vbgmmtest(mat, 6, 10, MatrixXd(0, 0), 0.001, ZTermCriteria<double>(200, 1e-5), IterAlgorithmLogLevel::Off);
  prior << 0.0854410412925517, 0.0887321214306351, 0.0993336254606369, 0.166806887388236, 0.287758648629299,
    0.271927675798641;
  centroids << 3.88032441163193, 70.9143408633333, 2.30404204477640, 60.0079687723917, 1.84032931720893,
    49.7495812282296, 2.00618927724826, 54.3037201853398, 4.23835223414998, 83.8307643479523, 4.46709333503216,
    78.6619493372870;
  covars[0] << 0.188938132841124, 1.178047296514962, 1.178047296514962, 16.526850023882997;
  covars[1] << 0.082449934833686, -0.095259188409701, -0.095259188409701, 24.600372962285146;
  covars[2] << 0.003708737106125, -0.037400981894731, -0.037400981894731, 12.839388121924005;
  covars[3] << 0.035985731291622, -0.380206949804224, -0.380206949804224, 21.146593028135630;
  covars[4] << 0.178997264529867, 1.049683840280841, 1.049683840280841, 25.054357151602147;
  covars[5] << 0.075149515313089, 0.118055355965879, 0.118055355965879, 11.515446798151082;
  double epLoglikhood = -1587.925763;
  MatrixXd epCentroids(2, 2);
  epCentroids << 1.99140376943641, 53.8135525360848, 4.24245952354588, 79.1657025196225;
  vbgmmtest.setInitData(prior, centroids, covars);
  bt.resetAndStart("Multithread version VBGMM");
  double loglikhood = vbgmmtest.runEM();
  STOP_AND_LOG(bt)
  EXPECT_NEAR(epLoglikhood, loglikhood, 1e-5);
  EXPECT_TRUE(epCentroids.isApprox(vbgmmtest.centroids(), 1e-9));

  bt.resetAndStart("Single Thread version VBGMM");
  loglikhood = vbgmmtest.runEM(false);
  STOP_AND_LOG(bt)
  EXPECT_NEAR(epLoglikhood, loglikhood, 1e-5);
  EXPECT_TRUE(epCentroids.isApprox(vbgmmtest.centroids(), 1e-9));

  prior << 0.119814180578843, 0.154727448939327, 0.102729539876930, 0.0433712995701079, 0.161017031982653,
    0.418340499052138;
  centroids << 2.41473223411622, 62.7709631541639, 1.94765675832747, 54.9150792529406, 1.98812552596795,
    48.4084888384817, 2.18949850054379, 53.7942967933476, 4.67185989649316, 86.9218812428909, 4.25314653247293,
    79.2620500074559;

  covars[0] << 0.439581996990205, 2.009154837000279, 2.009154837000279, 17.963003866039283;
  covars[1] << 0.004012832940521, 0.058865827536341, 0.058865827536341, 2.020242096827616;
  covars[2] << 0.036031713205226, 0.117540606666004, 0.117540606666004, 6.100508611688120;
  covars[3] << 0.142651086109089, 0.224315485317772, 0.224315485317772, 1.437210793104815;
  covars[4] << 0.026725857762148, 0.151618006441813, 0.151618006441813, 11.330334609530057;
  covars[5] << 0.133250572726010, -0.015234572031545, -0.015234572031545, 22.926727019261264;

  epLoglikhood = -2025.436807;
  epCentroids.resize(4, 2);
  epCentroids << 1.99122017583198, 53.8578635191351, 1.89544148682961, 53.0137439341479, 4.39904209926858,
    82.3849668290397, 4.24532339799796, 79.2057415135347;

  ZVBGMM<double, double>
    vbgmmtestw(mat, weight, 6, 10, MatrixXd(0, 0), 0.001, ZTermCriteria<double>(200, 1e-5), IterAlgorithmLogLevel::Off);
  vbgmmtestw.setInitData(prior, centroids, covars);

  bt.resetAndStart("Multithread version VBGMM with weight");
  loglikhood = vbgmmtestw.runEM();
  STOP_AND_LOG(bt)
  EXPECT_NEAR(epLoglikhood, loglikhood, 1e-5);
  EXPECT_TRUE(epCentroids.isApprox(vbgmmtestw.centroids(), 1e-9));

  bt.resetAndStart("Single Thread version VBGMM with weight");
  loglikhood = vbgmmtestw.runEM(false);
  STOP_AND_LOG(bt)
  EXPECT_NEAR(epLoglikhood, loglikhood, 1e-5);
  EXPECT_TRUE(epCentroids.isApprox(vbgmmtestw.centroids(), 1e-9));

  ZVBGMM<double, double>
    vbgmmtestr(remat, 6, 10, MatrixXd(0, 0), 0.001, ZTermCriteria<double>(200, 1e-5), IterAlgorithmLogLevel::Off);
  vbgmmtestr.setInitData(prior, centroids, covars);

  bt.resetAndStart("Multithread version VBGMM with repmat");
  loglikhood = vbgmmtestr.runEM();
  STOP_AND_LOG(bt)
  EXPECT_NEAR(epLoglikhood, loglikhood, 1e-5);
  EXPECT_TRUE(epCentroids.isApprox(vbgmmtestr.centroids(), 1e-9));

  bt.resetAndStart("Single Thread version VBGMM with repmat");
  loglikhood = vbgmmtestr.runEM(false);
  STOP_AND_LOG(bt)
  EXPECT_NEAR(epLoglikhood, loglikhood, 1e-5);
  EXPECT_TRUE(epCentroids.isApprox(vbgmmtestr.centroids(), 1e-9));
}

TEST(cluster, boostLib)
{
  using namespace Eigen;

  boost::math::chi_squared dist2(2); // two dimension
  double k = boost::math::quantile(dist2, .8);
  double expK = 3.218875824868201;
  double eps = 10e-9;
  EXPECT_NEAR(expK, k, eps);

  boost::math::chi_squared dist3(3);
  k = boost::math::quantile(dist3, .8);
  expK = 4.641627676087445;
  EXPECT_NEAR(expK, k, eps);

  RowVectorXd m1(2);
  RowVectorXd m2(2);
  MatrixXd cov1(2, 2);
  MatrixXd cov2(2, 2);
  m1 << 0, 0;
  m2 << 0, 0;
  cov1 << 1, 0, 0, 1;
  cov2 << .5, 0, 0, 1;
  expK = 1;
  k = ZPunctaDetection::getOverlapRateOfTwoErrorEllipse(m1, cov1, m2, cov2, 0.8);
  EXPECT_NEAR(expK, k, eps);
  m2 << 0, 1.3;
  cov2 << 1.5, .25, .25, 1.5;
  expK = 1;
  k = ZPunctaDetection::getOverlapRateOfTwoErrorEllipse(m1, cov1, m2, cov2, 0.8);
  EXPECT_NEAR(expK, k, eps);
  m2 << 0, 1.8;
  cov2 << .48, .1, .1, .48;
  expK = .424696968819728;
  k = ZPunctaDetection::getOverlapRateOfTwoErrorEllipse(m1, cov1, m2, cov2, 0.8);
  EXPECT_NEAR(expK, k, eps);
  cov2 << .24, .05, .05, .24;
  expK = .445295928628337;
  k = ZPunctaDetection::getOverlapRateOfTwoErrorEllipse(m1, cov1, m2, cov2, 0.8);
  EXPECT_NEAR(expK, k, eps);
  m2 << 0, 9;
  expK = 0;
  k = ZPunctaDetection::getOverlapRateOfTwoErrorEllipse(m1, cov1, m2, cov2, 0.8);
  EXPECT_NEAR(expK, k, eps);
}
