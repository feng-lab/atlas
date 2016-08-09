#pragma once

#include <QString>
#include <QObject>
#include <map>
#include "zparameter.h"

namespace nim {

#define ZParameterFactoryInstance nim::ZParameterFactory::instance()

class ZParameterMakerInterface
{
public:
  virtual ZParameter* create(const QString& name, QObject* parent = nullptr) const = 0;

  virtual ~ZParameterMakerInterface()
  {}
};

class ZParameterFactory
{
public:
  static ZParameterFactory& instance();

  ZParameterFactory();

  bool isTypeValid(const QString& type);

  // return nullptr if failed
  ZParameter* create(const QString& name, const QString& type, QObject* parent = nullptr) const;

  void registerMaker(const QString& typeName, ZParameterMakerInterface* maker);

private:
  std::map<QString, std::unique_ptr<ZParameterMakerInterface>> m_makers;
};

} // namespace nim

