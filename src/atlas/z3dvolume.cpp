#include "z3dvolume.h"

#include "z3dgpuinfo.h"
#include "z3dtexture.h"
#include "zlog.h"
#include <algorithm>

namespace nim {

Z3DVolume::Z3DVolume(ZImg& img,
                     const glm::vec3& spacing,
                     const glm::vec3& offset,
                     const glm::mat4& transformation,
                     QObject* parent)
  : QObject(parent)
  , m_histogramMaxValue(-1)
  , m_volColor(1.f, 1.f, 1.f)
{
  m_img.swap(img);
  m_dimensions = glm::uvec3(m_img.width(), m_img.height(), m_img.depth());
  m_detailVolumeDimensions = glm::round(glm::vec3(m_dimensions) * spacing);
  m_parentVolumeDimensions = m_detailVolumeDimensions;
  m_parentVolumeOffset = offset;
  setSpacing(spacing);
  setOffset(offset);
  setPhysicalToWorldMatrix(transformation);
  m_img.computeMinMax(m_minValue, m_maxValue);
}

Z3DVolume::~Z3DVolume()
{
  if (m_histogramThread) {
    if (m_histogramThread->isRunning()) {
      m_histogramThread->wait();
    }
  }
}

int Z3DVolume::bitsStored() const
{
  if (m_img.isType<uint8_t>()) {
    return m_img.validBitCount() ? m_img.validBitCount() : 8;
  } else if (m_img.isType<uint16_t>()) {
    return m_img.validBitCount() ? m_img.validBitCount() : 16;
  } else if (m_img.isType<float>()) {
    return 32;
  }
  return 0;
}

size_t Z3DVolume::numVoxels() const
{
  return m_img.channelVoxelNumber();
}

QString Z3DVolume::samplerType() const
{
  if (m_dimensions.z > 1) {
    return "sampler3D";
  }
  if (m_dimensions.y > 1 && m_dimensions.x > 1) {
    return "sampler2D";
  }

  return "sampler1D";
}

double Z3DVolume::floatMinValue() const
{
  if (bitsStored() <= 16) {
    return m_minValue / ((1 << bitsStored()) - 1);
  }

  return minValue(); // already float image
}

double Z3DVolume::floatMaxValue() const
{
  if (bitsStored() <= 16) {
    return m_maxValue / ((1 << bitsStored()) - 1);
  }

  return maxValue(); // already float image
}

double Z3DVolume::value(int x, int y, int z) const
{
  return m_img.value<double>(x, y, z);
}

double Z3DVolume::value(size_t index) const
{
  return m_img.value<double>(index);
}

void Z3DVolume::asyncGenerateHistogram()
{
  if (hasHistogram() || m_histogramThread) {
    return;
  }
  m_histogramThread.reset(new Z3DVolumeHistogramThread(this));
  connect(m_histogramThread.get(), &Z3DVolumeHistogramThread::finished, this, &Z3DVolume::setHistogram);
  m_histogramThread->start();
}

size_t Z3DVolume::histogramBinCount() const
{
  if (m_img.isType<uint8_t>()) {
    return 256;
  }
  return m_histogram.size();
}

size_t Z3DVolume::histogramValue(size_t index) const
{
  if (index < m_histogram.size()) {
    return m_histogram[index];
  } else {
    return 0;
  }
}

size_t Z3DVolume::histogramValue(double fraction) const
{
  size_t index = static_cast<size_t>(fraction * static_cast<double>(histogramBinCount() - 1));
  return histogramValue(index);
}

double Z3DVolume::normalizedHistogramValue(size_t index) const
{
  return static_cast<double>(histogramValue(index)) / m_histogramMaxValue;
}

double Z3DVolume::normalizedHistogramValue(double fraction) const
{
  size_t index = static_cast<size_t>(fraction * static_cast<double>(histogramBinCount() - 1));
  return normalizedHistogramValue(index);
}

double Z3DVolume::logNormalizedHistogramValue(size_t index) const
{
  return std::log(static_cast<double>(histogramValue(index)) + 1.0) / std::log(m_histogramMaxValue + 1.0);
}

double Z3DVolume::logNormalizedHistogramValue(double fraction) const
{
  size_t index = static_cast<size_t>(fraction * static_cast<double>(histogramBinCount() - 1));
  return logNormalizedHistogramValue(index);
}

Z3DTexture* Z3DVolume::texture() const
{
  if (!m_texture) {
    generateTexture();
  }
  return m_texture.get();
}

ZBBox<glm::dvec3> Z3DVolume::worldBoundBox() const
{
  if (m_hasTransformMatrix) {
    ZBBox<glm::dvec3> res;
    res.expand(glm::dvec3(worldLUF()));
    res.expand(glm::dvec3(worldLDB()));
    res.expand(glm::dvec3(worldLDF()));
    res.expand(glm::dvec3(worldLUB()));
    res.expand(glm::dvec3(worldRUF()));
    res.expand(glm::dvec3(worldRDB()));
    res.expand(glm::dvec3(worldRDF()));
    res.expand(glm::dvec3(worldRUB()));

    return res;
  } else {
    return physicalBoundBox();
  }
}

void Z3DVolume::setPhysicalToWorldMatrix(const glm::mat4& transformationMatrix)
{
  m_transformationMatrix = transformationMatrix;
  if (m_transformationMatrix != glm::mat4(1.0)) {
    m_hasTransformMatrix = true;
  }
}

glm::mat4 Z3DVolume::voxelToWorldMatrix() const
{
  return physicalToWorldMatrix() * voxelToPhysicalMatrix();
}

glm::mat4 Z3DVolume::worldToVoxelMatrix() const
{
  return glm::inverse(voxelToWorldMatrix());
}

glm::mat4 Z3DVolume::worldToTextureMatrix() const
{
  return glm::inverse(textureToWorldMatrix());
}

glm::mat4 Z3DVolume::textureToWorldMatrix() const
{
  return voxelToWorldMatrix() * textureToVoxelMatrix();
}

glm::mat4 Z3DVolume::voxelToPhysicalMatrix() const
{
  // 1. Multiply by spacing 2. Apply offset
  glm::mat4 scale = glm::scale(glm::mat4(1.0), spacing());
  return glm::translate(scale, offset());
}

glm::mat4 Z3DVolume::physicalToVoxelMatrix() const
{
  glm::mat4 translate = glm::translate(glm::mat4(1.0), -offset());
  return glm::scale(translate, 1.f / spacing());
}

glm::mat4 Z3DVolume::worldToPhysicalMatrix() const
{
  return glm::inverse(physicalToWorldMatrix());
}

glm::mat4 Z3DVolume::textureToPhysicalMatrix() const
{
  return voxelToPhysicalMatrix() * textureToVoxelMatrix();
}

glm::mat4 Z3DVolume::physicalToTextureMatrix() const
{
  return voxelToTextureMatrix() * physicalToVoxelMatrix();
}

glm::mat4 Z3DVolume::textureToVoxelMatrix() const
{
  return glm::scale(glm::mat4(1.0), glm::vec3(dimensions()));
}

glm::mat4 Z3DVolume::voxelToTextureMatrix() const
{
  return glm::scale(glm::mat4(1.0), 1.0f / glm::vec3(dimensions()));
}

void Z3DVolume::setHistogram()
{
  m_histogram.swap(m_histogramThread->histogram());
  computeHistogramMaxValue();
  Q_EMIT histogramFinished();
}

void Z3DVolume::generateTexture() const
{
  if (dimensions().x == 0 || dimensions().y == 0 || dimensions().z == 0) {
    LOG(ERROR) << fmt::format(
      "OpenGL volumes must have a size greater than 0 in all dimensions. Actual size: ({}, {}, {})",
      m_img.width(),
      m_img.height(),
      m_img.depth());
    return;
  }

  GLenum format;
  GLint internalFormat;
  GLenum dataType;
  if (m_img.isType<uint8_t>()) {
    format = GL_RED;
    internalFormat = GLint(GL_R8);
    dataType = GL_UNSIGNED_BYTE;
  } else if (m_img.isType<uint16_t>()) {
    format = GL_RED;
    internalFormat = GLint(GL_R16);
    dataType = GL_UNSIGNED_SHORT;
  } else if (m_img.isType<float>()) {
    format = GL_RED;
    internalFormat = GLint(GL_R32F);
    dataType = GL_FLOAT;
  } else {
    LOG(ERROR) << "Only GREY, GREY16 or FLOAT32 stack formats are supported";
    return;
  }

  // Create texture
  //  m_texture = std::make_unique<Z3DTexture>(internalFormat,
  //                                           dimensions(),
  //                                           format,
  //                                           dataType,
  //                                           m_img.channelData(0),
  //                                           GLint(GL_LINEAR),
  //                                           GLint(GL_LINEAR),
  //                                           GLint(GL_CLAMP_TO_BORDER));
  m_texture = std::make_unique<Z3DTexture>(internalFormat, dimensions(), format, dataType, m_img.channelData(0));

  CHECK_GL_ERROR
}

void Z3DVolume::computeHistogramMaxValue()
{
  m_histogramMaxValue = *std::ranges::max_element(m_histogram);
}

//-----------------------------------------------------------------------------------
Z3DVolumeHistogramThread::Z3DVolumeHistogramThread(Z3DVolume* volume, QObject* parent)
  : QThread(parent)
  , m_volume(volume)
{}

void Z3DVolumeHistogramThread::run()
{
  m_histogram.assign(static_cast<size_t>(m_volume->maxValue() + 1), 0);
  for (size_t i = 0; i < m_volume->numVoxels(); ++i) {
    m_histogram[static_cast<size_t>(m_volume->value(i))]++;
  }
}

void Z3DVolume::translate(double dx, double dy, double dz)
{
  m_transformationMatrix[3][0] += dx;
  m_transformationMatrix[3][1] += dy;
  m_transformationMatrix[3][2] += dz;

  if (m_transformationMatrix != glm::mat4(1.0)) {
    m_hasTransformMatrix = true;
  }
}

} // namespace nim
