#ifndef ZVBGMM_H
#define ZVBGMM_H

#include "zgmm.h"

namespace nim {

template<class T, class WeightT>
class ZVBGMM;

#ifndef _USE_QTCONCURRENT_

template<class T, class WeightT>
class _ZVBGMMReduce
{
  const ZVBGMM<T, WeightT>* m_vbgmm;
public:
  typename ZVBGMM<T, WeightT>::Params m_result;

  void operator()(const tbb::blocked_range<size_t>& range)
  {
    for (size_t i = range.begin(); i != range.end(); ++i) {
      typename ZVBGMM<T, WeightT>::Params inter = m_vbgmm->runOneAttempt();
      if (m_result.loglikHist < inter.loglikHist)
        m_result.swap(inter);
    }
  }

  _ZVBGMMReduce(_ZVBGMMReduce& x, tbb::split) : m_vbgmm(x.m_vbgmm)
  {}

  void join(const _ZVBGMMReduce& y)
  {
    if (m_result.loglikHist < y.m_result.loglikHist)
      m_result.swap(const_cast<typename ZVBGMM<T, WeightT>::Params&>(y.m_result));  //don't need intermediate result
  }

  _ZVBGMMReduce(const ZVBGMM<T, WeightT>* vbgmm)
    : m_vbgmm(vbgmm)
  {}
};

#else
template <class T, class WeightT>
typename ZVBGMM<T,WeightT>::Params ZVBGMMRunOneAttempt(const ZVBGMM<T,WeightT>* t)
{
  return t->runOneAttempt();
}

template <class T, class WeightT>
void ZVBGMMGetBestResult(typename ZVBGMM<T,WeightT>::Params &result, const typename ZVBGMM<T,WeightT>::Params& inter)
{
  if (result.loglikHist < inter.loglikHist)
    result.swap(const_cast<typename ZVBGMM<T,WeightT>::Params&>(inter));  //don't need intermediate result
}
#endif

template<class T, class WeightT = float>
class ZVBGMM
{
public:

  typedef typename MaxFloatType<T, WeightT>::type ResultDataType;
  typedef Eigen::Matrix<ResultDataType, Eigen::Dynamic, Eigen::Dynamic> MatrixXrt;  // matrix of result data type
  typedef Eigen::Matrix<ResultDataType, Eigen::Dynamic, 1> VectorXrt;  // vector of result data type
  typedef Eigen::Matrix<ResultDataType, 1, Eigen::Dynamic> RowVectorXrt; // row vector of result data type

  struct Params
  {
    Params()
      : loglikHist(std::numeric_limits<ResultDataType>::lowest())
      , iter(0)
    {}

    inline void swap(Params& other)
    {
      alpha.swap(other.alpha);
      beta.swap(other.beta);
      W.swap(other.W);
      invW.swap(other.invW);
      v.swap(other.v);
      m.swap(other.m);
      logPiTilde.swap(other.logPiTilde);
      logLambdaTilde.swap(other.logLambdaTilde);
      logWishartConst.swap(other.logWishartConst);
      entropy.swap(other.entropy);
      std::swap(logDirConst, other.logDirConst);
      rnk.swap(other.rnk);
      logrnk.swap(other.logrnk);
      std::swap(loglikHist, other.loglikHist);
      std::swap(iter, other.iter);
    }

    void display(const QString& paraName = "") const
    {
      LOG(INFO) << "VBGMM " << paraName << " alpha: " << alpha;
      LOG(INFO) << "VBGMM " << paraName << " beta: " << beta;
      LOG(INFO) << "VBGMM " << paraName << " v: " << v;
      LOG(INFO) << "VBGMM " << paraName << " m: " << m;
      LOG(INFO) << "VBGMM " << paraName << " logPiTilde: " << logPiTilde;
      LOG(INFO) << "VBGMM " << paraName << " logLambdaTilde: " << logLambdaTilde;
      LOG(INFO) << "VBGMM " << paraName << " logWishartConst: " << logWishartConst;
      LOG(INFO) << "VBGMM " << paraName << " entropy: " << entropy;
      LOG(INFO) << "VBGMM " << paraName << " logDirConst: " << logDirConst;
      LOG(INFO) << "VBGMM " << paraName << " rnk: " << rnk;
      LOG(INFO) << "VBGMM " << paraName << " logrnk: " << logrnk;
    }

