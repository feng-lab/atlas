#include "zimagematrix2dtransform.h"

namespace {

void getAffineParameterScales(double width, double height, double* scaleRotation = nullptr,
                              double* scaleScaleX = nullptr, double* scaleScaleY = nullptr,
                              double* scaleShearXY = nullptr, double* scaleShearYX = nullptr)
{
  double dt = 1;
  double axy = std::atan(height / width);
  double bxy = M_PI / 2 - axy;
  double Kxy = std::pow(width, 3) * (std::sin(axy) / std::pow(std::cos(axy), 2) +
                                     std::log(std::abs(std::tan((M_PI / 4) + (axy / 2))))) +
               std::pow(height, 3) * (std::sin(bxy) / std::pow(std::cos(bxy), 2)
                                      + std::log(std::abs(std::tan((M_PI / 2) - (axy / 2)))));
  if (scaleRotation)
    *scaleRotation = std::abs(2 * std::asin((6 * dt * width * height) / Kxy));
  if (scaleScaleX)
    *scaleScaleX = 4 * dt / width;
  if (scaleScaleY)
    *scaleScaleY = 4 * dt / height;
  if (scaleShearXY)
    *scaleShearXY = 4 * dt / height;
  if (scaleShearYX)
    *scaleShearYX = 4 * dt / width;
}

} // empty namespace

