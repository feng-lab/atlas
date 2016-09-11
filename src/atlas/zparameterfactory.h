#pragma once

#include "zparameter.h"
#include <QString>
#include <QObject>
#include <map>

namespace nim {

class ZParameterMakerInterface
{
public:
  ZParameterMakerInterface() = default;

  virtual ~ZParameterMakerInterface() = default;

  virtual ZParameter* create(const QString& name, QObject* parent = nullptr) const = 0;
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