    VectorXrt alpha;
    VectorXrt beta;
    std::vector<MatrixXrt> W;
    std::vector<MatrixXrt> invW;
    VectorXrt v;
    MatrixXrt m;
    VectorXrt logPiTilde;
    VectorXrt logLambdaTilde;
    VectorXrt logWishartConst;
    VectorXrt entropy;
    ResultDataType logDirConst;
    MatrixXrt rnk;
    MatrixXrt logrnk;
    ResultDataType loglikHist;
    size_t iter;
  };

#ifndef _USE_QTCONCURRENT_

  friend class _ZVBGMMReduce<T, WeightT>;

#else
  friend Params ZVBGMMRunOneAttempt<T,WeightT>(const ZVBGMM<T,WeightT>* t);
#endif

  ZVBGMM(const Eigen::Matrix<T, Eigen::Dynamic, Eigen::Dynamic>& data,
         size_t nclasses, size_t nattempts = 10,
         const MatrixXrt& m = MatrixXrt(0, 0), ResultDataType alpha0 = 0.001,
         ZTermCriteria<ResultDataType> termCriteria = ZTermCriteria<ResultDataType>(200, 1e-5),
         IterAlgorithmLogLevel logLevel = IterAlgorithmLogLevel::Off)
    : m_nclasses(nclasses), m_nattemps(nattempts), m_m0(m), m_alpha0(alpha0), m_termCriteria(termCriteria)
    , m_logLevel(logLevel), m_hasWeight(false), m_hasInitData(false)
  {
    if (std::is_same<T, ResultDataType>::value) {
      // reinterpret_cast allowed (AliasedType is (possibly cv-qualified) DynamicType)
      m_pData = reinterpret_cast<const MatrixXrt*>(&data);
    } else {
      m_NonIntegerData = data.template cast<ResultDataType>();
      m_pData = &m_NonIntegerData;
    }
    if (checkData())
      initPrior();
  }

  ZVBGMM(const Eigen::Matrix<T, Eigen::Dynamic, Eigen::Dynamic>& data,
         const Eigen::Matrix<WeightT, Eigen::Dynamic, 1>& weight,
         size_t nclasses, size_t nattempts = 10,
         const MatrixXrt& m = MatrixXrt(0, 0), ResultDataType alpha0 = 0.001,
         ZTermCriteria<ResultDataType> termCriteria = ZTermCriteria<ResultDataType>(200, 1e-5),
         IterAlgorithmLogLevel logLevel = IterAlgorithmLogLevel::Off)
    : m_nclasses(nclasses), m_nattemps(nattempts), m_m0(m), m_alpha0(alpha0), m_termCriteria(termCriteria),
    m_logLevel(logLevel), m_hasWeight(true), m_hasInitData(false)
  {
    bool hasZeroWeight = false;
    for (int i = 0; i < data.rows(); ++i) {
      if (weight(i) < std::numeric_limits<ResultDataType>::epsilon() * 1e3) {
        hasZeroWeight = true;
        break;
      }
    }
    if (hasZeroWeight) {
      m_NonIntegerData = MatrixXrt(data.rows(), data.cols());
      m_NonIntegerWeight = VectorXrt(weight.rows());
      int numRows = 0;
      for (int i = 0; i < data.rows(); ++i) {
        if (weight(i) >= std::numeric_limits<ResultDataType>::epsilon() * 1e3) {
          m_NonIntegerData.row(numRows) = data.row(i).template cast<ResultDataType>();
          m_NonIntegerWeight(numRows++) = weight(i);
        }
      }
      m_NonIntegerData.conservativeResize(numRows, Eigen::NoChange);
      m_NonIntegerWeight.conservativeResize(numRows);
      m_pData = &m_NonIntegerData;
      m_pWeight = &m_NonIntegerWeight;
    } else {
      if (std::is_same<T, ResultDataType>::value) {
        m_pData = reinterpret_cast<const MatrixXrt*>(&data);
      } else {
        m_NonIntegerData = data.template cast<ResultDataType>();
        m_pData = &m_NonIntegerData;
      }
      if (std::is_same<WeightT, ResultDataType>::value) {
        m_pWeight = reinterpret_cast<const VectorXrt*>(&weight);
      } else {
        m_NonIntegerWeight = weight.template cast<ResultDataType>();
        m_pWeight = &m_NonIntegerWeight;
      }
    }
    if (checkData())
      initPrior();
  }

  void setLogLevel(IterAlgorithmLogLevel logLevel)
  {
    m_logLevel = logLevel;
  }

