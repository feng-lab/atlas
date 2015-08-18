#ifndef Z3DRENDERPORT_H
#define Z3DRENDERPORT_H

#include "z3dport.h"
#include "z3dtexture.h"
#include <typeinfo>
#include "z3drendertarget.h"

class Z3DRenderInputPort;

#ifdef _USE_GLEW_
#ifndef GL_DEPTH_COMPONENT24
#define GL_DEPTH_COMPONENT24 0x81A6
#endif
#ifndef GL_RGBA16
#define GL_RGBA16 0x805B
#endif
#endif

namespace nim {

class Z3DRenderOutputPort : public Z3DOutputPortBase
{
public:
  Z3DRenderOutputPort(const QString& name, bool allowMultipleConnections = true,
                      Z3DProcessor::InvalidationState invalidationState = Z3DProcessor::InvalidAllResult,
                      GLint internalColorFormat=(GLint)GL_RGBA16, GLint internalDepthFormat=(GLint)GL_DEPTH_COMPONENT24);
  virtual ~Z3DRenderOutputPort();

  virtual void invalidate() override;

  void bindTarget() { m_renderTarget.bind(); m_resultIsValid = true; }
  void releaseTarget() { m_renderTarget.release(); }

  GLint internalDepthFormat() const { return m_internalDepthFormat; }
  GLint internalColorFormat() const { return m_internalColorFormat; }

  // Clears the contents of an activated outport's RenderTarget,
  void clearTarget();

  virtual bool hasValidData() const override { return m_resultIsValid; }

  // returns the dimensions of the associated RenderTarget
  glm::ivec2 size() const { return m_renderTarget.size(); }

  // return the maximum of expectesize of all connected inports.
  // If no inport connected, return (-1, -1)
  glm::ivec2 expectedSize() const;

  // Returns true, if the associated RenderTarget is currently bound.
  bool isBound() const { return m_renderTarget.isBound(); }

  const Z3DRenderTarget& renderTarget() const { return m_renderTarget; }
  Z3DRenderTarget& renderTarget() { return m_renderTarget; }

  Z3DTexture* colorTexture();
  Z3DTexture* depthTexture();

  // Resizes the associated RenderTarget to the passed dimensions.
  void resize(const glm::ivec2& newsize);
  void resize(int x, int y) { resize(glm::ivec2(x,y)); }

  // change RenderTarget with the given format.
  void changeColorFormat(GLint internalColorFormat);
  void chagneDepthFormat(GLint internalDepthFormat);

  virtual bool canConnectTo(const Z3DInputPortBase* inport) const override;

  //void setMultisample(bool multisample, int nsample = 4);

public:
  virtual void setProcessor(Z3DProcessor *p) override;

private:
  bool m_resultIsValid;
  glm::ivec2 m_size;

  GLint m_internalColorFormat;
  GLint m_internalDepthFormat;

  bool m_multisample;
  int m_sample;

  Z3DRenderTarget m_renderTarget;
};

class Z3DRenderInputPort : public Z3DInputPortBase
{
public:
  Z3DRenderInputPort(const QString &name, bool allowMultipleConnections = false,
                     Z3DProcessor::InvalidationState invalidationState = Z3DProcessor::InvalidAllResult);
  virtual ~Z3DRenderInputPort();

  virtual bool isReady() const override { return numValidInputs() > 0; }

  void setExpectedSize(glm::ivec2 size) { m_expectedSize = size; }
  glm::ivec2 expectedSize() const { return m_expectedSize; }

  // go through all connected output render ports and count how many have valid rendering
  size_t numValidInputs() const;

  // once we have the number of valid inputs, we can use a index as parameter to query data from input
  // idx range from 0 to numValidInputs() - 1
  glm::ivec2 size(size_t idx = 0) const;
  const Z3DTexture* colorTexture(size_t idx = 0) const;
  const Z3DTexture* depthTexture(size_t idx = 0) const;

protected:
  virtual void setProcessor(Z3DProcessor *p) override;

private:
  const Z3DRenderTarget* renderTarget(size_t idx) const;

  glm::ivec2 m_expectedSize;
};

} // namespace nim

#endif // Z3DRENDERPORT_H
