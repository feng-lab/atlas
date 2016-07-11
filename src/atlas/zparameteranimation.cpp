#include "zparameteranimation.h"

#include "QsLog.h"
#include "zglmutils.h"
#include <QJsonArray>
#include "zparameterfactory.h"
#include "zcameraparameteranimation.h"
#include "zcameraparameterkey.h"

namespace nim {

ZParameterAnimation::ZParameterAnimation(const QString &name, const QString &type, const QColor &color, QObject *parent)
  : QObject(parent), m_name(name), m_type(type), m_color(color), m_boundPara(nullptr)
{
}

ZParameterAnimation::~ZParameterAnimation()
{
  if (m_boundPara)
    releaseParameter();
}

void ZParameterAnimation::bindParameter(ZParameter &para)
{
  assert(para.type() == m_type);
  m_boundPara = &para;
}

void ZParameterAnimation::releaseParameter()
{
  m_boundPara = nullptr;
}

void ZParameterAnimation::deleteKey(ZParameterKey *key)
{
  key->setParaAnimation(nullptr);
  emit keyAboutToDelete(key);
  m_keys.erase(std::remove_if(m_keys.begin(), m_keys.end(),
                              [=](const auto &ckey) { return ckey.get() == key; }),
               m_keys.end());
  emit keysChanged();
}

void ZParameterAnimation::addKey(ZParameterKey *keyIn, bool keepRedundant)
{
  std::unique_ptr<ZParameterKey> key(keyIn);
  assert(key);
  assert(key->time() >= 0.0);
  assert(key->value().type() == m_type);

  key->setParaAnimation(this);

  if (m_keys.empty()) {
    m_keys.push_back(std::move(key));
    emit keysChanged();
    return;
  }

  if (key->time() < m_keys[0]->time()) {
    if (keepRedundant || key->value().jsonValue() != m_keys[0]->value().jsonValue()) {
      m_keys.insert(m_keys.begin(), std::move(key));
      emit keysChanged();
      return;
    }
  }

  for (size_t i=0; i<m_keys.size(); ++i) {
    if (key->time() == m_keys[i]->time()) {
      m_keys[i] = std::move(key);
      emit keysChanged();
      return;
    }
    if (key->time() > m_keys[i]->time()) {
      if (i+1 < m_keys.size()) {
        if (key->time() < m_keys[i+1]->time()) {
          if (keepRedundant || key->value().jsonValue() != m_keys[i]->value().jsonValue() ||
              key->value().jsonValue() != m_keys[i+1]->value().jsonValue()) {
            m_keys.insert(m_keys.begin()+i+1, std::move(key));
            emit keysChanged();
            return;
          }
        }
      } else {
        if (keepRedundant || key->value().jsonValue() != m_keys[i]->value().jsonValue()) {
          m_keys.push_back(std::move(key));
          emit keysChanged();
          return;
        }
      }
    }
  }
}

QString ZParameterAnimation::jsonKey() const
{
  return m_name + QString(" ") + m_type;
}

ZParameterAnimation *ZParameterAnimation::create(const QString &key, const QJsonValue &value, QObject *parent)
{
  int spaceIdx = key.lastIndexOf(QChar(' '));
  if (spaceIdx == -1) {
    LWARN() << "Invalid Animation Parameter" << key;
    return nullptr;
  }
  QString name = key.left(spaceIdx);
  QString type = key.mid(spaceIdx+1);
  QColor color(0,0,0,255);
  if (!ZParameterFactoryInstance.isTypeValid(type)) {
    LWARN() << "Invalid Animation Parameter" << key;
    return nullptr;
  }
  if (!value.isObject()) {
    LWARN() << "Invalid Animation Parameter" << key << "value";
    return nullptr;
  }
  QJsonObject obj = value.toObject();
  if (obj.contains("color") && obj.value("color").isString()) {
    toVal(obj.value("color").toString(), color);
  }
  if (type == "3DCamera") {
    ZCameraParameterAnimation* res = new ZCameraParameterAnimation(name, color, parent);
    if (obj.contains("keys")) {
      QJsonArray keyArray = obj.value("keys").toArray();
      for (int i=0; i<keyArray.size(); ++i) {
        auto cpkey = std::make_unique<ZCameraParameterKey>();
        if (cpkey->readValue(keyArray.at(i))) {
          res->addKey(cpkey.release());
        }
      }
    }
    return res;
  } else {
    ZParameterAnimation* res = new ZParameterAnimation(name, type, color, parent);
    if (obj.contains("keys")) {
      QJsonArray keyArray = obj.value("keys").toArray();
      for (int i=0; i<keyArray.size(); ++i) {
        auto cpkey = std::make_unique<ZParameterKey>(type);
        if (cpkey->readValue(keyArray.at(i))) {
          res->addKey(cpkey.release());
        }
      }
    }
    return res;
  }
}

void ZParameterAnimation::write(QJsonObject &json) const
{
  QJsonObject obj;
  obj["color"] = toQString(m_color);
  if (!m_keys.empty()) {
    QJsonArray keysArray;
    for (size_t i=0; i<m_keys.size(); ++i) {
      keysArray.append(m_keys[i]->jsonValue());
    }
    obj["keys"] = keysArray;
  }
  json.insert(jsonKey(), obj);
}

ZParameterKey *ZParameterAnimation::createKey(double secs) const
{
  assert(secs >= 0);
  assert(m_boundPara);

  return new ZParameterKey(secs, *m_boundPara);
}

void ZParameterAnimation::setCurrentTime(double secs)
{
  if (!m_boundPara)
    return;
  updateParaToTime(secs, m_boundPara);
}

void ZParameterAnimation::updateParaToTime(double secs, ZParameter *para) const
{
  assert(para->type() == m_type);
  assert(secs >= 0);

  if (m_keys.empty())
    return;
  if (secs <= m_keys[0]->time()) {
    para->setValueSameAs(m_keys[0]->value());
    return;
  }
  for (size_t i=1; i<m_keys.size(); ++i) {
    if (secs < m_keys[i]->time()) {
      m_keys[i]->interpolate(*m_keys[i-1], secs, *para);
      return;
    }
  }
  para->setValueSameAs(lastKey().value());
}

void ZParameterAnimation::removeRedundantKeys()
{
  if (m_keys.size() < 2)
    return;

  auto result = m_keys.begin();
  ++result;
  auto first = result;
  int prevDist = 1;
  while (first != m_keys.end()) {
    auto prev = first - prevDist;
    auto next = first + 1;
    if ((*first)->value().jsonValue() != (*prev)->value().jsonValue() ||
        (next != m_keys.end() && (*first)->value().jsonValue() != (*next)->value().jsonValue())) {
      *result = std::move(*first);
      ++result;
    } else {
      ++prevDist;
    }
    ++first;
  }
  m_keys.erase(result, m_keys.end());
}

} // namespace nim