  // note: only used for testing, don't call this otherwise
  void
  setInitData(const VectorXrt& mixingCoefficients, const MatrixXrt& centroids, const std::vector<MatrixXrt>& covars)
  {
    CHECK(mixingCoefficients.rows() == centroids.rows());
    CHECK(static_cast<size_t>(centroids.rows()) == covars.size());
    CHECK(centroids.cols() == m_pData->cols());
    CHECK(static_cast<size_t>(centroids.rows()) == m_nclasses);
    m_initMixingCoefficients = mixingCoefficients;
    m_initCentroids = centroids;
    m_initCovars = covars;
    m_hasInitData = true;
  }

  ResultDataType runEM(bool useMultithreading = true)
  {
    if (!m_hasEnoughData) {
      LOG(ERROR) << "Abort, Please check input data.";
      return -1;
    }

    if (useMultithreading && m_nattemps > 1) {
#ifndef _USE_QTCONCURRENT_
      _ZVBGMMReduce<T, WeightT> vbgmm(this);
      tbb::parallel_reduce(tbb::blocked_range<size_t>(0, m_nattemps), vbgmm);
      Params& result = vbgmm.m_result;
#else
      QList<ZVBGMM<T,WeightT>*> alllist;
      for (size_t i=0; i<m_nattemps; i++) alllist.append(this);
      Params result = QtConcurrent::blockingMappedReduced(alllist,
                                                          ZVBGMMRunOneAttempt<T,WeightT>,
                                                          ZVBGMMGetBestResult<T,WeightT>);
#endif
      composeResults(result);

      if (m_logLevel == IterAlgorithmLogLevel::Final || m_logLevel == IterAlgorithmLogLevel::Iter) {
        if (result.iter >= m_termCriteria.maxIter()) {
          LOG(INFO) << "VBGMM maximum number of iterations ("
                    << m_termCriteria.maxIter()
                    << ") has been exceeded.";
        }
        LOG(INFO) << "VBGMM Final Loglikelihood: " << result.loglikHist;
        LOG(INFO) << "VBGMM Final Centroids:\n" << centroids();
      }
      return result.loglikHist;
    }

    ResultDataType bestLogLikHist = std::numeric_limits<ResultDataType>::lowest();
    size_t finalIter = 0;
    for (size_t i = 0; i < m_nattemps; ++i) {
      //m_post.display("before init");
      if (m_hasInitData)
        initPostWithInitData(m_post);
      else
        initPostWithGMM(m_post);
      size_t iter = 0;
      bool done = false;
      ResultDataType oldLoglikHist = std::numeric_limits<ResultDataType>::infinity();
      ResultDataType loglikHist;
      //m_post.display("before loop");
      while (!done) {
        //LOG(INFO) << "-1";
        // E step
        //m_post.display("before infer");
        mixGaussBayesInfer(m_post);
        //m_post.display("after infer");
        VectorXrt Nk;
        MatrixXrt xbar;
        std::vector<MatrixXrt> S;
        computeEss(Nk, xbar, S, m_post);
        //LOG(INFO) << Nk;
        loglikHist = lowerBound(Nk, xbar, S, m_post);
        //LOG(INFO) << "out E";
        // M step
        Mstep(Nk, xbar, S, m_post);
        //m_post.display("after M");
        //LOG(INFO) << "out M";

        bool useSlopeCovergeTest = true;

        if (useSlopeCovergeTest) {
          //converged if the slope of the function falls below 'threshold',
          // i.e., |f(t) - f(t-1)| / avg < threshold,
          // where avg = (|f(t)| + |f(t-1)|)/2
          ResultDataType avg =
            (std::abs(loglikHist) + std::abs(oldLoglikHist) + std::numeric_limits<ResultDataType>::epsilon()) / 2;
          ResultDataType slope = std::abs(loglikHist - oldLoglikHist) / avg;
          if (iter > 0 && loglikHist - oldLoglikHist < -1.) {
            LOG(WARNING) << "Objective decreased! " << loglikHist << " " << oldLoglikHist;
          }
          done = m_termCriteria.meet(iter, slope);
        } else {
          done = m_termCriteria.meet(iter, std::abs(loglikHist - oldLoglikHist));
        }

        if (m_logLevel == IterAlgorithmLogLevel::Iter) {
          if (m_nattemps == 1) {
            LOG(INFO) << "VBGMM Iter: " << iter << " Loglikelihood: " << loglikHist;
          } else {
            LOG(INFO) << "VBGMM attempt " << i + 1 << " Iter: " << iter << " Loglikelihood: " << loglikHist;
          }
        }
        iter++;
        oldLoglikHist = loglikHist;
      }
      if (loglikHist > bestLogLikHist) {
        bestLogLikHist = loglikHist;
        composeResults(m_post);
        finalIter = iter - 1;
      }
    }
    if (m_logLevel == IterAlgorithmLogLevel::Final || m_logLevel == IterAlgorithmLogLevel::Iter) {
      if (finalIter >= m_termCriteria.maxIter()) {
        LOG(INFO) << "VBGMM maximum number of iterations ("
                  << m_termCriteria.maxIter()
                  << ") has been exceeded.";
      }
      LOG(INFO) << "VBGMM Final Loglikelihood: " << bestLogLikHist;
      LOG(INFO) << "VBGMM Final Centroids:\n" << centroids();
    }
    return bestLogLikHist;
  }

