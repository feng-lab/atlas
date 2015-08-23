#include "z3dgl.h"
#include "z3drenderport.h"
#include "z3drendertarget.h"

#include "z3dgpuinfo.h"

namespace nim {

Z3DRenderOutputPort::Z3DRenderOutputPort(const QString &name, bool allowMultipleConnections,
                                         Z3DFilter::InvalidationState invalidationState, GLint internalColorFormat, GLint internalDepthFormat)
  : Z3DOutputPortBase(name, allowMultipleConnections, invalidationState)
  , m_resultIsValid(false)
  , m_internalColorFormat(internalColorFormat)
  , m_internalDepthFormat(internalDepthFormat)
  , m_multisample(false)
  , m_sample(4)
  , m_renderTarget(m_internalColorFormat, m_internalDepthFormat,
                   m_size, m_multisample, m_sample)
{
  CHECK_GL_ERROR;
}

Z3DRenderOutputPort::~Z3DRenderOutputPort()
{
}

void Z3DRenderOutputPort::invalidate()
{
  m_resultIsValid = false;
  Z3DOutputPortBase::invalidate();
}

void Z3DRenderOutputPort::clearTarget() const
{
  if (!isBound())
    LERROR() << "RenderTarget is not bound, can not clear.";
  else
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}

Z3DTexture *Z3DRenderOutputPort::colorTexture()
{
  return m_renderTarget.attachment(GL_COLOR_ATTACHMENT0);
}

Z3DTexture *Z3DRenderOutputPort::depthTexture()
{
  return m_renderTarget.attachment(GL_DEPTH_ATTACHMENT);
}

void Z3DRenderOutputPort::resize(const glm::ivec2 &newsize)
{
  if (m_size == newsize)
    return;
  if (newsize == glm::ivec2(0)) {
    LWARN() << "invalid size:" << newsize;
    return;
  }
  if (newsize.x > Z3DGpuInfoInstance.maxTextureSize() ||
      newsize.y > Z3DGpuInfoInstance.maxTextureSize()) {
    LWARN() << "size" << newsize << "exceeds texture size limit:"
            << Z3DGpuInfoInstance.maxTextureSize();
    return;
  }
  m_renderTarget.resize(newsize);
  m_resultIsValid = false;
  m_size = newsize;
}

void Z3DRenderOutputPort::changeColorFormat(GLint internalColorFormat)
{
  m_internalColorFormat = internalColorFormat;
  m_renderTarget.changeColorAttachmentFormat(m_internalColorFormat);
  invalidate();
}

void Z3DRenderOutputPort::chagneDepthFormat(GLint internalDepthFormat)
{
  m_internalDepthFormat = internalDepthFormat;
  m_renderTarget.changeDepthAttachmentFormat(m_internalDepthFormat);
  invalidate();
}

bool Z3DRenderOutputPort::canConnectTo(const Z3DInputPortBase *inport) const
{
  if (dynamic_cast<const Z3DRenderInputPort*>(inport))
    return Z3DOutputPortBase::canConnectTo(inport);
  else
    return false;
}

//void Z3DRenderOutputPort::setMultisample(bool multisample, int nsample)
//{
//  if (!isInitialized() || (multisample == m_multisample && nsample == m_sample))
//    return;
//  m_multisample = multisample;
//  m_sample = nsample;
//  changeFormat(m_internalColorFormat, m_internalDepthFormat);    // use same format, just replace rendertarget
//}

//-----------------------------------------------------------------------------------

Z3DRenderInputPort::Z3DRenderInputPort(const QString &name, bool allowMultipleConnections,
                                       Z3DFilter::InvalidationState invalidationState)
  : Z3DInputPortBase(name, allowMultipleConnections, invalidationState)
{
}

Z3DRenderInputPort::~Z3DRenderInputPort()
{
}

size_t Z3DRenderInputPort::numValidInputs() const
{
  size_t res = 0;
  for (size_t i=0; i<m_connectedOutputPorts.size(); ++i) {
    const Z3DRenderOutputPort* p = dynamic_cast<const Z3DRenderOutputPort*>(m_connectedOutputPorts[i]);
    assert(p);
    if (p->hasValidData())
      ++res;
  }
  return res;
}

glm::ivec2 Z3DRenderInputPort::size(size_t idx) const
{
  if (renderTarget(idx))
    return renderTarget(idx)->size();
  else
    return glm::ivec2(0);
}

const Z3DTexture *Z3DRenderInputPort::colorTexture(size_t idx) const
{
  if (renderTarget(idx))
    return renderTarget(idx)->attachment(GL_COLOR_ATTACHMENT0);
  else
    return NULL;
}

const Z3DTexture *Z3DRenderInputPort::depthTexture(size_t idx) const
{
  if (renderTarget(idx))
    return renderTarget(idx)->attachment(GL_DEPTH_ATTACHMENT);
  else
    return NULL;
}

const Z3DRenderTarget *Z3DRenderInputPort::renderTarget(size_t idx) const
{
  if (idx >= numValidInputs())
    return NULL;
  size_t res = 0;
  for (size_t i=0; i<m_connectedOutputPorts.size(); ++i) {
    const Z3DRenderOutputPort* p = dynamic_cast<const Z3DRenderOutputPort*>(m_connectedOutputPorts[i]);
    assert(p);
    if (p->hasValidData())
      ++res;
    if (idx == res - 1)
      return &p->renderTarget();
  }
  return nullptr;
}

} // namespace nim
