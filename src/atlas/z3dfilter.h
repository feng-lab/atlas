#pragma once

#include "z3dtypes.h"
#include "z3dcanvaseventlistener.h"
#include "zflags.h"
#include "zjson.h"
#include "zglmutils.h"
#include <QObject>
#include <map>
#include <set>
#include <vector>

namespace nim {

class Z3DInteractionHandler;

class ZParameter;

class ZEventListenerParameter;

class Z3DFilter
  : public QObject
  , public Z3DCanvasEventListener
{
  Q_OBJECT

  // The rendering engine drives the filter pipeline and needs access to
  // internal validity and size propagation hooks.
  friend class Z3DRenderingEngine;

public:
  // specifies the invalidation status of the filter.
  // The rendering engine uses this value to mark filters that have to be executed.
  enum class State
  {
    Valid = 0,
    MonoViewResultInvalid = 1,
    LeftEyeResultInvalid = 1 << 1,
    RightEyeResultInvalid = 1 << 2,
    StereoResultInvalid = LeftEyeResultInvalid | RightEyeResultInvalid,
    AllResultInvalid = MonoViewResultInvalid | StereoResultInvalid
  };

  explicit Z3DFilter(QObject* parent = nullptr);

  [[nodiscard]] QString className() const
  {
    return metaObject()->className();
  }

  void setName(const QString& name)
  {
    m_name = name;
  }

  [[nodiscard]] QString name() const
  {
    return m_name;
  }

  // returns all parameters
  [[nodiscard]] const std::vector<ZParameter*>& parameters() const
  {
    return m_parameters;
  }

  std::vector<ZParameter*>& parameters()
  {
    return m_parameters;
  }

  // returns first parameter with the given name. return nullptr if not found
  [[nodiscard]] ZParameter* parameter(const QString& name) const;

  virtual void invalidate(State inv);

  virtual void setProgressiveRenderingMode(bool /*v*/) {}

  void onEvent(QEvent* e, int w, int h) override;

  [[nodiscard]] const std::vector<ZEventListenerParameter*>& eventListeners() const
  {
    return m_eventListeners;
  }

  [[nodiscard]] const std::vector<Z3DInteractionHandler*>& interactionHandlers() const
  {
    return m_interactionHandlers;
  }

  void read(const json::object& json);

  void write(json::object& json) const;

  void invalidateResult()
  {
    invalidate(State::AllResultInvalid);
  }

  // returns true if filter is ready to do rendering
  // The default implementation return true
  [[nodiscard]] virtual bool isReady(Z3DEye eye) const;

  // Debugging aid: record a short reason before calling invalidate.
  // Set by parameter-change hooks or invalidations.
#ifdef NO // ATLAS_DEBUG_VERSION
  void debugSetInvalidateReason(const QString& reason)
  {
    m_lastInvalidateReason = reason;
  }

  // Retrieve and clear the last reason. Intended for logging at invalidate sites.
  QString debugTakeInvalidateReason()
  {
    QString r = m_lastInvalidateReason;
    m_lastInvalidateReason.clear();
    return r;
  }
#endif

Q_SIGNALS:
  void renderingError(const QString& error) const;

  // Emitted when this filter's outputs become invalid for any reason.
  void invalidated();

protected:
  // mark that the output of current filter for certain eye is valid.
  // if process function (e.g. prepare data) is not related to stereo view or mono view, you should rewrite this
  // function in subclass and set the invalidstate to VALID to avoid being executed again for
  // a different eye parameter.
  // This function is called by the engine after process(eye) is called.
  virtual void setValid(Z3DEye eye);

  // return true if the output of current filter for certain eye is valid.
  // Used by the engine to decide whether it is necessary to call process(eye).
  [[nodiscard]] virtual bool isValid(Z3DEye eye) const;

  // this is the place to do rendering related work
  // the engine will set its invalidation level to VALID after calling this
  // input is current camera (eye), can be left or right in stereo case
  virtual double process(Z3DEye eye) = 0;

  void addParameter(ZParameter& para, State inv = State::AllResultInvalid);

  void removeParameter(ZParameter& para);

  // listen to some events
  void addEventListener(ZEventListenerParameter& para);

  // react to interaction
  void addInteractionHandler(Z3DInteractionHandler& handler);

  virtual void enterInteractionMode() {}

  virtual void exitInteractionMode() {}

  // NOTE: Atlas' simplified engine pipeline now treats the target render size as a global contract
  // (driven by the compositor output size). The default implementation ignores targetSize and only
  // invalidates results; filters that need explicit sizing should override and
  // use targetSize as the desired output resolution.
  virtual void updateSize(const glm::uvec2& targetSize);

protected:
  State m_state;

  QString m_name;

  // Last render target size observed via updateSize(). Used to avoid triggering
  // expensive invalidation/cancellation cascades when the engine rebuilds the
  // pipeline but the output size is unchanged (e.g. during incremental mesh loads).
  glm::uvec2 m_lastUpdateSize{0u, 0u};

  // all parameters that can change the render behavior
  std::vector<ZParameter*> m_parameters;
  std::set<QString> m_parameterNames;

  std::vector<ZEventListenerParameter*> m_eventListeners;
  std::vector<Z3DInteractionHandler*> m_interactionHandlers;

  // Debug: last invalidate reason string (optional)
#ifdef ATLAS_DEBUG_VERSION
  QString m_lastInvalidateReason;
#endif
};

DECLARE_OPERATORS_FOR_ENUM(Z3DFilter::State)

} // namespace nim
