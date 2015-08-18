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
  qDeleteAll(m_keys);
  m_keys.clear();
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
  m_keys.removeAll(key);
  delete key;
  emit keyChanged();
}

void ZParameterAnimation::addKey(ZParameterKey *key, bool keepRedundant)
{
  assert(key);
  assert(key->time() >= 0.0);
  assert(key->value().type() == m_type);

  key->setParaAnimation(this);

  if (m_keys.empty()) {
    m_keys.push_back(key);
    emit keyChanged();
    return;
  }

  if (key->time() < m_keys[0]->time()) {
    if (keepRedundant || key->value().jsonValue() != m_keys[0]->value().jsonValue()) {
      m_keys.push_front(key);
      emit keyChanged();
      return;
    }
  }

  for (int i=0; i<m_keys.size(); ++i) {
    if (key->time() == m_keys[i]->time()) {
      delete m_keys[i];
      m_keys[i] = key;
      emit keyChanged();
      return;
    }
    if (key->time() > m_keys[i]->time()) {
      if (i+1 < m_keys.size()) {
        if (key->time() < m_keys[i+1]->time()) {
          if (keepRedundant || key->value().jsonValue() != m_keys[i]->value().jsonValue() ||
              key->value().jsonValue() != m_keys[i+1]->value().jsonValue()) {
            m_keys.insert(i+1, key);
            emit keyChanged();
            return;
          }
        }
      } else {
        if (keepRedundant || key->value().jsonValue() != m_keys[i]->value().jsonValue()) {
          m_keys.push_back(key);
          emit keyChanged();
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
        ZCameraParameterKey* cpkey = new ZCameraParameterKey();
        if (!cpkey->readValue(keyArray.at(i))) {
          delete cpkey;
        } else {
          res->addKey(cpkey);
        }
      }
    }
    return res;
  } else {
    ZParameterAnimation* res = new ZParameterAnimation(name, type, color, parent);
    if (obj.contains("keys")) {
      QJsonArray keyArray = obj.value("keys").toArray();
      for (int i=0; i<keyArray.size(); ++i) {
        ZParameterKey* cpkey = new ZParameterKey(type);
        if (!cpkey->readValue(keyArray.at(i))) {
          delete cpkey;
        } else {
          res->addKey(cpkey);
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
    for (int i=0; i<m_keys.size(); ++i) {
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
  for (int i=1; i<m_keys.size(); ++i) {
    if (secs < m_keys[i]->time()) {
      m_keys[i]->interpolate(*m_keys[i-1], secs, *para);
      return;
    }
  }
  para->setValueSameAs(lastKey()->value());
}

void ZParameterAnimation::removeRedundantKeys()
{
  if (m_keys.empty())
    return;
  // todo: for switch key
  QList<ZParameterKey*>::iterator it = m_keys.begin();
  QJsonValue prevValue = (*it)->value().jsonValue();
  ++it;
  while (it != m_keys.end()) {
    QJsonValue currValue = (*it)->value().jsonValue();
    if (currValue == prevValue) {
      QList<ZParameterKey*>::iterator nextIt = it+1;
      if (nextIt == m_keys.end()) {
        delete *it;
        it = m_keys.erase(it);
      } else if (currValue == (*nextIt)->value().jsonValue()) {
        delete *it;
        it = m_keys.erase(it);
      } else {
        prevValue = (*it)->value().jsonValue();
        ++it;
      }
    } else {
      prevValue = (*it)->value().jsonValue();
      ++it;
    }
  }
}

} // namespace nim