namespace nim {

ZImageMatrix2DTransform::ZImageMatrix2DTransform()
  : ZImageTransform()
  , m_centerX(0)
  , m_centerY(0)
{
  std::vector<double>(6, 0).swap(m_parameters);
  m_parameters[0] = 1;
  m_parameters[4] = 1;
}

void ZImageMatrix2DTransform::transformRange(double inXMin, double inXMax, double inYMin, double inYMax,
                                             double& outXMin, double& outXMax, double& outYMin, double& outYMax) const
{
  double outCoords[8];
  outCoords[0] = outCoords[2] = inXMin;
  outCoords[4] = outCoords[6] = inXMax;
  outCoords[1] = outCoords[5] = inYMin;
  outCoords[3] = outCoords[7] = inYMax;

  transformPointInverse(outCoords);
  outXMin = outXMax = outCoords[0];
  outYMin = outYMax = outCoords[1];
  for (size_t i = 2; i < 8; i += 2) {
    transformPointInverse(outCoords + i);
    outXMin = std::min(outXMin, outCoords[i]);
    outXMax = std::max(outXMax, outCoords[i]);
    outYMin = std::min(outYMin, outCoords[i + 1]);
    outYMax = std::max(outYMax, outCoords[i + 1]);
  }
}

void ZImageMatrix2DTransform::transformPointInverse(double* inoutCoords) const
{
  CHECK(inoutCoords);
  const Eigen::Matrix3d& mat = m_tform.inverseTransformMatrix();

  double inCoords[2];
  inCoords[0] = inoutCoords[0];
  inCoords[1] = inoutCoords[1];
  inoutCoords[0] =
    mat(0, 0) * (inCoords[0] - m_centerX) + mat(0, 1) * (inCoords[1] - m_centerY) + mat(0, 2) + m_centerX;
  inoutCoords[1] =
    mat(1, 0) * (inCoords[0] - m_centerX) + mat(1, 1) * (inCoords[1] - m_centerY) + mat(1, 2) + m_centerY;
}

size_t ZImageMatrix2DTransform::numParameters() const
{
  return 6;
}

void ZImageMatrix2DTransform::setParameters(const double* para)
{
  m_tform.reset();
  m_tform.setMatrix(para[0], para[1], para[2],
                    para[3], para[4], para[5]);
  m_parameters = std::vector<double>(para, para + 6);
}

void ZImageMatrix2DTransform::adaptParameters(size_t fromLevel, size_t toLevel)
{
  if (fromLevel == toLevel)
    return;
  double scale = std::pow(2.0, double(fromLevel) - double(toLevel));
  m_centerX *= scale;
  m_centerY *= scale;
  m_parameters[2] *= scale;
  m_parameters[5] *= scale;
  setParameters(m_parameters.data());
}

void ZImageMatrix2DTransform::transformPoint(double* inoutCoords) const
{
  CHECK(inoutCoords);
  const Eigen::Matrix3d& mat = m_tform.transformMatrix();

  double inCoords[2];
  inCoords[0] = inoutCoords[0];
  inCoords[1] = inoutCoords[1];
  inoutCoords[0] =
    mat(0, 0) * (inCoords[0] - m_centerX) + mat(0, 1) * (inCoords[1] - m_centerY) + mat(0, 2) + m_centerX;
  inoutCoords[1] =
    mat(1, 0) * (inCoords[0] - m_centerX) + mat(1, 1) * (inCoords[1] - m_centerY) + mat(1, 2) + m_centerY;
}

ZImageTransform* ZImageMatrix2DTransform::clone() const
{
  return new ZImageMatrix2DTransform(*this);
}

ZImageTransform* ZImageMatrix2DTransform::makeInverseTransform() const
{
  ZImageMatrix2DTransform* res = new ZImageMatrix2DTransform(*this);
  res->m_tform.invert();
  return res;
}

///////////////////////

ZImageYTranslation2DTransform::ZImageYTranslation2DTransform()
  : ZImageMatrix2DTransform()
{
  std::vector<double>(1, 0).swap(m_parameters);
}

size_t ZImageYTranslation2DTransform::numParameters() const
{
  return 1;
}

void ZImageYTranslation2DTransform::setParameters(const double* para)
{
  m_tform.reset();
  m_tform.setTranslation(0, para[0]);
  m_tform.makeMatrix();
  m_parameters = std::vector<double>(para, para + 1);
}

void ZImageYTranslation2DTransform::adaptParameters(size_t fromLevel, size_t toLevel)
{
  if (fromLevel == toLevel)
    return;
  double scale = std::pow(2.0, double(fromLevel) - double(toLevel));
  m_centerX *= scale;
  m_centerY *= scale;
  m_parameters[0] *= scale;
  setParameters(m_parameters.data());
}

void ZImageYTranslation2DTransform::transformPoint(double* inoutCoords) const
{
  CHECK(inoutCoords);
  const Eigen::Matrix3d& mat = m_tform.transformMatrix();

  inoutCoords[1] += mat(1, 2);
}

ZImageTransform* ZImageYTranslation2DTransform::clone() const
{
  return new ZImageYTranslation2DTransform(*this);
}

ZImageTransform* ZImageYTranslation2DTransform::makeInverseTransform() const
{
  ZImageYTranslation2DTransform* res = new ZImageYTranslation2DTransform(*this);
  res->m_tform.invert();
  return res;
}

///////////////////////

ZImageTranslation2DTransform::ZImageTranslation2DTransform()
  : ZImageMatrix2DTransform()
{
  std::vector<double>(2, 0).swap(m_parameters);
}

size_t ZImageTranslation2DTransform::numParameters() const
{
  return 2;
}

void ZImageTranslation2DTransform::setParameters(const double* para)
{
  m_tform.reset();
  m_tform.setTranslation(para[0], para[1]);
  m_tform.makeMatrix();
  m_parameters = std::vector<double>(para, para + 2);
}

void ZImageTranslation2DTransform::adaptParameters(size_t fromLevel, size_t toLevel)
{
  if (fromLevel == toLevel)
    return;
  double scale = std::pow(2.0, double(fromLevel) - double(toLevel));
  m_centerX *= scale;
  m_centerY *= scale;
  m_parameters[0] *= scale;
  m_parameters[1] *= scale;
  setParameters(m_parameters.data());
}

void ZImageTranslation2DTransform::transformPoint(double* inoutCoords) const
{
  CHECK(inoutCoords);
  const Eigen::Matrix3d& mat = m_tform.transformMatrix();

  inoutCoords[0] += mat(0, 2);
  inoutCoords[1] += mat(1, 2);
}

ZImageTransform* ZImageTranslation2DTransform::clone() const
{
  return new ZImageTranslation2DTransform(*this);
}

ZImageTransform* ZImageTranslation2DTransform::makeInverseTransform() const
{
  ZImageTranslation2DTransform* res = new ZImageTranslation2DTransform(*this);
  res->m_tform.invert();
  return res;
}

/////////////////////////

ZImageRigid2DTransform::ZImageRigid2DTransform()
  : ZImageMatrix2DTransform()
{
  std::vector<double>(3, 0).swap(m_parameters);
}

size_t ZImageRigid2DTransform::numParameters() const
{
  return 3;
}

void ZImageRigid2DTransform::setParameters(const double* para)
{
  m_tform.reset();
  m_tform.setTranslation(para[0], para[1]);
  m_tform.setRotationAngle(para[2]);
  m_tform.makeMatrix();
  m_parameters = std::vector<double>(para, para + 3);
}

void ZImageRigid2DTransform::adaptParameters(size_t fromLevel, size_t toLevel)
{
  if (fromLevel == toLevel)
    return;
  double scale = std::pow(2.0, double(fromLevel) - double(toLevel));
  m_centerX *= scale;
  m_centerY *= scale;
  m_parameters[0] *= scale;
  m_parameters[1] *= scale;
  setParameters(m_parameters.data());
}

std::vector<double> ZImageRigid2DTransform::estimateParameterScales(const double* dims) const
{
  std::vector<double> optimizerScales(numParameters(), 1.0);
  getAffineParameterScales(dims[0], dims[1], &optimizerScales[2]);
  return optimizerScales;
}

ZImageTransform* ZImageRigid2DTransform::clone() const
{
  return new ZImageRigid2DTransform(*this);
}

ZImageTransform* ZImageRigid2DTransform::makeInverseTransform() const
{
  ZImageRigid2DTransform* res = new ZImageRigid2DTransform(*this);
  res->m_tform.invert();
  return res;
}

/////////////////////////

ZImageSimilarity2DTransform::ZImageSimilarity2DTransform()
  : ZImageMatrix2DTransform()
{
  std::vector<double>(4, 0).swap(m_parameters);
  m_parameters[3] = 1;
}

size_t ZImageSimilarity2DTransform::numParameters() const
{
  return 4;
}

void ZImageSimilarity2DTransform::setParameters(const double* para)
{
  m_tform.reset();
  m_tform.setTranslation(para[0], para[1]);
  m_tform.setRotationAngle(para[2]);
  m_tform.setScale(para[3], para[3]);
  m_tform.makeMatrix();
  m_parameters = std::vector<double>(para, para + 4);
}

void ZImageSimilarity2DTransform::adaptParameters(size_t fromLevel, size_t toLevel)
{
  if (fromLevel == toLevel)
    return;
  double scale = std::pow(2.0, double(fromLevel) - double(toLevel));
  m_centerX *= scale;
  m_centerY *= scale;
  m_parameters[0] *= scale;
  m_parameters[1] *= scale;
  setParameters(m_parameters.data());
}

std::vector<double> ZImageSimilarity2DTransform::estimateParameterScales(const double* dims) const
{
  std::vector<double> optimizerScales(numParameters(), 1.0);
  getAffineParameterScales(dims[0], dims[1], &optimizerScales[2], &optimizerScales[3]);
  return optimizerScales;
}

ZImageTransform* ZImageSimilarity2DTransform::clone() const
{
  return new ZImageSimilarity2DTransform(*this);
}

ZImageTransform* ZImageSimilarity2DTransform::makeInverseTransform() const
{
  ZImageSimilarity2DTransform* res = new ZImageSimilarity2DTransform(*this);
  res->m_tform.invert();
  return res;
}

/////////////////////////

ZImageAffine2DTransform::ZImageAffine2DTransform()
  : ZImageMatrix2DTransform()
{
  std::vector<double>(7, 0).swap(m_parameters);
  m_parameters[3] = 1;
  m_parameters[4] = 1;
}

size_t ZImageAffine2DTransform::numParameters() const
{
  return 7;
}

void ZImageAffine2DTransform::setParameters(const double* para)
{
  m_tform.reset();
  m_tform.setTranslation(para[0], para[1]);
  m_tform.setRotationAngle(para[2]);
  m_tform.setScale(para[3], para[4]);
  m_tform.setShear(para[5], para[6]);
  m_tform.makeMatrix();
  m_parameters = std::vector<double>(para, para + 7);
}

void ZImageAffine2DTransform::adaptParameters(size_t fromLevel, size_t toLevel)
{
  if (fromLevel == toLevel)
    return;
  double scale = std::pow(2.0, double(fromLevel) - double(toLevel));
  m_centerX *= scale;
  m_centerY *= scale;
  m_parameters[0] *= scale;
  m_parameters[1] *= scale;
  setParameters(m_parameters.data());
}

std::vector<double> ZImageAffine2DTransform::estimateParameterScales(const double* dims) const
{
  std::vector<double> optimizerScales(numParameters(), 1.0);
  getAffineParameterScales(dims[0], dims[1], &optimizerScales[2], &optimizerScales[3], &optimizerScales[4],
                           &optimizerScales[5], &optimizerScales[6]);
  return optimizerScales;
}

ZImageTransform* ZImageAffine2DTransform::clone() const
{
  return new ZImageAffine2DTransform(*this);
}

ZImageTransform* ZImageAffine2DTransform::makeInverseTransform() const
{
  ZImageAffine2DTransform* res = new ZImageAffine2DTransform(*this);
  res->m_tform.invert();
  return res;
}

} // namespace nim