  inline int numOfClusters() const
  { return m_result.m.rows(); }

  inline MatrixXrt centroids() const
  { return m_result.m; }

  inline MatrixXrt covar(size_t compIdx) const
  { return m_result.invW[compIdx] / (m_result.v(compIdx) - m_dimension - 1); }

  Eigen::VectorXi labels() const
  {
    return m_resultLabels;
  }

  inline MatrixXrt responsiblities() const
  { return m_resultRnk; }

protected:

  bool checkData()   // check if there are enough data points to make m_nclasses initial clusters
  {
    m_hasEnoughData = true;
    if (m_nclasses == 0 || m_pData->rows() == 0) {
      m_nclasses = 0;
      m_hasEnoughData = false;
      LOG(ERROR) << "number of initial class or number of data points is 0";
      return false;
    }
    if (m_hasWeight && m_pWeight->size() < m_pData->rows()) {
      m_nclasses = 0;
      m_hasEnoughData = false;
      LOG(ERROR) << "weight data is not enough: number of weight value is " << m_pWeight->size()
                 << " , number of data points is " << m_pData->rows();
      return false;
    }

    size_t nUniqueData = 0;
    std::set<VectorXrt, ZVectorCompare<ResultDataType>> myset;
    std::pair<typename std::set<VectorXrt, ZVectorCompare<ResultDataType>>::iterator, bool> ret;
    for (int r = 0; r < m_pData->rows(); r++) {
      ret = myset.insert(m_pData->row(r));
      if (ret.second != false) {
        nUniqueData++;
      }
      if (nUniqueData > m_nclasses) {
        return true;
      }
    }

    if (nUniqueData <= m_nclasses) {
      // just reduce initial number of class
      m_nclasses = nUniqueData;
    }
    return true;
  }

  void initPrior()
  {
    m_dimension = m_pData->cols();
    if (m_m0.size() == 0) {  // init m use data center ??
      //RowVectorXni centre = ZEigenUtils::featureMean(*m_pData, *m_pWeight);
      //m = centre.colwise().replicate(m_nclasses);
      m_m0 = MatrixXrt::Zero(m_nclasses, m_pData->cols());
    } else {
      CHECK(m_m0.cols() == m_pData->cols() && m_m0.rows() == static_cast<int>(m_nclasses));
    }
    if (m_hasWeight) {
      // define a vague prior
      VectorXrt alpha = VectorXrt::Ones(m_nclasses) * m_alpha0;
      VectorXrt beta = VectorXrt::Ones(m_nclasses);   // low precision for mean
      std::vector<MatrixXrt> W;
      for (size_t i = 0; i < m_nclasses; i++) {
        W.push_back(200 * MatrixXrt::Identity(m_dimension, m_dimension));
      }
      VectorXrt v = VectorXrt::Ones(m_nclasses) * 20;
      mixGaussBayesStructure(alpha, beta, m_m0, v, W, false, m_prior);
    } else {
      // define a vague prior
      VectorXrt alpha = VectorXrt::Ones(m_nclasses) * m_alpha0;
      VectorXrt beta = VectorXrt::Ones(m_nclasses);   // low precision for mean
      std::vector<MatrixXrt> W;
      for (size_t i = 0; i < m_nclasses; i++) {
        W.push_back(200 * MatrixXrt::Identity(m_dimension, m_dimension));
      }
      VectorXrt v = VectorXrt::Ones(m_nclasses) * 20;
      mixGaussBayesStructure(alpha, beta, m_m0, v, W, false, m_prior);
    }
  }

