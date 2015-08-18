#include "z3dgl.h"
#include "z3drenderprocessor.h"

#include "z3dshaderprogram.h"
#include "z3dcameraparameter.h"
#include "QsLog.h"
#include "z3dgpuinfo.h"
#include <QImageWriter>
#include <memory>
#include "zimg.h"

namespace nim {

Z3DRenderProcessor::Z3DRenderProcessor(Z3DGlobalParameters &globalPara, QObject *parent)
  : Z3DBoundedFilter(globalPara, parent)
  , m_hardwareSupportVAO(Z3DGpuInfoInstance.isVAOSupported())
  , m_privateVAO(1)
{
}

void Z3DRenderProcessor::updateSize()
{
  // 1. update outport size
  bool resized = false;

  const std::vector<Z3DOutputPortBase*> outports = outputPorts();
  glm::ivec2 maxOutportSize(-1, -1);
  for(size_t i=0; i<outports.size(); ++i) {
    Z3DRenderOutputPort* rp = dynamic_cast<Z3DRenderOutputPort*>(outports[i]);
    if (rp) {
      glm::ivec2 outportSize = rp->expectedSize();
      if (outportSize.x > 0 && outportSize != rp->size()) {
        resized = true;
        rp->resize(outportSize);
      }

      maxOutportSize = glm::max(maxOutportSize, rp->size());
    }
  }

  // 2. update private ports
  const std::vector<Z3DRenderOutputPort*> privatePorts = privateRenderPorts();
  for (size_t i=0; i<privatePorts.size(); ++i) {
    privatePorts[i]->resize(maxOutportSize);
  }

  // 3. update inport expected size
  const std::vector<Z3DInputPortBase*> inports = inputPorts();
  for (size_t i=0; i<inports.size(); i++) {
    Z3DRenderInputPort *renderInport = dynamic_cast< Z3DRenderInputPort* >(inports[i]);
    if (renderInport)
      renderInport->setExpectedSize(maxOutportSize);
  }

  invalidate();
}

void Z3DRenderProcessor::addPrivateRenderPort(Z3DRenderOutputPort* port)
{
  port->setProcessor(this);
  m_privateRenderPorts.push_back(port);

  std::map<QString, Z3DOutputPortBase*>::const_iterator it = m_outputPortMap.find(port->name());
  if (it == m_outputPortMap.end())
    m_outputPortMap.emplace(port->name(), port);
  else {
    LERROR() << className() << "port" << port->name() << "has already been inserted!";
    assert(false);
  }
}

void Z3DRenderProcessor::addPrivateRenderPort(Z3DRenderOutputPort& port)
{
  addPrivateRenderPort(&port);
}

void Z3DRenderProcessor::renderScreenQuad(const Z3DShaderProgram &shader)
{
  if (!shader.isLinked())
    return;

  glDepthFunc(GL_ALWAYS);

  m_privateVAO.bind();

  GLfloat vertices[] = {-1.f, 1.f, 0.f, //top left corner
                        -1.f, -1.f, 0.f, //bottom left corner
                        1.f, 1.f, 0.f, //top right corner
                        1.f, -1.f, 0.f}; // bottom right rocner
  GLint attr_vertex = shader.vertexAttributeLocation();

  GLuint bufObjects[1];
  glGenBuffers(1, bufObjects);

  glEnableVertexAttribArray(attr_vertex);
  glBindBuffer(GL_ARRAY_BUFFER, bufObjects[0]);
  glBufferData(GL_ARRAY_BUFFER, 3*4*sizeof(GLfloat), vertices, GL_STATIC_DRAW);
  glVertexAttribPointer(attr_vertex, 3, GL_FLOAT, GL_FALSE, 0, 0);

  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

  glBindBuffer(GL_ARRAY_BUFFER, 0);
  glDeleteBuffers(1, bufObjects);

  glDisableVertexAttribArray(attr_vertex);

  m_privateVAO.release();

  glDepthFunc(GL_LESS);
}

const std::vector<Z3DRenderOutputPort*> &Z3DRenderProcessor::privateRenderPorts() const
{
  return m_privateRenderPorts;
}

void Z3DRenderProcessor::saveTextureAsImage(const Z3DTexture *tex, const QString &filename)
{
  try {
    GLenum dataFormat = GL_BGRA;
    GLenum dataType = GL_UNSIGNED_INT_8_8_8_8_REV;
    std::unique_ptr<uint8_t[]> colorBuffer(new uint8_t[tex->bypePerPixel(dataFormat, dataType) * tex->numPixels()]);
    tex->downloadTextureToBuffer(dataFormat, dataType, colorBuffer.get());
    QImage upsideDownImage(colorBuffer.get(), tex->width(), tex->height(),
                           QImage::Format_ARGB32);
    QImage image = upsideDownImage.mirrored(false, true);
    QImageWriter writer(filename);
    writer.setCompression(1);
    if(!writer.write(image)) {
      LERROR() << writer.errorString();
    }
  }
  catch (ZException const & e) {
    LERROR() << "Exception:" << e.what();
  }
}

void Z3DRenderProcessor::saveDepthTextureAsImage(const Z3DTexture *tex, const QString &filename)
{
  try {
    GLenum dataFormat = GL_DEPTH_COMPONENT;
    GLenum dataType = GL_UNSIGNED_INT;
    std::unique_ptr<uint32_t[]> depthBuffer(new uint32_t[tex->numPixels()]);
    tex->downloadTextureToBuffer(dataFormat, dataType, depthBuffer.get());
    nim::ZImg img;
    img.wrapData(depthBuffer.get(), tex->width(), tex->height(), 1);
    img.flip(nim::Dimension::Y);
    img.save(filename);
  }
  catch (ZException const & e) {
    LERROR() << "Exception:" << e.what();
  }
}

} // namespace nim
