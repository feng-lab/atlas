#ifndef ZGMM_H
#define ZGMM_H

#include "zkmeans.h"

namespace nim {

template <class T, class WeightT = float>
class ZGMM
{
public:

  typedef typename MaxFloatType<T,WeightT>::type ResultDataType;
  typedef Eigen::Matrix<ResultDataType, Eigen::Dynamic, Eigen::Dynamic> MatrixXrt;  // matrix of result data type
  typedef Eigen::Matrix<ResultDataType, Eigen::Dynamic, 1> VectorXrt;  // vector of result data type
  typedef Eigen::Matrix<ResultDataType, 1, Eigen::Dynamic> RowVectorXrt; // row vector of result data type

  enum class CovarianceType
  {
    Spherical, Diag, Full, PPCA
  };

  ZGMM(const Eigen::Matrix<T, Eigen::Dynamic, Eigen::Dynamic> &data,
       size_t nclasses, bool checkCovars = true, CovarianceType covarType = CovarianceType::Full,
       ZTermCriteria<ResultDataType> termCriteria = ZTermCriteria<ResultDataType>(),
       IterAlgorithmLogLevel logLevel = IterAlgorithmLogLevel::Off)
    : m_nclasses(nclasses), m_checkCovars(checkCovars), m_covarType(covarType), m_termCriteria(termCriteria),
      m_logLevel(logLevel), m_hasWeight(false), m_labelsNeedUpdate(false), m_hasInitData(false)
  {
    if (std::is_same<T,ResultDataType>::value) {
      // reinterpret_cast allowed (AliasedType is (possibly cv-qualified) DynamicType)
      m_pData = reinterpret_cast<const MatrixXrt *>(&data);
    } else {
      m_NonIntegerData = data.template cast<ResultDataType>();
      m_pData = &m_NonIntegerData;
    }
    if (checkData()) {
      m_priors = VectorXrt::Ones(m_nclasses)/m_nclasses;
      for (size_t i=0; i<m_nclasses; i++) {
        m_covars.push_back(MatrixXrt::Identity(m_dimension, m_dimension));
      }
    } else {
      for (size_t i=0; i<m_nclasses; i++) {
        m_covars.push_back(MatrixXrt::Zero(m_dimension, m_dimension));
      }
    }
  }

