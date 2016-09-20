#include "zimagematrix3dtransform.h"

namespace nim {

ZImageMatrix3DTransform::ZImageMatrix3DTransform()
{
  std::vector<double>(12, 0).swap(m_parameters);
  m_parameters[0] = 1;
  m_parameters[5] = 1;
  m_parameters[10] = 1;
}

void ZImageMatrix3DTransform::transformRange(double inXMin, double inXMax, double inYMin, double inYMax, double inZMin,
                                             double inZMax,
                                             double& outXMin, double& outXMax, double& outYMin, double& outYMax,
                                             double& outZMin, double& outZMax) const
{
  double outCoords[24];
  outCoords[0] = outCoords[3] = outCoords[6] = outCoords[9] = inXMin;
  outCoords[12] = outCoords[15] = outCoords[18] = outCoords[21] = inXMax;
  outCoords[1] = outCoords[4] = outCoords[13] = outCoords[16] = inYMin;
  outCoords[7] = outCoords[10] = outCoords[19] = outCoords[22] = inYMax;
  outCoords[2] = outCoords[8] = outCoords[14] = outCoords[20] = inZMin;
  outCoords[5] = outCoords[11] = outCoords[17] = outCoords[23] = inZMax;

  transformPointInverse(outCoords);
  outXMin = outXMax = outCoords[0];
  outYMin = outYMax = outCoords[1];
  outZMin = outZMax = outCoords[2];
  for (size_t i = 3; i < 24; i += 3) {
    transformPointInverse(outCoords + i);
    outXMin = std::min(outXMin, outCoords[i]);
    outXMax = std::max(outXMax, outCoords[i]);
    outYMin = std::min(outYMin, outCoords[i + 1]);
    outYMax = std::max(outYMax, outCoords[i + 1]);
    outZMin = std::min(outZMin, outCoords[i + 2]);
    outZMax = std::max(outZMax, outCoords[i + 2]);
  }
}

void ZImageMatrix3DTransform::transformPointInverse(double* inoutCoords) const
{
  CHECK(inoutCoords);
  const Eigen::Matrix4d& mat = m_tform.inverseTransformMatrix();

  double inCoords[3];
  inCoords[0] = inoutCoords[0];
  inCoords[1] = inoutCoords[1];
  inCoords[2] = inoutCoords[2];
  inoutCoords[0] = mat(0, 0) * (inCoords[0] - m_centerX) + mat(0, 1) * (inCoords[1] - m_centerY) +
                   mat(0, 2) * (inCoords[2] - m_centerZ) + mat(0, 3) + m_centerX;
  inoutCoords[1] = mat(1, 0) * (inCoords[0] - m_centerX) + mat(1, 1) * (inCoords[1] - m_centerY) +
                   mat(1, 2) * (inCoords[2] - m_centerZ) + mat(1, 3) + m_centerY;
  inoutCoords[2] = mat(2, 0) * (inCoords[0] - m_centerX) + mat(2, 1) * (inCoords[1] - m_centerY) +
                   mat(2, 2) * (inCoords[2] - m_centerZ) + mat(2, 3) + m_centerZ;
}

size_t ZImageMatrix3DTransform::numParameters() const
{
  return 12;
}

void ZImageMatrix3DTransform::setParameters(const double* para)
{
  m_tform.reset();
  m_tform.setMatrix(para[0], para[1], para[2], para[3],
                    para[4], para[5], para[6], para[7],
                    para[8], para[9], para[10], para[11]);
  m_parameters = std::vector<double>(para, para + 12);
}

void ZImageMatrix3DTransform::adaptParameters(size_t fromLevel, size_t toLevel)
{
  if (fromLevel == toLevel)
    return;
  double scale = std::pow(2.0, double(fromLevel) - double(toLevel));
  m_centerX *= scale;
  m_centerY *= scale;
  m_centerZ *= scale;
  m_parameters[3] *= scale;
  m_parameters[7] *= scale;
  m_parameters[11] *= scale;
  setParameters(m_parameters.data());
}

void ZImageMatrix3DTransform::transformPoint(double* inoutCoords) const
{
  CHECK(inoutCoords);
  const Eigen::Matrix4d& mat = m_tform.transformMatrix();

  double inCoords[3];
  inCoords[0] = inoutCoords[0];
  inCoords[1] = inoutCoords[1];
  inCoords[2] = inoutCoords[2];
  inoutCoords[0] = mat(0, 0) * (inCoords[0] - m_centerX) + mat(0, 1) * (inCoords[1] - m_centerY) +
                   mat(0, 2) * (inCoords[2] - m_centerZ) + mat(0, 3) + m_centerX;
  inoutCoords[1] = mat(1, 0) * (inCoords[0] - m_centerX) + mat(1, 1) * (inCoords[1] - m_centerY) +
                   mat(1, 2) * (inCoords[2] - m_centerZ) + mat(1, 3) + m_centerY;
  inoutCoords[2] = mat(2, 0) * (inCoords[0] - m_centerX) + mat(2, 1) * (inCoords[1] - m_centerY) +
                   mat(2, 2) * (inCoords[2] - m_centerZ) + mat(2, 3) + m_centerZ;
}

ZImageTransform* ZImageMatrix3DTransform::clone() const
{
  return new ZImageMatrix3DTransform(*this);
}

ZImageTransform* ZImageMatrix3DTransform::makeInverseTransform() const
{
  auto res = new ZImageMatrix3DTransform(*this);
  res->m_tform.invert();
  return res;
}

///////////////////////

ZImageTranslation3DTransform::ZImageTranslation3DTransform()
{
  std::vector<double>(3, 0).swap(m_parameters);
}

size_t ZImageTranslation3DTransform::numParameters() const
{
  return 3;
}

void ZImageTranslation3DTransform::setParameters(const double* para)
{
  m_tform.reset();
  m_tform.setTranslation(para[0], para[1], para[2]);
  m_tform.makeMatrix();
  m_parameters = std::vector<double>(para, para + 3);
}

void ZImageTranslation3DTransform::adaptParameters(size_t fromLevel, size_t toLevel)
{
  if (fromLevel == toLevel)
    return;
  double scale = std::pow(2.0, double(fromLevel) - double(toLevel));
  m_centerX *= scale;
  m_centerY *= scale;
  m_centerZ *= scale;
  m_parameters[0] *= scale;
  m_parameters[1] *= scale;
  m_parameters[2] *= scale;
  setParameters(m_parameters.data());
}

void ZImageTranslation3DTransform::transformPoint(double* inoutCoords) const
{
  CHECK(inoutCoords);
  const Eigen::Matrix4d& mat = m_tform.transformMatrix();

  inoutCoords[0] += mat(0, 3);
  inoutCoords[1] += mat(1, 3);
  inoutCoords[2] += mat(2, 3);
}

ZImageTransform* ZImageTranslation3DTransform::clone() const
{
  return new ZImageTranslation3DTransform(*this);
}

ZImageTransform* ZImageTranslation3DTransform::makeInverseTransform() const
{
  auto res = new ZImageTranslation3DTransform(*this);
  res->m_tform.invert();
  return res;
}

/////////////////////////

ZImageRigid3DTransform::ZImageRigid3DTransform()
{
  std::vector<double>(6, 0).swap(m_parameters);
}

size_t ZImageRigid3DTransform::numParameters() const
{
  return 6;
}

void ZImageRigid3DTransform::setParameters(const double* para)
{
  m_tform.reset();
  m_tform.setTranslation(para[0], para[1], para[2]);
  m_tform.setRotationAngle(para[3], para[4], para[5]);
  m_tform.makeMatrix();
  m_parameters = std::vector<double>(para, para + 6);
}

void ZImageRigid3DTransform::adaptParameters(size_t fromLevel, size_t toLevel)
{
  if (fromLevel == toLevel)
    return;
  double scale = std::pow(2.0, double(fromLevel) - double(toLevel));
  m_centerX *= scale;
  m_centerY *= scale;
  m_centerZ *= scale;
  m_parameters[0] *= scale;
  m_parameters[1] *= scale;
  m_parameters[2] *= scale;
  setParameters(m_parameters.data());
}

ZImageTransform* ZImageRigid3DTransform::clone() const
{
  return new ZImageRigid3DTransform(*this);
}

ZImageTransform* ZImageRigid3DTransform::makeInverseTransform() const
{
  auto res = new ZImageRigid3DTransform(*this);
  res->m_tform.invert();
  return res;
}

/////////////////////////

ZImageSimilarity3DTransform::ZImageSimilarity3DTransform()
{
  std::vector<double>(7, 0).swap(m_parameters);
  m_parameters[6] = 1;
}

size_t ZImageSimilarity3DTransform::numParameters() const
{
  return 7;
}

void ZImageSimilarity3DTransform::setParameters(const double* para)
{
  m_tform.reset();
  m_tform.setTranslation(para[0], para[1], para[2]);
  m_tform.setRotationAngle(para[3], para[4], para[5]);
  m_tform.setScale(para[6], para[6], para[6]);
  m_tform.makeMatrix();
  m_parameters = std::vector<double>(para, para + 7);
}

void ZImageSimilarity3DTransform::adaptParameters(size_t fromLevel, size_t toLevel)
{
  if (fromLevel == toLevel)
    return;
  double scale = std::pow(2.0, double(fromLevel) - double(toLevel));
  m_centerX *= scale;
  m_centerY *= scale;
  m_centerZ *= scale;
  m_parameters[0] *= scale;
  m_parameters[1] *= scale;
  m_parameters[2] *= scale;
  setParameters(m_parameters.data());
}

ZImageTransform* ZImageSimilarity3DTransform::clone() const
{
  return new ZImageSimilarity3DTransform(*this);
}

ZImageTransform* ZImageSimilarity3DTransform::makeInverseTransform() const
{
  auto res = new ZImageSimilarity3DTransform(*this);
  res->m_tform.invert();
  return res;
}

/////////////////////////

ZImageAffine3DTransform::ZImageAffine3DTransform()
{
  std::vector<double>(15, 0).swap(m_parameters);
  m_parameters[6] = 1;
  m_parameters[7] = 1;
  m_parameters[8] = 1;
}

size_t ZImageAffine3DTransform::numParameters() const
{
  return 15;
}

void ZImageAffine3DTransform::setParameters(const double* para)
{
  m_tform.reset();
  m_tform.setTranslation(para[0], para[1], para[2]);
  m_tform.setRotationAngle(para[3], para[4], para[5]);
  m_tform.setScale(para[6], para[7], para[8]);
  m_tform.setShear(para[9], para[10], para[11], para[12], para[13], para[14]);
  m_tform.makeMatrix();
  m_parameters = std::vector<double>(para, para + 15);
}

void ZImageAffine3DTransform::adaptParameters(size_t fromLevel, size_t toLevel)
{
  if (fromLevel == toLevel)
    return;
  double scale = std::pow(2.0, double(fromLevel) - double(toLevel));
  m_centerX *= scale;
  m_centerY *= scale;
  m_centerZ *= scale;
  m_parameters[0] *= scale;
  m_parameters[1] *= scale;
  m_parameters[2] *= scale;
  setParameters(m_parameters.data());
}

ZImageTransform* ZImageAffine3DTransform::clone() const
{
  return new ZImageAffine3DTransform(*this);
}

ZImageTransform* ZImageAffine3DTransform::makeInverseTransform() const
{
  auto res = new ZImageAffine3DTransform(*this);
  res->m_tform.invert();
  return res;
}

} // namespace nim
