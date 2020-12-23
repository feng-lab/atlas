#include "zparameteranimation.h"

#include "zlog.h"
#include "zglmutils.h"
#include "zlog.h"
#include "zparameterfactory.h"
#include "zcameraparameteranimation.h"
#include "zserializationutils.h"
#include "zglobal.h"
#include <algorithm>

namespace nim {

ZParameterAnimation::ZParameterAnimation(const QString& name, const QString& type, const QColor& color, QObject* parent)
  : QObject(parent), m_name(name), m_type(type), m_color(color)
{
}

ZParameterAnimation::~ZParameterAnimation()
{
  if (m_boundPara)
    releaseParameter();
}

void ZParameterAnimation::bindParameter(ZParameter& para)
{
  CHECK(para.type() == m_type);
  m_boundPara = &para;
}

void ZParameterAnimation::releaseParameter()
{
  m_boundPara = nullptr;
}

void ZParameterAnimation::deleteKey(ZParameterKey* key)
{
  emit keyAboutToDelete(key);
  std::erase_if(m_keys, [key](const auto& ckey) {
    return ckey.get() == key;
  });
}

void ZParameterAnimation::addKey(std::unique_ptr<ZParameterKey> key, bool keepRedundant)
{
  CHECK(key);
  CHECK(key->time() >= 0.0);
  CHECK(key->value().type() == m_type);

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

  for (size_t i = 0; i < m_keys.size(); ++i) {
    if (key->time() == m_keys[i]->time()) {
      m_keys[i] = std::move(key);
      emit keysChanged();
      return;
    }
    if (key->time() > m_keys[i]->time()) {
      if (i + 1 < m_keys.size()) {
        if (key->time() < m_keys[i + 1]->time()) {
          if (keepRedundant || key->value().jsonValue() != m_keys[i]->value().jsonValue() ||
              key->value().jsonValue() != m_keys[i + 1]->value().jsonValue()) {
            m_keys.insert(m_keys.begin() + i + 1, std::move(key));
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

void ZParameterAnimation::sortKeys()
{
  std::sort(m_keys.begin(), m_keys.end(),
            [](const auto& a, const auto& b) {
              return a->time() < b->time();
            });
}

QString ZParameterAnimation::jsonKey() const
{
  return m_name + QString(" ") + m_type;
}

ZParameterAnimation* ZParameterAnimation::create(const QString& key, const QJsonValue& value, QObject* parent)
{
  int spaceIdx = key.lastIndexOf(QChar(' '));
  if (spaceIdx == -1) {
    LOG(WARNING) << "Invalid Animation Parameter " << key;
    return nullptr;
  }
  QString name = key.left(spaceIdx);
  QString type = key.mid(spaceIdx + 1);
  QColor color(0, 0, 0, 255);
  if (!ZParameterFactory::instance().isTypeValid(type)) {
    LOG(WARNING) << "Invalid Animation Parameter " << key;
    return nullptr;
  }
  if (!value.isObject()) {
    LOG(WARNING) << "Invalid Animation Parameter " << key << " value";
    return nullptr;
  }
  QJsonObject obj = value.toObject();
  if (obj.contains("color") && obj.value("color").isString()) {
    toVal(obj.value("color").toString(), color);
  }
  if (type == "3DCamera") {
    auto res = new ZCameraParameterAnimation(name, color, parent);
    if (obj.contains("keys")) {
      QJsonArray keyArray = obj.value("keys").toArray();
      for (int i = 0; i < keyArray.size(); ++i) {
        auto cpkey = std::make_unique<ZCameraParameterKey>();
        if (cpkey->readValue(keyArray.at(i))) {
          res->addKey(std::unique_ptr<ZParameterKey>(std::move(cpkey)));
        }
      }
    }
    return res;
  } else {
    auto res = new ZParameterAnimation(name, type, color, parent);
    if (obj.contains("keys")) {
      QJsonArray keyArray = obj.value("keys").toArray();
      for (int i = 0; i < keyArray.size(); ++i) {
        auto cpkey = std::make_unique<ZParameterKey>(type);
        if (cpkey->readValue(keyArray.at(i))) {
          res->addKey(std::move(cpkey));
        }
      }
    }
    return res;
  }
}

void ZParameterAnimation::write(QJsonObject& json) const
{
  QJsonObject obj;
  obj["color"] = toQString(m_color);
  if (!m_keys.empty()) {
    QJsonArray keysArray;
    for (size_t i = 0; i < m_keys.size(); ++i) {
      keysArray.append(m_keys[i]->jsonValue());
    }
    obj["keys"] = keysArray;
  }
  json.insert(jsonKey(), obj);
}

std::unique_ptr<ZParameterKey> ZParameterAnimation::createKey(double secs) const
{
  CHECK(secs >= 0);
  CHECK(m_boundPara);

  return std::make_unique<ZParameterKey>(secs, *m_boundPara);
}

void ZParameterAnimation::setCurrentTime(double secs)
{
  if (!m_boundPara)
    return;
  updateParaToTime(secs, m_boundPara);
}

void ZParameterAnimation::updateParaToTime(double secs, ZParameter* para) const
{
  CHECK(para->type() == m_type);
  CHECK(secs >= 0);

  if (m_keys.empty())
    return;
  if (secs <= m_keys[0]->time()) {
    para->setValueSameAs(m_keys[0]->value());
    return;
  }
  for (size_t i = 1; i < m_keys.size(); ++i) {
    if (secs < m_keys[i]->time()) {
      m_keys[i]->interpolate(*m_keys[i - 1], secs, *para);
      return;
    }
  }
  para->setValueSameAs(lastKey().value());
}

void ZParameterAnimation::removeRedundantKeys()
{
  if (m_keys.size() < 2)
    return;

  auto it = m_keys.begin();
  auto result = it;  // pointer to last non-redundant key
  ++it;
  while (it != m_keys.end()) {
    auto next = it + 1;
    if ((*it)->value().jsonValue() != (*result)->value().jsonValue() ||   // not equal to prev
        (next != m_keys.end() && (*it)->value().jsonValue() != (*next)->value().jsonValue())) { // or not equal to next
      ++result;  // make space for new valid it
      if (it != result)
        *result = std::move(*it);   // keep it and advance result
    }
    ++it;
  }
  m_keys.erase(result + 1, m_keys.end());
}

} // namespace nim