  ZGMM(const Eigen::Matrix<T, Eigen::Dynamic, Eigen::Dynamic> &data, const Eigen::Matrix<WeightT, Eigen::Dynamic, 1> &weight,
       size_t nclasses, bool checkCovars = true, CovarianceType covarType = CovarianceType::Full,
       ZTermCriteria<ResultDataType> termCriteria = ZTermCriteria<ResultDataType>(),
       IterAlgorithmLogLevel logLevel = IterAlgorithmLogLevel::Off)
    : m_nclasses(nclasses), m_checkCovars(checkCovars), m_covarType(covarType), m_termCriteria(termCriteria),
      m_logLevel(logLevel), m_hasWeight(true), m_labelsNeedUpdate(false), m_hasInitData(false)
  {
    bool hasZeroWeight = false;
    for (int i=0; i<data.rows(); ++i) {
      if (weight(i) < std::numeric_limits<ResultDataType>::epsilon() * 1e3) {
        hasZeroWeight = true;
        break;
      }
    }
    if (hasZeroWeight) {
      m_NonIntegerData = MatrixXrt(data.rows(), data.cols());
      m_NonIntegerWeight = VectorXrt(weight.rows());
      int numRows = 0;
      for (int i=0; i<data.rows(); ++i) {
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
      if (std::is_same<T,ResultDataType>::value) {
        m_pData = reinterpret_cast<const MatrixXrt *>(&data);
      } else {
        m_NonIntegerData = data.template cast<ResultDataType>();
        m_pData = &m_NonIntegerData;
      }
      if (std::is_same<WeightT,ResultDataType>::value) {
        m_pWeight = reinterpret_cast<const VectorXrt *>(&weight);
      } else {
        m_NonIntegerWeight = weight.template cast<ResultDataType>();
        m_pWeight = &m_NonIntegerWeight;
      }
    }

    if (checkData()) {
      m_priors = VectorXrt::Ones(m_nclasses)/m_nclasses;
      for (size_t i=0; i<m_nclasses; i++) {
        m_covars.push_back(MatrixXrt::Identity(m_dimension, m_dimension));
      }
    } else {
      for (size_t i=0; i<m_nclasses; i++) {
        m_covars.push_back(MatrixXrt::Zero(m_dimension, m_dimension));
      }
    }
  }

  void setLogLevel(IterAlgorithmLogLevel logLevel)
  {
    m_logLevel = logLevel;
  }

  // note: only used for testing, don't call this otherwise
  void setInitData(const MatrixXrt &centroids)
  {
    assert(centroids.cols() == m_pData->cols());
    assert(static_cast<size_t>(centroids.rows()) == m_nclasses);
    m_centroids = centroids;
    m_hasInitData = true;
  }

  ResultDataType runEM()
  {
    if (!m_hasEnoughData) {
      if (m_logLevel == IterAlgorithmLogLevel::Iter || m_logLevel == IterAlgorithmLogLevel::Final) {
        LOG(INFO) << "GMM Data is not enough";
        LOG(INFO) << "GMM Final Centroids:\n" << m_centroids;
        LOG(INFO) << "GMM Final Loglikelihood: -1";
      }
      return -1;
    }

    if (m_hasInitData)
      initWithInitData();
    else
      initWithKMeans();

    ResultDataType min_covar;
    std::vector<MatrixXrt> init_covars;
    if (m_checkCovars) {
      min_covar = std::numeric_limits<ResultDataType>::epsilon();
      init_covars = m_covars;
    }
    ResultDataType likelihoodChange = std::numeric_limits<ResultDataType>::max();
    ResultDataType eold = std::numeric_limits<ResultDataType>::infinity();
    ResultDataType e = std::numeric_limits<ResultDataType>::infinity();
    size_t iter;
    for (iter=0; !m_termCriteria.meet(iter, likelihoodChange); iter++) {
      // remove empty components
      int ncls = m_nclasses;
      for (int j=0; j<ncls; ++j) {
        if (m_priors(j) < std::numeric_limits<ResultDataType>::epsilon()) {
          for (int k=j+1; k<ncls; ++k) {
            m_covars[k-1] = m_covars[k];
            m_priors(k-1) = m_priors(k);
            m_centroids.row(k-1) = m_centroids.row(k);
            m_responsibilities.col(k-1) = m_responsibilities.col(k);
          }
          m_labelsNeedUpdate = true;
          --j;
          --ncls;
        }
      }
      if (m_nclasses != static_cast<size_t>(ncls)) {
        m_nclasses = static_cast<size_t>(ncls);
        m_covars.resize(m_nclasses);
        m_priors.conservativeResize(m_nclasses);
        m_centroids.conservativeResize(m_nclasses, Eigen::NoChange);
        m_responsibilities.conservativeResize(Eigen::NoChange, m_nclasses);
      }

      // calculate posteriors based on old parameters
      MatrixXrt act = activations();
      posterior(act);
      // calculate error value
      VectorXrt prob = act * m_priors.transpose();
      // Error value is negative log likelihood of data
      if (m_hasWeight)
        e = -(prob.array().log() * m_pWeight->array()).sum();
      else
        e = -prob.array().log().sum();

      if (m_logLevel == IterAlgorithmLogLevel::Iter) {
        LOG(INFO) << "GMM Iter: " << iter << " Negative Loglikelihood: " << e;
        LOG(INFO) << "GMM Centroids:\n" << m_centroids;
      }
      if (m_termCriteria.willTestEPS()) {
        likelihoodChange = std::abs(e-eold);
        if (m_termCriteria.meet(iter, likelihoodChange)) {
          if (m_logLevel == IterAlgorithmLogLevel::Iter || m_logLevel == IterAlgorithmLogLevel::Final) {
            LOG(INFO) << "GMM Final Centroids:\n" << m_centroids;
            LOG(INFO) << "GMM Final Negative Loglikelihood: " << e;
          }
          return e;
        } else {
          eold = e;
        }
      }

      if (m_hasWeight) {    //substitute responsibilities with responsibilities.*weight
        // Adjust the new estimates for the parameters
        RowVectorXrt new_pr = (m_responsibilities.cwiseProduct((*m_pWeight)*RowVectorXrt::Ones(m_nclasses))).colwise().sum();
        MatrixXrt new_c = m_responsibilities.cwiseProduct((*m_pWeight)*RowVectorXrt::Ones(m_nclasses)).transpose() * (*m_pData);

        // Now move new estimates to old parameter vectors
        m_priors = new_pr / (m_pWeight->sum());
        m_centroids = new_c.array() / (new_pr.transpose() * RowVectorXrt::Ones(m_dimension)).array();

        switch (m_covarType) {
        case CovarianceType::Spherical: {
          MatrixXrt n2 = m_dist2(*m_pData, m_centroids);
          RowVectorXrt v(m_nclasses);
          for (size_t j=0; j<m_nclasses; j++) {
            VectorXrt colj = m_responsibilities.col(j).cwiseProduct(*m_pWeight);
            v(j) = colj.transpose() * n2.col(j);
          }
          for (size_t j=0; j<m_nclasses; j++) {
            ResultDataType covar = (v(j)/new_pr(j)) / m_dimension;
            if (m_checkCovars && covar < min_covar) {
              // don't change covar
            } else {
              m_covars[j].diagonal() = VectorXrt::Ones(m_dimension) * covar;
            }
          }

          break;
        }
        case CovarianceType::Diag: {
          for (size_t j=0; j<m_nclasses; j++) {
            MatrixXrt diffs = m_pData->rowwise() - m_centroids.row(j);
            diffs = diffs.array().square();
            VectorXrt colj = m_responsibilities.col(j).cwiseProduct(*m_pWeight);
            RowVectorXrt covar = diffs.cwiseProduct(colj*RowVectorXrt::Ones(m_dimension)).colwise().sum() / new_pr(j);
            if (m_checkCovars && covar.minCoeff() < min_covar) {
              // don't change covar
            } else {
              m_covars[j].diagonal() = covar;
            }
          }

          break;
        }
        case CovarianceType::Full: {
          for (size_t j=0; j<m_nclasses; j++) {
            MatrixXrt diffs = m_pData->rowwise() - m_centroids.row(j);
            VectorXrt colj = m_responsibilities.col(j).cwiseProduct(*m_pWeight);
            diffs = diffs.cwiseProduct(colj.cwiseSqrt() * RowVectorXrt::Ones(m_dimension));
            MatrixXrt covar = (diffs.transpose() * diffs) / new_pr(j);
            if (m_checkCovars && ZEigenUtils::rank(covar) < m_dimension) {
              // don't change covar
              if (m_logLevel == IterAlgorithmLogLevel::Iter) {
                LOG(INFO) << "GMM check covars is on, rank of covar low:\n" << covar;
              }
            } else {
              m_covars[j] = covar;
            }
          }

          break;
        }
        case CovarianceType::PPCA: {

        }
        }
      } else {
        // Adjust the new estimates for the parameters
        RowVectorXrt new_pr = m_responsibilities.colwise().sum();
        MatrixXrt new_c = m_responsibilities.transpose() * (*m_pData);

        // Now move new estimates to old parameter vectors
        m_priors = new_pr / m_pData->rows();
        m_centroids = new_c.array() / (new_pr.transpose() * RowVectorXrt::Ones(m_dimension)).array();

        switch (m_covarType) {
        case CovarianceType::Spherical: {
          MatrixXrt n2 = m_dist2(*m_pData, m_centroids);
          RowVectorXrt v(m_nclasses);
          for (size_t j=0; j<m_nclasses; j++) {
            v(j) = m_responsibilities.col(j).transpose() * n2.col(j);
          }
          for (size_t j=0; j<m_nclasses; j++) {
            ResultDataType covar = (v(j)/new_pr(j)) / m_dimension;
            if (m_checkCovars && covar < min_covar) {
              // don't change covar
            } else {
              m_covars[j].diagonal() = VectorXrt::Ones(m_dimension) * covar;
            }
          }

          break;
        }
        case CovarianceType::Diag: {
          for (size_t j=0; j<m_nclasses; j++) {
            MatrixXrt diffs = m_pData->rowwise() - m_centroids.row(j);
            diffs = diffs.array().square();
            RowVectorXrt covar = diffs.cwiseProduct(m_responsibilities.col(j)*RowVectorXrt::Ones(m_dimension)).colwise().sum() / new_pr(j);
            if (m_checkCovars && covar.minCoeff() < min_covar) {
              // don't change covar
            } else {
              m_covars[j].diagonal() = covar;
            }
          }

          break;
        }
        case CovarianceType::Full: {
          for (size_t j=0; j<m_nclasses; j++) {
            MatrixXrt diffs = m_pData->rowwise() - m_centroids.row(j);
            VectorXrt colj = m_responsibilities.col(j);
            diffs = diffs.cwiseProduct(colj.cwiseSqrt() * RowVectorXrt::Ones(m_dimension));
            MatrixXrt covar = (diffs.transpose() * diffs) / new_pr(j);
            if (m_checkCovars && ZEigenUtils::rank(covar) < m_dimension) {
              // don't change covar
              if (m_logLevel == IterAlgorithmLogLevel::Iter) {
                LOG(INFO) << "GMM check covars is on, rank of covar low:\n" << covar;
              }
            } else {
              m_covars[j] = covar;
            }
          }

          break;
        }
        case CovarianceType::PPCA: {

        }
        }
      }
    }

    // calculate posteriors based on old parameters
    MatrixXrt act = activations();
    posterior(act);
    // calculate error value
    VectorXrt prob = act * m_priors.transpose();
    // Error value is negative log likelihood of data
    if (m_hasWeight)
      e = -(prob.array().log() * m_pWeight->array()).sum();
    else
      e = -prob.array().log().sum();
    if (m_logLevel == IterAlgorithmLogLevel::Iter || m_logLevel == IterAlgorithmLogLevel::Final) {
      if (iter >= m_termCriteria.maxIter()) {
        LOG(INFO) << "GMM maximum number of iterations ("
                  << m_termCriteria.maxIter()
                  << ") has been exceeded.";
      }
      LOG(INFO) << "GMM Final Centroids:\n" << m_centroids;
      LOG(INFO) << "GMM Final Negative Loglikelihood: " << e;
    }
    return e;
  }

  inline size_t numOfClusters() const {return m_nclasses; }
  inline MatrixXrt centroids() const { return m_centroids; }
  inline VectorXrt priors() const { return m_priors.transpose(); }
  inline MatrixXrt covar(size_t compIdx) const { return m_covars[compIdx]; }
  inline std::vector<MatrixXrt> covars() const { return m_covars; }
  Eigen::VectorXi labels()
  {
    if (m_labelsNeedUpdate) {
      typename RowVectorXrt::Index index;
      for (int i=0; i<m_labels.size(); i++) {
        m_responsibilities.row(i).maxCoeff(&index);
        m_labels(i) = index;
      }
      m_labelsNeedUpdate = false;
    }
    return m_labels;
  }
  inline MatrixXrt responsiblities() const { return m_responsibilities; }
  // return data belongs to class compIdx
  MatrixXrt data(size_t compIdx)
  {
    MatrixXrt res = *m_pData;
    Eigen::VectorXi label = labels();
    int num = 0;
    for (int i=0; i < label.size(); ++i) {
      if (label(i) == static_cast<int>(compIdx))
        res.row(num++) = m_pData->row(i);
    }
    res.conservativeResize(num, Eigen::NoChange);
    return res;
  }
  // return weight belongs to class compIdx
  VectorXrt weight(size_t compIdx)
  {
    VectorXrt res = *m_pWeight;
    Eigen::VectorXi label = labels();
    int num = 0;
    for (int i=0; i < label.size(); ++i) {
      if (label(i) == static_cast<int>(compIdx))
        res(num++) = (*m_pWeight)(i);
    }
    res.conservativeResize(num);
    return res;
  }

protected:

  void initWithInitData()
  {
    // get labels
    MatrixXrt allDists = m_dist2(*m_pData, m_centroids);
    m_labels.resize(m_pData->rows());
    for (int r=0; r < m_pData->rows(); r++) {
      typename MatrixXrt::Index index;
      allDists.row(r).minCoeff(&index);
      m_labels(r) = static_cast<int>(index);
    }
    initPriors();
  }

  //  The k-means algorithm is used to determine the initial centres.
  void initWithKMeans()
  {
    if (!m_hasEnoughData)
      return;

    if (m_hasWeight) {
      ZKMeans<ResultDataType, ResultDataType> km(*m_pData, *m_pWeight, m_nclasses, 10);
      km.setLogLevel(m_logLevel);
      km.run();
      m_centroids = km.centroids();
      m_labels = km.labels();
    } else {
      ZKMeans<ResultDataType> km(*m_pData, m_nclasses, 10);
      km.setLogLevel(m_logLevel);
      km.run();
      m_centroids = km.centroids();
      m_labels = km.labels();
    }
    initPriors();
  }

  bool checkData()   // check if there are enough data points to make m_nclasses clusters
  {
    if (m_nclasses == 0 || m_pData->rows() == 0) {
      m_hasEnoughData = false;
      m_nclasses = 0;
      LOG(ERROR) << "number of class or number of data points is 0";
      return false;
    }
    if (m_hasWeight && m_pWeight->size() < m_pData->rows()) {
      m_hasEnoughData = false;
      m_nclasses = 0;
      LOG(ERROR) << "weight data is not enough: number of weight value is " << m_pWeight->size()
                 << " , number of data points is " << m_pData->rows();
      return false;
    }

    m_dimension = m_pData->cols();

    size_t nUniqueData = 0;
    m_uniqueDatas.resize(m_nclasses+1, m_pData->cols());
    m_uniqueDatas.row(nUniqueData++) = m_pData->row(0);
    for (int r=1; r < m_pData->rows(); r++) {
      MatrixXrt dist = m_dist2(m_uniqueDatas.topRows(nUniqueData), m_pData->row(r));
      if ((dist.array() <= Eigen::NumTraits<ResultDataType>::dummy_precision()).any()) {  // duplicate
        continue;
      }
      m_uniqueDatas.row(nUniqueData++) = m_pData->row(r);
      if (nUniqueData > m_nclasses) {
        m_hasEnoughData = true;
        return true;
      }
    }

    if (nUniqueData <= m_nclasses) {
      m_hasEnoughData = false;
      m_nclasses = nUniqueData;
      for (size_t i=0; i<m_nclasses; i++) {
        m_covars.push_back(MatrixXrt::Zero(m_dimension, m_dimension));
      }
      m_uniqueDatas.conservativeResize(nUniqueData, Eigen::NoChange);
      // get labels
      m_uniqueLabels.resize(m_pData->rows());
      MatrixXrt allDists = m_dist2(*m_pData, m_uniqueDatas);
      for (int r=0; r < m_pData->rows(); r++) {
        typename MatrixXrt::Index index;
        allDists.row(r).minCoeff(&index);
        m_uniqueLabels(r) = static_cast<int>(index);
      }
      m_centroids = m_uniqueDatas;
      m_labels = m_uniqueLabels;
    }
    return false;
  }

  //  computes the activations A (i.e. the  probability
  //  P(X|J) of the data conditioned on each component density)  for a
  //  Gaussian mixture model.  For the PPCA model, each activation is the
  //  conditional probability of X given that it is generated by the
  //  component subspace.
  MatrixXrt activations()
  {
    if (!m_hasEnoughData)
      return MatrixXrt();

    MatrixXrt a(m_pData->rows(), m_nclasses);
    switch (m_covarType) {
    case CovarianceType::Spherical: {
      MatrixXrt n2 = m_dist2(*m_pData, m_centroids);
      RowVectorXrt covars(m_nclasses);
      for (size_t i=0; i<m_nclasses; i++) {
        covars(i) = m_covars[i](0,0);
      }
      // calculate width factors
      MatrixXrt wi2 = VectorXrt::Ones(m_pData->rows()) * (covars*2);
      MatrixXrt normal = (wi2 * M_PI).array().pow(m_dimension/2.0);
      a = exp(-n2.array()/wi2.array()) / normal.array();
      break;
    }
    case CovarianceType::Diag: {
      ResultDataType normal = std::pow(2*M_PI, m_dimension/2.0);
      for (size_t j=0; j<m_nclasses; j++) {
        ResultDataType s = m_covars[j].diagonal().cwiseSqrt().prod();
        MatrixXrt diffs = m_pData->rowwise() - m_centroids.row(j);
        diffs = diffs.array().square();
        a.col(j) = exp(-0.5*(diffs.cwiseQuotient(VectorXrt::Ones(m_pData->rows()) * m_covars[j].diagonal())).rowwise().sum().array()) / (normal*s);
      }
      break;
    }
    case CovarianceType::Full: {
      ResultDataType normal = std::pow(2*M_PI, m_dimension/2.0);
      for (size_t j=0; j<m_nclasses; j++) {
        MatrixXrt diffs = m_pData->rowwise() - m_centroids.row(j);
        Eigen::LLT<MatrixXrt> chol;
        chol.compute(m_covars[j]);
        MatrixXrt c = chol.matrixU();
        MatrixXrt temp = diffs * c.inverse();
        temp = temp.array().square();
        a.col(j) = exp(-0.5*temp.rowwise().sum().array()) / (normal*c.diagonal().prod());
      }
      break;
    }
    case CovarianceType::PPCA: {
      break;
    }
    }
    return a;
  }

  //computes the posteriors responsibilities (i.e. the probability of
  //each component conditioned on the data P(J|X)) for a Gaussian mixture
  //model. need activations as input.
  void posterior(MatrixXrt &a)
  {
    if (!m_hasEnoughData)
      return;

    m_responsibilities = a.cwiseProduct(VectorXrt::Ones(m_pData->rows()) * m_priors);
    VectorXrt s = m_responsibilities.rowwise().sum();
    for (int i=0; i<s.size(); i++) {
      if (s(i) == 0) {
        LOG(WARNING) << "Some zero posterior probabilities";
        s(i) = 1;
        m_responsibilities.row(i) = RowVectorXrt::Ones(m_nclasses) / m_nclasses;
      }
    }
    m_responsibilities = m_responsibilities.cwiseQuotient(s*RowVectorXrt::Ones(m_nclasses));
    m_labelsNeedUpdate = true;
  }

  //  The priors are computed from the proportion of examples belonging to each
  //  cluster. The covariance matrices are calculated as the sample
  //  covariance of the points associated with (i.e. closest to) the
  //  corresponding centres. For a mixture of PPCA model, the PPCA
  //  decomposition is calculated for the points closest to a given centre.
  //  This initialisation can be used as the starting point for training
  //  the model using the EM algorithm.
  void initPriors()
  {
    if (m_hasWeight) {
      VectorXrt clusterSizes = VectorXrt::Zero(m_nclasses);
      for (int i=0; i<m_labels.rows(); i++) {
        clusterSizes(m_labels(i)) += (*m_pWeight)(i);
      }
      for (size_t i=0; i<m_nclasses; i++) {
        m_priors(i) = clusterSizes(i)/clusterSizes.sum();
      }
      Eigen::VectorXi counts(m_nclasses);
      std::vector<std::unique_ptr<MatrixXrt>> sepMats;
      std::vector<std::unique_ptr<VectorXrt>> sepWeights;
      std::vector<int> sepMatsRowIdxs;
      switch (m_covarType) {
      case CovarianceType::Spherical:
        if (m_nclasses > 1) {
          MatrixXrt cdist = m_dist2(m_centroids, m_centroids);
          cdist.diagonal() = VectorXrt::Ones(m_nclasses) * std::numeric_limits<ResultDataType>::max();
          for (size_t i=0; i<m_nclasses; i++) {
            ResultDataType covar = cdist.col(i).minCoeff();
            if (covar < std::numeric_limits<ResultDataType>::epsilon()) {
              covar = 1;
            }
            m_covars[i].diagonal() = VectorXrt::Ones(m_dimension) * covar;
          }
        } else {
          ResultDataType covar = ZEigenUtils::featureCovariance(*m_pData, *m_pWeight).diagonal().mean();
          m_covars[0].diagonal() = VectorXrt::Ones(m_dimension) * covar;
        }
        break;
      case CovarianceType::Diag:
        // Pick out data points belonging to this centre
        counts.setZero();
        for (int r=0; r < m_labels.rows(); r++) {
          counts(m_labels(r))++;
        }

        for (int i=0; i<counts.rows(); i++) {
          sepMats.emplace_back(std::make_unique<MatrixXrt>(counts(i), m_dimension));
          sepWeights.emplace_back(std::make_unique<VectorXrt>(counts(i)));
          sepMatsRowIdxs.push_back(0);
        }

        for (int r=0; r < m_pData->rows(); r++) {
          sepMats[m_labels(r)]->row(sepMatsRowIdxs[m_labels(r)]++) = m_pData->row(r);
          sepWeights[m_labels(r)]->row(sepMatsRowIdxs[m_labels(r)]-1) = (*m_pWeight).row(r);
        }

        for (size_t i=0; i<m_nclasses; i++) {
          // get cov
          MatrixXrt covar = ZEigenUtils::featureCovariance(*(sepMats[i]), *(sepWeights[i]));
          for (size_t j=0; j<m_dimension; j++) {
            if (covar(j,j) < std::numeric_limits<ResultDataType>::epsilon()) {
              covar(j,j) = 1;
            }
          }
          m_covars[i].diagonal() = covar.diagonal();
        }
        break;
      case CovarianceType::Full:
        // Pick out data points belonging to this centre
        counts.setZero();
        for (int r=0; r < m_labels.rows(); r++) {
          counts(m_labels(r))++;
        }

        for (int i=0; i<counts.rows(); i++) {
          sepMats.emplace_back(std::make_unique<MatrixXrt>(counts(i), m_dimension));
          sepWeights.emplace_back(std::make_unique<VectorXrt>(counts(i)));
          sepMatsRowIdxs.push_back(0);
        }

        for (int r=0; r < m_pData->rows(); r++) {
          sepMats[m_labels(r)]->row(sepMatsRowIdxs[m_labels(r)]++) = m_pData->row(r);
          sepWeights[m_labels(r)]->row(sepMatsRowIdxs[m_labels(r)]-1) = m_pWeight->row(r);
        }

        for (size_t i=0; i<m_nclasses; i++) {
          // get cov
          m_covars[i] = ZEigenUtils::featureCovariance(*(sepMats[i]), *(sepWeights[i]), true, true);
          if (ZEigenUtils::rank(m_covars[i]) < m_dimension) {
            m_covars[i] += MatrixXrt::Identity(m_dimension, m_dimension);
          }
        }
        break;
      case CovarianceType::PPCA:
        break;
      }
    } else {
      VectorXrt clusterSizes = VectorXrt::Zero(m_nclasses);
      for (int i=0; i<m_labels.rows(); i++) {
        clusterSizes(m_labels(i)) += 1;
      }
      for (size_t i=0; i<m_nclasses; i++) {
        m_priors(i) = clusterSizes(i)/clusterSizes.sum();
      }
      Eigen::VectorXi counts(m_nclasses);
      std::vector<std::unique_ptr<MatrixXrt>> sepMats;
      std::vector<int> sepMatsRowIdxs;
      switch (m_covarType) {
      case CovarianceType::Spherical:
        if (m_nclasses > 1) {
          MatrixXrt cdist = m_dist2(m_centroids, m_centroids);
          cdist.diagonal() = VectorXrt::Ones(m_nclasses) * std::numeric_limits<ResultDataType>::max();
          for (size_t i=0; i<m_nclasses; i++) {
            ResultDataType covar = cdist.col(i).minCoeff();
            if (covar < std::numeric_limits<ResultDataType>::epsilon()) {
              covar = 1;
            }
            m_covars[i].diagonal() = VectorXrt::Ones(m_dimension) * covar;
          }
        } else {
          ResultDataType covar = ZEigenUtils::featureCovariance(*m_pData).diagonal().mean();
          m_covars[0].diagonal() = VectorXrt::Ones(m_dimension) * covar;
        }
        break;
      case CovarianceType::Diag:
        // Pick out data points belonging to this centre
        counts.setZero();
        for (int r=0; r < m_labels.rows(); r++) {
          counts(m_labels(r))++;
        }

        for (int i=0; i<counts.rows(); i++) {
          sepMats.emplace_back(std::make_unique<MatrixXrt>(counts(i), m_dimension));
          sepMatsRowIdxs.push_back(0);
        }

        for (int r=0; r < m_pData->rows(); r++) {
          sepMats[m_labels(r)]->row(sepMatsRowIdxs[m_labels(r)]++) = m_pData->row(r);
        }

        for (size_t i=0; i<m_nclasses; i++) {
          // get cov
          MatrixXrt covar = ZEigenUtils::featureCovariance(*(sepMats[i]));
          for (size_t j=0; j<m_dimension; j++) {
            if (covar(j,j) < std::numeric_limits<ResultDataType>::epsilon()) {
              covar(j,j) = 1;
            }
          }
          m_covars[i].diagonal() = covar.diagonal();
        }
        break;
      case CovarianceType::Full:
        // Pick out data points belonging to this centre
        counts.setZero();
        for (int r=0; r < m_labels.rows(); r++) {
          counts(m_labels(r))++;
        }

        for (int i=0; i<counts.rows(); i++) {
          sepMats.emplace_back(std::make_unique<MatrixXrt>(counts(i), m_dimension));
          sepMatsRowIdxs.push_back(0);
        }

        for (int r=0; r < m_pData->rows(); r++) {
          sepMats[m_labels(r)]->row(sepMatsRowIdxs[m_labels(r)]++) = m_pData->row(r);
        }

        for (size_t i=0; i<m_nclasses; i++) {
          // get cov
          m_covars[i] = ZEigenUtils::featureCovariance(*(sepMats[i]), true);
          if (ZEigenUtils::rank(m_covars[i]) < m_dimension) {
            m_covars[i] += MatrixXrt::Identity(m_dimension, m_dimension);
          }
        }
        break;
      case CovarianceType::PPCA:
        break;
      }
    }
  }

private:
  size_t m_dimension;   //dimension of the space
  size_t m_nclasses;    //number of mixture components
  bool m_checkCovars;  // Ensure that covariances don't collapse
  CovarianceType m_covarType;  // type of covariance model
  RowVectorXrt m_priors;  // mixing coefficients
  MatrixXrt m_centroids; // means of Gaussians stored as rows of matrix
  Eigen::VectorXi m_labels;

  std::vector<MatrixXrt> m_covars;  // covariances of Gaussians
  MatrixXrt m_responsibilities; // nData*nclasses responsibilities matrix, entry ij represents class j's responsibility for data i


  ZTermCriteria<ResultDataType> m_termCriteria;
  IterAlgorithmLogLevel m_logLevel;
  bool m_hasWeight;
  bool m_labelsNeedUpdate;

  MatrixXrt m_NonIntegerData;
  VectorXrt m_NonIntegerWeight;
  const MatrixXrt *m_pData;
  const VectorXrt *m_pWeight;

  bool m_hasEnoughData;      //
  MatrixXrt m_uniqueDatas;    //This will be the centroids if we don't have enough data
  Eigen::VectorXi m_uniqueLabels;   //see above

  ZDistanceEuclideanSquared<ResultDataType> m_dist2;

  bool m_hasInitData;
};

} // namespace nim

#endif // ZGMM_H
