#include "zimgregistration.h"

//#include <itkMattesMutualInformationImageToImageMetricv4.h>
//#include <itkMeanSquaresImageToImageMetricv4.hxx>
//#include <itkBSplineTransform.h>
//#include <itkImageRegistrationMethodv4.h>
//#include <itkBSplineTransformParametersAdaptor.h>
//#include <itkRigid2DTransform.hxx>
//#include <itkLBFGSBOptimizerv4.h>

namespace nim {

ZImgRegistration::ZImgRegistration()
  : m_fixedImg(nullptr)
  , m_movingImg(nullptr)
  , m_costFunction(nullptr)
  , m_transform(nullptr)
  , m_useMultithreading(true)
  , m_numScales(1)
{
}

void ZImgRegistration::setCostFunction(ZRegistrationCostFunction& costFunction)
{
  m_costFunction = &costFunction;
}

void ZImgRegistration::setInitialTransform(ZImageTransform& tfm)
{
  m_transform = &tfm;
}

void ZImgRegistration::setOptimizer(const QString& str)
{
  if (str == "LBFGS") {
    m_optimizer.setLineSearchDirectionType(ceres::LBFGS);
  } else if (str == "BFGS") {
    m_optimizer.setLineSearchDirectionType(ceres::BFGS);
  } else if (str == "Steepest Descent") {
    m_optimizer.setLineSearchDirectionType(ceres::STEEPEST_DESCENT);
  } else if (str == "Nonlinear Conjugate Gradient") {
    m_optimizer.setLineSearchDirectionType(ceres::NONLINEAR_CONJUGATE_GRADIENT);
  } else {
    LOG(FATAL) << "impossible optimizer type selection";
  }
}

double ZImgRegistration::run()
{
  CHECK(m_transform);
  CHECK(m_costFunction);
  CHECK(m_fixedImg && m_movingImg && !m_fixedImg->isEmpty() && !m_movingImg->isEmpty());
  CHECK(!m_fixedImg->isTimeSeries() && !m_fixedImg->isMultiChannelsImg());

  m_costFunction->setTransform(*m_transform);
  m_optimizer.setCostFunction(*m_costFunction);
  m_costFunction->setUseMultithreading(m_useMultithreading);
  m_transform->adaptParameters(0, m_numScales - 1);

  for (int i = m_numScales - 1; i >= 0; --i) {
    double scale = std::pow(0.5, i);
    size_t sizeX = m_fixedImg->width();
    size_t sizeY = m_fixedImg->height();
    size_t sizeZ = m_fixedImg->depth();

    ZImg tfim;
    ZImg tmim;
    if (i == 0) {
      m_costFunction->setImages(*m_fixedImg, *m_movingImg);
    } else {
      tfim = m_fixedImg->zoomed(scale, scale, scale);
      tmim = m_movingImg->zoomed(scale, scale, scale);
      sizeX = tfim.width();
      sizeY = tfim.height();
      sizeZ = tfim.depth();
      m_costFunction->setImages(tfim, tmim);
    }
    double dims[3];
    dims[0] = sizeX;
    dims[1] = sizeY;
    dims[2] = sizeZ;

//    // rigid grid search
//    if (i == m_numScales-1) {
//      double bestCost = std::numeric_limits<double>::max();
//      std::vector<double> bestParams(costFunction.numParameters(), 0);
//      if (m_transform == "Affine") {
//        bestParams[3] = 1.0;
//        bestParams[4] = 1.0;
//      }
//      std::vector<double> params = bestParams;
//      ZImg img(ZImgInfo(sizeX+1, sizeY+1, 1, 1, 1, 1, 8, VoxelFormat::Float));
//      for (int transX = -(int)sizeX / 2; transX < (int)sizeX / 2; transX += 4) {
//        for (int transY = -(int)sizeY / 2; transY < (int)sizeY / 2; transY += 4) {
//          //for (double rot = -M_PI; rot < M_PI; rot += M_PI / 2) {
//          double rot = 0;
//            params[0] = transX;
//            params[1] = transY;
//            params[2] = rot;
//            double cost;
//            costFunction.evaluate(params.data(), &cost);
//            if (cost < bestCost) {
//              bestCost = cost;
//              bestParams = params;
//            }
//            img.setValue(cost, (transX+(int)sizeX/2)/4, (transY+(int)sizeY/2)/4//, 0);
//          //}
//        }
//      }
//      img.save("/Users/feng/Downloads/gridres.tif");
//      //parameters = bestParams;
//      LOG(INFO) << bestParams[0] << " " << bestParams[1] << " " << bestParams[2];
//    }

//    // shift centers of two images to one point, make sure two imgs have overlap
//    if (i == m_numScales-1) {
//      if (i == 0) {
//        double xMoment = 0;
//        double yMoment = 0;
//        double intenSum = 0;
//        for (size_t y=0; y<sizeY; ++y) {
//          for (size_t x=0; x<sizeX; ++x) {
//            double inten = fixedData[x + y*sizeX];
//            intenSum += inten;
//            xMoment += x * inten;
//            yMoment += y * inten;
//          }
//        }
//        double xCenter = xMoment / intenSum;
//        double yCenter = yMoment / intenSum;

//        xMoment = 0;
//        yMoment = 0;
//        intenSum = 0;
//        for (size_t y=0; y<sizeY; ++y) {
//          for (size_t x=0; x<sizeX; ++x) {
//            double inten = movingData[x + y*sizeX];
//            intenSum += inten;
//            xMoment += x * inten;
//            yMoment += y * inten;
//          }
//        }
//        double xShift = xCenter - xMoment / intenSum;
//        double yShift = yCenter - yMoment / intenSum;
//        parameters[0] = -xShift;
//        parameters[1] = -yShift;
//      } else {
//        double xMoment = 0;
//        double yMoment = 0;
//        double intenSum = 0;
//        for (size_t y=0; y<sizeY; ++y) {
//          for (size_t x=0; x<sizeX; ++x) {
//            double inten = tfim[x + y*sizeX];
//            intenSum += inten;
//            xMoment += x * inten;
//            yMoment += y * inten;
//          }
//        }
//        double xCenter = xMoment / intenSum;
//        double yCenter = yMoment / intenSum;

//        xMoment = 0;
//        yMoment = 0;
//        intenSum = 0;
//        for (size_t y=0; y<sizeY; ++y) {
//          for (size_t x=0; x<sizeX; ++x) {
//            double inten = tmim[x + y*sizeX];
//            intenSum += inten;
//            xMoment += x * inten;
//            yMoment += y * inten;
//          }
//        }
//        double xShift = xCenter - xMoment / intenSum;
//        double yShift = yCenter - yMoment / intenSum;
//        parameters[0] = -xShift;
//        parameters[1] = -yShift;
//      }
//    }

    LOG(INFO) << "";
    LOG(INFO) << "  " << "MultiResolution Level: " << i;

    if (i < m_numScales - 1) {
      m_transform->adaptParameters(i + 1, i);
    }
    LOG(INFO) << "  " << "Initial Parameters: " << m_transform->paraQString();
    std::vector<double> scales = m_transform->estimateParameterScales(dims);
    QString scalesQString = QString("%1").arg(scales[0]);
    for (size_t i = 1; i < scales.size(); ++i) {
      scalesQString += QString(" %1").arg(scales[i]);
    }
    LOG(INFO) << "  " << "Parameter Scales: " << scalesQString;

    m_optimizer.setParameterScales(scales);
    m_optimizer.setInitialParameters(m_transform->parameters());

    m_optimizer.minimize();

    m_transform->setParameters(m_optimizer.currentParameters());

    LOG(INFO) << "  " << "Final Parameters: " << m_transform->paraQString();
    LOG(INFO) << "Optimizer brief report: " << m_optimizer.briefReport();
  }
  return m_optimizer.finalCost();
}

} // namespace nim