  void initPostWithGMM(Params& post) const
  {
    if (m_hasWeight) {
      ResultDataType numData = m_pWeight->sum();
      ZGMM<ResultDataType, ResultDataType> gmm(*m_pData, *m_pWeight, m_nclasses,
                                               true, ZGMM<ResultDataType, ResultDataType>::CovarianceType::Full,
                                               ZTermCriteria<ResultDataType>(200, 1e-5));
      gmm.setLogLevel(m_logLevel);
      gmm.runEM();

      VectorXrt Nk = numData * gmm.priors();
      MatrixXrt xbar = gmm.centroids();
      std::vector<MatrixXrt> S = gmm.covars();

      if (gmm.numOfClusters() < m_nclasses) {
        Nk.conservativeResize(m_nclasses);
        xbar.conservativeResize(m_nclasses, m_dimension);
        for (size_t i = gmm.numOfClusters(); i < m_nclasses; ++i) {
          Nk(i) = 0;
          xbar.row(i) = RowVectorXrt::Zero(m_dimension);
          S.push_back(MatrixXrt::Zero(m_dimension, m_dimension));
        }
      }

      Mstep(Nk, xbar, S, post);
    } else {
      ResultDataType numData = m_pData->rows();
      ZGMM<ResultDataType, ResultDataType> gmm(*m_pData, m_nclasses,
                                               true, ZGMM<ResultDataType, ResultDataType>::CovarianceType::Full,
                                               ZTermCriteria<ResultDataType>(200, 1e-5));
      gmm.setLogLevel(m_logLevel);
      gmm.runEM();

      while (gmm.numOfClusters() != m_nclasses) {
        gmm.runEM();
      }

      VectorXrt Nk = numData * gmm.priors();
      MatrixXrt xbar = gmm.centroids();
      std::vector<MatrixXrt> S = gmm.covars();

      if (gmm.numOfClusters() < m_nclasses) {
        Nk.conservativeResize(m_nclasses);
        xbar.conservativeResize(m_nclasses, m_dimension);
        for (size_t i = gmm.numOfClusters(); i < m_nclasses; ++i) {
          Nk(i) = 0;
          xbar.row(i) = RowVectorXrt::Zero(m_dimension);
          S.push_back(MatrixXrt::Zero(m_dimension, m_dimension));
        }
      }

      Mstep(Nk, xbar, S, post);
    }
  }

  void initPostWithInitData(Params& post) const
  {
    ResultDataType numData;
    if (m_hasWeight) {
      numData = m_pWeight->sum();
    } else {
      numData = m_pData->rows();
    }
    VectorXrt Nk = numData * m_initMixingCoefficients;
    MatrixXrt xbar = m_initCentroids;
    std::vector<MatrixXrt> S = m_initCovars;

    Mstep(Nk, xbar, S, post);
  }

  void Mstep(VectorXrt& Nk, MatrixXrt& xbar, std::vector<MatrixXrt>& S, Params& post) const
  {
    VectorXrt alpha = m_prior.alpha + Nk; // 10.58
    VectorXrt beta = m_prior.beta + Nk;  // 10.60
    MatrixXrt m = MatrixXrt::Zero(m_nclasses, m_dimension);
    VectorXrt v = VectorXrt::Zero(m_nclasses);
    std::vector<MatrixXrt> invW;
    for (size_t k = 0; k < m_nclasses; k++) {
      if (Nk(k) < 0.001) { // extinguished
        m.row(k) = m_prior.m.row(k);
        invW.push_back(m_prior.invW[k]);
        v(k) = m_prior.v(k);
      } else {
        m.row(k) = (m_prior.beta(k) * m_prior.m.row(k) + Nk(k) * xbar.row(k)) / beta(k); //10.61
        invW.push_back(m_prior.invW[k] + Nk(k) * S[k]
                       + (m_prior.beta(k) * Nk(k) / (m_prior.beta(k) + Nk(k))) *
                         (xbar.row(k) - m_prior.m.row(k)).transpose() * (xbar.row(k) - m_prior.m.row(k))); // 10.62
        v(k) = m_prior.v(k) + Nk(k); //10.63
      }
    }
    mixGaussBayesStructure(alpha, beta, m, v, invW, true, post);
    //displayParams(m_post);
  }

