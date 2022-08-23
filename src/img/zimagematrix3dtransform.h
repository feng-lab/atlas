#pragma once

#include "zaffine3d.h"
#include "zimagetransform.h"

namespace nim {

class ZImageMatrix3DTransform : public ZImageTransform
{
public:
  ZImageMatrix3DTransform();

  void setTransform(const ZAffine3D& tform)
  {
    m_tform = tform;
  }

  void setRotationCenter(double x, double y, double z)
  {
    m_centerX = x;
    m_centerY = y;
    m_centerZ = z;
  }

  void transformRange(double inXMin,
                      double inXMax,
                      double inYMin,
                      double inYMax,
                      double inZMin,
                      double inZMax,
                      double& outXMin,
                      double& outXMax,
                      double& outYMin,
                      double& outYMax,
                      double& outZMin,
                      double& outZMax) const;

  void transformPointInverse(double* inoutCoords) const;

  // ZImageTransform interface

public:
  size_t numParameters() const override;

  using ZImageTransform::setParameters;

  void setParameters(double const* para) override;

  bool is2DTransform() const override
  {
    return false;
  }

  void adaptParameters(size_t fromLevel, size_t toLevel) override;

  void transformPoint(double* inoutCoords) const override;

  inline QString toQString() const override
  {
    return m_tform.toQString();
  }

  ZImageTransform* clone() const override;

  ZImageTransform* makeInverseTransform() const override;

protected:
  ZAffine3D m_tform;
  double m_centerX = 0;
  double m_centerY = 0;
  double m_centerZ = 0;
};

class ZImageTranslation3DTransform : public ZImageMatrix3DTransform
{
public:
  ZImageTranslation3DTransform();

  // ZImageTransform interface

public:
  size_t numParameters() const override;

  using ZImageTransform::setParameters;

  void setParameters(double const* para) override;

  void adaptParameters(size_t fromLevel, size_t toLevel) override;

  void transformPoint(double* inoutCoords) const override;

  ZImageTransform* clone() const override;

  ZImageTransform* makeInverseTransform() const override;
};

class ZImageRigid3DTransform : public ZImageMatrix3DTransform
{
public:
  ZImageRigid3DTransform();

  // ZImageTransform interface

public:
  size_t numParameters() const override;

  using ZImageTransform::setParameters;

  void setParameters(double const* para) override;

  void adaptParameters(size_t fromLevel, size_t toLevel) override;

  ZImageTransform* clone() const override;

  ZImageTransform* makeInverseTransform() const override;
};

class ZImageSimilarity3DTransform : public ZImageMatrix3DTransform
{
public:
  ZImageSimilarity3DTransform();

  // ZImageTransform interface

public:
  size_t numParameters() const override;

  using ZImageTransform::setParameters;

  void setParameters(double const* para) override;

  void adaptParameters(size_t fromLevel, size_t toLevel) override;

  ZImageTransform* clone() const override;

  ZImageTransform* makeInverseTransform() const override;
};

class ZImageAffine3DTransform : public ZImageMatrix3DTransform
{
public:
  ZImageAffine3DTransform();

  // ZImageTransform interface

public:
  size_t numParameters() const override;

  using ZImageTransform::setParameters;

  void setParameters(double const* para) override;

  void adaptParameters(size_t fromLevel, size_t toLevel) override;

  ZImageTransform* clone() const override;

  ZImageTransform* makeInverseTransform() const override;
};

//((scale-1)/2) in output image maps to 0 in input image, and ((3*scale-1)/2) in output
// image maps to 1 in input image.
template<typename TPixel>
void image3DResize_Old(const TPixel* img,
                       size_t width,
                       size_t height,
                       size_t depth,
                       TPixel* imgOut,
                       size_t outWidth,
                       size_t outHeight,
                       size_t outDepth,
                       Interpolant interpolant = Interpolant::Cubic)
{
  ZImageMatrix3DTransform tfm;
  tfm.setImageInterpolation(ZImageInterpolation(interpolant, PadOption::Replicate));
  ZAffine3D tform(width * 1.0 / outWidth,
                  0,
                  0,
                  0.5 * (width * 1.0 / outWidth - 1.0),
                  0,
                  height * 1.0 / outHeight,
                  0,
                  0.5 * (height * 1.0 / outHeight - 1.0),
                  0,
                  0,
                  depth * 1.0 / outDepth,
                  0.5 * (depth * 1.0 / outDepth - 1.0));
  tfm.setTransform(tform);
  tfm.transformImage(img, width, height, depth, imgOut, 0, outWidth, 0, outHeight, 0, outDepth);
}

} // namespace nim
