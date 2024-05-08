#include "zparameterfactory.h"

#include "z3dcameraparameter.h"
#include "z3dtransferfunction.h"
#include "z3dtransformparameter.h"
#include "z2dtransformparameter.h"
#include "zcolormap.h"
#include "znumericparameter.h"
#include "zoptionparameter.h"
#include "zstringparameter.h"
#include "zfontparameter.h"
#include "zlog.h"

namespace nim {

template<typename T>
class ZParameterMaker : public ZParameterMakerInterface
{
public:
  ZParameter* create(const QString& name, QObject* parent) const override
  {
    return new T(name, parent);
  }
};

ZParameterFactory& ZParameterFactory::instance()
{
  static ZParameterFactory factory;
  return factory;
}

ZParameterFactory::ZParameterFactory()
{
  registerMaker("Bool", new ZParameterMaker<ZBoolParameter>());
  registerMaker("3DCamera", new ZParameterMaker<Z3DCameraParameter>());
  registerMaker("3DTransferFunction", new ZParameterMaker<Z3DTransferFunctionParameter>());
  registerMaker("3DTransform", new ZParameterMaker<Z3DTransformParameter>());
  registerMaker("2DTransform", new ZParameterMaker<Z2DTransformParameter>());
  registerMaker("ColorMap", new ZParameterMaker<ZColorMapParameter>());
  registerMaker("Double", new ZParameterMaker<ZDoubleParameter>());
  registerMaker("Float", new ZParameterMaker<ZFloatParameter>());
  registerMaker("Int", new ZParameterMaker<ZIntParameter>());
  registerMaker("DoubleSpan", new ZParameterMaker<ZDoubleSpanParameter>());
  registerMaker("FloatSpan", new ZParameterMaker<ZFloatSpanParameter>());
  registerMaker("IntSpan", new ZParameterMaker<ZIntSpanParameter>());
  registerMaker("DVec2", new ZParameterMaker<ZDVec2Parameter>());
  registerMaker("DVec3", new ZParameterMaker<ZDVec3Parameter>());
  registerMaker("DVec4", new ZParameterMaker<ZDVec4Parameter>());
  registerMaker("Vec2", new ZParameterMaker<ZVec2Parameter>());
  registerMaker("Vec3", new ZParameterMaker<ZVec3Parameter>());
  registerMaker("Vec4", new ZParameterMaker<ZVec4Parameter>());
  registerMaker("IVec2", new ZParameterMaker<ZIVec2Parameter>());
  registerMaker("IVec3", new ZParameterMaker<ZIVec3Parameter>());
  registerMaker("IntIntOption", new ZParameterMaker<ZIntIntOptionParameter>());
  registerMaker("StringIntOption", new ZParameterMaker<ZStringIntOptionParameter>());
  registerMaker("StringStringOption", new ZParameterMaker<ZStringStringOptionParameter>());
  registerMaker("String", new ZParameterMaker<ZStringParameter>());
  registerMaker("Font", new ZParameterMaker<ZFontParameter>());
}

bool ZParameterFactory::isTypeValid(const QString& type)
{
  return m_makers.contains(type);
}

ZParameter* ZParameterFactory::create(const QString& name, const QString& type, QObject* parent) const
{
  auto it = m_makers.find(type);
  if (it == m_makers.end()) {
    return nullptr;
  }
  return it->second->create(name, parent);
}

void ZParameterFactory::registerMaker(const QString& typeName, ZParameterMakerInterface* maker)
{
  if (m_makers.contains(typeName)) {
    LOG(WARNING) << "Multiple makers for type " << typeName;
  }
  m_makers[typeName].reset(maker);
}

} // namespace nim