  ResultDataType lowerBound(const VectorXrt& Nk, const MatrixXrt& xbar, const std::vector<MatrixXrt>& S,
                            const Params& post) const  // Bishop sec 10.2.2
  {
    // 10.71
    VectorXrt ElogpXall = VectorXrt::Zero(m_nclasses);
    for (size_t k = 0; k < m_nclasses; k++) {
      RowVectorXrt xbarc = xbar.row(k) - post.m.row(k);
      ElogpXall(k) = 0.5 * Nk(k) * (post.logLambdaTilde(k) - m_dimension / post.beta(k)
                                    - (post.v(k) * S[k] * post.W[k]).trace()
                                    - post.v(k) * (((xbarc * post.W[k]).array() * xbarc.array()).sum())
                                    - m_dimension * std::log(2 * M_PI));
    }
    ResultDataType ElogpX = ElogpXall.sum();

    // 10.72
    ResultDataType ElogpZ = (Nk.array() * post.logPiTilde.array()).sum();

    // 10.73
    ResultDataType Elogppi = m_prior.logDirConst + (post.logPiTilde * (m_alpha0 - 1)).sum();

    // 10.74
    VectorXrt ElogpmuSigmaAll = VectorXrt::Zero(m_nclasses);
    for (size_t k = 0; k < m_nclasses; k++) {
      RowVectorXrt mc = post.m.row(k) - m_prior.m.row(k);
      ElogpmuSigmaAll(k) = 0.5 * (m_dimension * std::log(m_prior.beta(k) / (2 * M_PI)) + post.logLambdaTilde(k) -
                                  m_dimension * m_prior.beta(k) / post.beta(k)
                                  - m_prior.beta(k) * post.v(k) * ((mc * post.W[k]).array() * mc.array()).sum()) +
                           m_prior.logWishartConst(k)
                           + 0.5 * (m_prior.v(k) - m_dimension - 1) * post.logLambdaTilde(k) -
                           0.5 * post.v(k) * ((m_prior.invW[k] * post.W[k]).trace());
    }
    ResultDataType ElogpmuSigma = ElogpmuSigmaAll.sum();

    // Entropy terms
    // 10.75//
    ResultDataType ElogqZ;
    if (m_hasWeight)
      ElogqZ = ((post.rnk.array() * post.logrnk.array()).rowwise().sum() * (*m_pWeight).array()).sum();
    else
      ElogqZ = (post.rnk.array() * post.logrnk.array()).sum();

    // 10.76
    ResultDataType Elogqpi = (post.logPiTilde.array() * (post.alpha.array() - 1)).sum() + post.logDirConst;

    // 10.77//
    ResultDataType ElogqmuSigma = (0.5 * post.logLambdaTilde.array() +
                                   m_dimension / 2. * log(post.beta.array() / (2 * M_PI)) - m_dimension / 2. -
                                   post.entropy.array()).sum();

    // overall sum
    // 10.70
    return ElogpX + ElogpZ + Elogppi + ElogpmuSigma - ElogqZ - Elogqpi - ElogqmuSigma;
  }

  void computeEss(VectorXrt& Nk, MatrixXrt& xbar, std::vector<MatrixXrt>& S, const Params& post) const
  {
    if (m_hasWeight) {
      Nk = post.rnk.cwiseProduct((*m_pWeight) * RowVectorXrt::Ones(post.rnk.cols())).colwise().sum();
      Nk = Nk.array() + 1e-10;
      xbar = MatrixXrt::Zero(m_nclasses, m_dimension);
      S.clear();
      for (size_t k = 0; k < m_nclasses; k++) {
        xbar.row(k) = (m_pData->array() * (post.rnk.col(k) * RowVectorXrt::Ones(m_dimension)).array() *
                       ((*m_pWeight) * RowVectorXrt::Ones(m_dimension)).array()).colwise().sum() / Nk(k); // 10.52
        MatrixXrt XC = m_pData->rowwise() - xbar.row(k);
        S.push_back((XC.array() * (post.rnk.col(k) * RowVectorXrt::Ones(m_dimension)).array() *
                     ((*m_pWeight) * RowVectorXrt::Ones(m_dimension)).array()).matrix().transpose() * XC /
                    Nk(k)); // 10.53
      }
    } else {
      Nk = post.rnk.colwise().sum();
      Nk = Nk.array() + 1e-10;
      xbar = MatrixXrt::Zero(m_nclasses, m_dimension);
      S.clear();
      for (size_t k = 0; k < m_nclasses; k++) {
        xbar.row(k) = (m_pData->array() * (post.rnk.col(k) * RowVectorXrt::Ones(m_dimension)).array()).colwise().sum() /
                      Nk(k); // 10.52
        MatrixXrt XC = m_pData->rowwise() - xbar.row(k);
        S.push_back(
          (XC.array() * (post.rnk.col(k) * RowVectorXrt::Ones(m_dimension)).array()).matrix().transpose() * XC /
          Nk(k)); // 10.53
      }
    }
  }

