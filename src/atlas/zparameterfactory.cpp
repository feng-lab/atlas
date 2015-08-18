#include "zparameterfactory.h"

#include "z3dcameraparameter.h"
#include "z3dtransferfunction.h"
#include "z3dtransformparameter.h"
#include "zcolormap.h"
#include "znumericparameter.h"
#include "zoptionparameter.h"
#include "zstringparameter.h"
#include "zfontparameter.h"

namespace nim {

template<typename T>
class ZParameterMaker : public ZParameterMakerInterface
{
public:
  ZParameter* create(const QString &name, QObject *parent = nullptr) const
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

ZParameterFactory::~ZParameterFactory()
{
  for (std::map<QString, ZParameterMakerInterface*>::iterator it = m_makers.begin();
       it != m_makers.end(); ++it) {
    delete it->second;
  }
  m_makers.clear();
}

bool ZParameterFactory::isTypeValid(const QString &type)
{
  return m_makers.find(type) != m_makers.end();
}

ZParameter *ZParameterFactory::create(const QString &name, const QString &type, QObject *parent) const
{
  std::map<QString, ZParameterMakerInterface*>::const_iterator it = m_makers.find(type);
  if (it == m_makers.end())
    return nullptr;
  return it->second->create(name, parent);
}

void ZParameterFactory::registerMaker(const QString &typeName, ZParameterMakerInterface *maker)
{
  if (m_makers.find(typeName) != m_makers.end()) {
    LWARN() << "Multiple makers for type" << typeName;
    delete m_makers[typeName];
  }
  m_makers[typeName] = maker;
}

} // namespace nim
