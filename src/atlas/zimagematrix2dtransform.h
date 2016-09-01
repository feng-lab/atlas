#pragma once

#include "zaffine2d.h"
#include "zimagetransform.h"

namespace nim {

class ZImageMatrix2DTransform : public ZImageTransform
{
public:
  ZImageMatrix2DTransform();

  void setTransform(const ZAffine2D& tform)
  { m_tform = tform; }

  void setRotationCenter(double x, double y)
  {
    m_centerX = x;
    m_centerY = y;
  }

  void transformRange(double inXMin, double inXMax, double inYMin, double inYMax,
                      double& outXMin, double& outXMax, double& outYMin, double& outYMax) const;

  void transformPointInverse(double* inoutCoords) const;

  // ZImageTransform interface
public:
  virtual size_t numParameters() const override;

  using ZImageTransform::setParameters;

  virtual void setParameters(double const* para) override;

  virtual bool is2DTransform() const override
  { return true; }

  virtual void adaptParameters(size_t fromLevel, size_t toLevel) override;

  virtual void transformPoint(double* inoutCoords) const override;

  virtual QString toQString() const override
  { return m_tform.toQString(); }

  virtual ZImageTransform* clone() const override;

  virtual ZImageTransform* makeInverseTransform() const override;

protected:
  ZAffine2D m_tform;
  double m_centerX;
  double m_centerY;
};

class ZImageYTranslation2DTransform : public ZImageMatrix2DTransform
{
public:
  ZImageYTranslation2DTransform();

  // ZImageTransform interface
public:
  virtual size_t numParameters() const override;

  using ZImageTransform::setParameters;

  virtual void setParameters(double const* para) override;

  virtual void adaptParameters(size_t fromLevel, size_t toLevel) override;

  virtual void transformPoint(double* inoutCoords) const override;

  virtual ZImageTransform* clone() const override;

  virtual ZImageTransform* makeInverseTransform() const override;
};

class ZImageTranslation2DTransform : public ZImageMatrix2DTransform
{
public:
  ZImageTranslation2DTransform();

  // ZImageTransform interface
public:
  virtual size_t numParameters() const override;

  using ZImageTransform::setParameters;

  virtual void setParameters(double const* para) override;

  virtual void adaptParameters(size_t fromLevel, size_t toLevel) override;

  virtual void transformPoint(double* inoutCoords) const override;

  virtual ZImageTransform* clone() const override;

  virtual ZImageTransform* makeInverseTransform() const override;
};

class ZImageRigid2DTransform : public ZImageMatrix2DTransform
{
public:
  ZImageRigid2DTransform();

  // ZImageTransform interface
public:
  virtual size_t numParameters() const override;

  using ZImageTransform::setParameters;

  virtual void setParameters(double const* para) override;

  virtual void adaptParameters(size_t fromLevel, size_t toLevel) override;

  virtual std::vector<double> estimateParameterScales(const double* dims) const override;

  virtual ZImageTransform* clone() const override;

  virtual ZImageTransform* makeInverseTransform() const override;
};

class ZImageSimilarity2DTransform : public ZImageMatrix2DTransform
{
public:
  ZImageSimilarity2DTransform();

  // ZImageTransform interface
public:
  virtual size_t numParameters() const override;

  using ZImageTransform::setParameters;

  virtual void setParameters(double const* para) override;

  virtual void adaptParameters(size_t fromLevel, size_t toLevel) override;

  virtual std::vector<double> estimateParameterScales(const double* dims) const override;

  virtual ZImageTransform* clone() const override;

  virtual ZImageTransform* makeInverseTransform() const override;
};

class ZImageAffine2DTransform : public ZImageMatrix2DTransform
{
public:
  ZImageAffine2DTransform();

  // ZImageTransform interface
public:
  virtual size_t numParameters() const override;

  using ZImageTransform::setParameters;

  virtual void setParameters(double const* para) override;

  virtual void adaptParameters(size_t fromLevel, size_t toLevel) override;

  virtual std::vector<double> estimateParameterScales(const double* dims) const override;

  virtual ZImageTransform* clone() const override;

  virtual ZImageTransform* makeInverseTransform() const override;
};

//((scale-1)/2) in output image maps to 0 in input image, and ((3*scale-1)/2) in output
//image maps to 1 in input image.
template<typename TPixel>
void image2DResize_Old(const TPixel* img, size_t width, size_t height,
                       TPixel* imgOut, size_t outWidth, size_t outHeight,
                       Interpolant interpolant = Interpolant::Cubic)
{
  ZImageMatrix2DTransform tfm;
  tfm.setImageInterpolation(ZImageInterpolation(interpolant, PadOption::Replicate));
  ZAffine2D tform(width * 1.0 / outWidth, 0, 0.5 * (width * 1.0 / outWidth - 1.0),
                  0, height * 1.0 / outHeight, 0.5 * (height * 1.0 / outHeight - 1.0));
  tfm.setTransform(tform);
  tfm.transformImage(img, width, height, imgOut, 0, outWidth, 0, outHeight);
}

} // namespace nim