  // update rnk, logrnk
  // rnk = p(z=k|X(i,:), model) soft responsibility
  // return logprob of observed data log p(X(i,:) | model)
  VectorXrt mixGaussBayesInfer(Params& post) const
  {
    MatrixXrt E(m_pData->rows(), m_nclasses);
    for (size_t k = 0; k < m_nclasses; k++) {
      MatrixXrt XC = m_pData->rowwise() - post.m.row(k);
      E.col(k) = m_dimension / post.beta(k) +
                 (post.v(k) * (((XC * post.W[k]).cwiseProduct(XC)).rowwise().sum())).array(); // 10.64
    }
    MatrixXrt logRho =
      (post.logPiTilde + 0.5 * post.logLambdaTilde).transpose().colwise().replicate(m_pData->rows()) - 0.5 * E;
    VectorXrt logSumRho = ZEigenUtils::logsumexpRow(logRho);
    post.logrnk = logRho - logSumRho.rowwise().replicate(m_nclasses);
    post.rnk = exp(post.logrnk.array());
    return logSumRho;
  }

  void mixGaussBayesStructure(VectorXrt& alpha, VectorXrt& beta, MatrixXrt& m, VectorXrt& v,
                              std::vector<MatrixXrt>& W,
                              bool isInvW, Params& out) const
  {
    out.alpha.swap(alpha);
    out.beta.swap(beta);
    out.m.swap(m);
    out.v.swap(v);
    if (isInvW) {
      out.invW.swap(W);
      out.W.clear();
      for (size_t i = 0; i < m_nclasses; i++) {
        out.W.push_back(out.invW[i].inverse());
      }
    } else {
      out.W.swap(W);
      out.invW.clear();
      for (size_t i = 0; i < m_nclasses; i++) {
        out.invW.push_back(out.W[i].inverse());
      }
    }
    // precompute various functions of the distribution for speed
    //LOG(INFO) << out.alpha;
    //LOG(INFO) << out.alpha.sum();
    //out.display();
    out.logPiTilde = ZEigenUtils::matrixDigamma(out.alpha).array() - ZEigenUtils::digamma(out.alpha.sum());  //10.66
    out.logDirConst = ZEigenUtils::gammaln(out.alpha.sum()) - ZEigenUtils::matrixGammaln(out.alpha).sum(); // B.23
    out.logLambdaTilde = VectorXrt::Zero(m_nclasses);
    out.logWishartConst = VectorXrt::Zero(m_nclasses);
    out.entropy = VectorXrt::Zero(m_nclasses);
    for (size_t k = 0; k < m_nclasses; k++) {
      ResultDataType logdetW = ZEigenUtils::logdet(out.W[k]);
      VectorXrt tmp =
        (VectorXrt::LinSpaced(m_dimension, -static_cast<int>(m_dimension), -1).array() + out.v(k) + 1) * 0.5;
      out.logLambdaTilde(k) = ZEigenUtils::matrixDigamma(tmp).sum()
                              + m_dimension * std::log(2.0) + logdetW; // B.81
      out.logWishartConst(k) = -(out.v(k) / 2.0) * logdetW - (out.v(k) * m_dimension / 2.) * std::log(2.0)
                               - ZEigenUtils::mvtGammaln(m_dimension, out.v(k) / 2.0); // B.79
      out.entropy(k) = -out.logWishartConst(k) - (out.v(k) - m_dimension - 1) / 2 * out.logLambdaTilde(k)
                       + out.v(k) * m_dimension / 2.; // B.82
    }
  }

  Params runOneAttempt() const
  {
    Params post;
    if (m_hasInitData)
      initPostWithInitData(post);
    else
      initPostWithGMM(post);
    size_t iter = 0;
    bool done = false;
    ResultDataType loglikHist;
    while (!done) {
      // E step
      mixGaussBayesInfer(post);
      VectorXrt Nk;
      MatrixXrt xbar;
      std::vector<MatrixXrt> S;
      computeEss(Nk, xbar, S, post);
      loglikHist = lowerBound(Nk, xbar, S, post);
      // M step
      Mstep(Nk, xbar, S, post);

      bool useSlopeCovergeTest = true;

      if (useSlopeCovergeTest) {
        //converged if the slope of the function falls below 'threshold',
        // i.e., |f(t) - f(t-1)| / avg < threshold,
        // where avg = (|f(t)| + |f(t-1)|)/2
        ResultDataType avg =
          (std::abs(loglikHist) + std::abs(post.loglikHist) + std::numeric_limits<ResultDataType>::epsilon()) / 2;
        ResultDataType slope = std::abs(loglikHist - post.loglikHist) / avg;
        if (iter > 0 && loglikHist - post.loglikHist < -1.) {
          LOG(WARNING) << "Objective decreased! " << loglikHist << " " << post.loglikHist;
        }
        done = m_termCriteria.meet(iter, slope);
      } else {
        done = m_termCriteria.meet(iter, std::abs(loglikHist - post.loglikHist));
      }

      if (m_logLevel == IterAlgorithmLogLevel::Iter) {
        LOG(INFO) << "VBGMM Iter: " << iter << " Loglikelihood: " << loglikHist;
      }
      iter++;
      post.loglikHist = loglikHist;
    }
    post.iter = iter - 1;

    return post;
  }

