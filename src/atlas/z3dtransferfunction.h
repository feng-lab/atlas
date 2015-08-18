#ifndef Z3DTRANSFERFUNCTION_H
#define Z3DTRANSFERFUNCTION_H

#include "z3dgl.h"
#include <QObject>
#include <vector>
#include "zcolormap.h"
#include "zparameter.h"

namespace nim {

class Z3DVolume;
class Z3DTexture;

// only support 1d transfer function now
class Z3DTransferFunction : public ZColorMap
{
  Q_OBJECT
public:

  Z3DTransferFunction(double min = 0.0, double max = 1.0, const glm::col4 &minColor = glm::col4(0,0,0,0),
                      const glm::col4 &maxColor = glm::col4(255,255,255,255),
                      int width = 256,
                      QObject *parent = 0);


  Z3DTransferFunction(const Z3DTransferFunction &tf);
  Z3DTransferFunction(Z3DTransferFunction &&tf);
  virtual ~Z3DTransferFunction();

  void swap(Z3DTransferFunction& other) noexcept;

  Z3DTransferFunction& operator=(Z3DTransferFunction other) { swap(other); return *this; }

  bool operator==(const Z3DTransferFunction& tf) const;
  bool operator!=(const Z3DTransferFunction& tf) const;

  void resetToDefault();

  QString samplerType() const;

  inline glm::ivec3 textureDimensions() const { return m_dimensions; }

  // Returns the texture of the transfer function.
  Z3DTexture* texture();

  void resize(int width);

  void updateTexture();

  // domain should be in [0.0, 1.0] range
  virtual bool isValidDomainMin(double min) const override;
  virtual bool isValidDomainMax(double max) const override;

signals:

protected:
  void createTexture();

  glm::ivec3 m_dimensions;
  GLenum m_textureFormat;
  GLenum m_textureDataType;

private:
  // Adapts the given width and height of transfer function to graphics board capabilities.
  void fitDimensions(int& width, int& height, int& depth) const;
};

class Z3DTransferFunctionParameter : public ZSingleValueParameter<Z3DTransferFunction>
{
  Q_OBJECT
public:
  Z3DTransferFunctionParameter(const QString& name, QObject *parent = NULL);
  Z3DTransferFunctionParameter(const QString& name, double min, double max, const glm::col4 &minColor,
                               const glm::col4 &maxColor, int width, QObject *parent = NULL);

  void setVolume(Z3DVolume* volume);

  inline Z3DVolume* volume() const { return m_volume; }

protected:
  virtual QWidget* actualCreateWidget(QWidget *parent) override;

  Z3DVolume* m_volume;

  // ZParameter interface
public:
  virtual void setSameAs(const ZParameter &rhs) override;
  virtual bool supportInterpolation() const override { return false; }
  virtual QJsonValue jsonValue() const override;
  virtual void readValue(const QJsonValue &jsonValue) override;
};

} // namespace nim

#endif // Z3DTRANSFERFUNCTION_H