  void composeResults(Params& post)   // remove extinguished components, assign labels
  {
    int idx = 0;
    m_result.alpha = VectorXrt::Zero(m_nclasses);
    m_result.beta = VectorXrt::Zero(m_nclasses);
    m_result.W.clear();
    m_result.invW.clear();
    m_result.v = VectorXrt::Zero(m_nclasses);
    m_result.m = MatrixXrt::Zero(m_nclasses, m_dimension);
    m_result.logDirConst = post.logDirConst;
    m_result.logLambdaTilde = VectorXrt::Zero(m_nclasses);
    m_result.logPiTilde = VectorXrt::Zero(m_nclasses);
    m_result.logWishartConst = VectorXrt::Zero(m_nclasses);
    m_result.entropy = VectorXrt::Zero(m_nclasses);
    m_resultRnk = MatrixXrt::Zero(post.rnk.rows(), post.rnk.cols());
    for (size_t k = 0; k < m_nclasses; k++) {
      if (post.v(k) > m_prior.v(k)) {
        m_result.alpha(idx) = post.alpha(k);
        m_result.beta(idx) = post.beta(k);
        m_result.W.push_back(post.W[k]);
        m_result.invW.push_back(post.invW[k]);
        m_result.v(idx) = post.v(k);
        m_result.m.row(idx) = post.m.row(k);
        m_result.logLambdaTilde(idx) = post.logLambdaTilde(k);
        m_result.logPiTilde(idx) = post.logPiTilde(k);
        m_result.logWishartConst(idx) = post.logWishartConst(k);
        m_result.entropy(idx) = post.entropy(k);
        m_resultRnk.col(idx) = post.rnk.col(k);
        idx++;
      }
    }
    m_result.alpha.conservativeResize(idx);
    m_result.beta.conservativeResize(idx);
    m_result.v.conservativeResize(idx);
    m_result.m.conservativeResize(idx, Eigen::NoChange);
    m_result.logLambdaTilde.conservativeResize(idx);
    m_result.logPiTilde.conservativeResize(idx);
    m_result.logWishartConst.conservativeResize(idx);
    m_result.entropy.conservativeResize(idx);
    m_resultRnk.conservativeResize(Eigen::NoChange, idx);
    // assign labels
    typename RowVectorXrt::Index index;
    m_resultLabels = Eigen::VectorXi::Zero(m_pData->rows());
    for (int i = 0; i < m_resultLabels.size(); i++) {
      m_resultRnk.row(i).maxCoeff(&index);
      m_resultLabels(i) = index;
    }
  }

private:

  Params m_prior;
  Params m_post;
  Params m_result;

  size_t m_dimension;   //dimension of the space
  size_t m_nclasses;    //number of mixture components
  size_t m_nattemps;
  MatrixXrt m_m0;       //prior mean positions
  ResultDataType m_alpha0;

  Eigen::VectorXi m_resultLabels;    //results components idx for each data

  MatrixXrt m_resultRnk; //nData*nResultClasses responsibilities matrix, entry ij represents class j's responsibility for data i

  ZTermCriteria<ResultDataType> m_termCriteria;
  IterAlgorithmLogLevel m_logLevel;
  bool m_hasWeight;

  MatrixXrt m_NonIntegerData;
  VectorXrt m_NonIntegerWeight;
  const MatrixXrt* m_pData;
  const VectorXrt* m_pWeight;

  bool m_hasEnoughData;      //

  bool m_hasInitData;
  VectorXrt m_initMixingCoefficients;
  MatrixXrt m_initCentroids;
  std::vector<MatrixXrt> m_initCovars;
};

} // namespace nim

#endif // ZVBGMM_H
