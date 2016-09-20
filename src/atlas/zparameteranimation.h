#pragma once

#include "zrandom.h"
#include "zparameterkey.h"
#include "zparameter.h"
#include <QObject>
#include <QColor>

namespace nim {

class ZParameterAnimation : public QObject
{
Q_OBJECT
public:
  ZParameterAnimation(const QString& name, const QString& type,
                      const QColor& color = QColor(ZRandom::instance().randInt(255),
                                                   ZRandom::instance().randInt(255),
                                                   ZRandom::instance().randInt(255)),
                      QObject* parent = nullptr);

  virtual ~ZParameterAnimation();

  // not valid means parameter type is wrong or no key
  bool isValid() const
  { return m_boundPara && !m_keys.empty(); }

  void bindParameter(ZParameter& para);

  void releaseParameter();

  ZParameter* boundParameter() const
  { return m_boundPara; }

  inline QString name() const
  { return m_name; }

  inline void setName(const QString& n)
  { m_name = n; }

  inline QString type() const
  { return m_type; }

  inline QColor color() const
  { return m_color; }

  inline void setColor(const QColor& c)
  { if (m_color != c) { m_color = c; emit colorChanged(this); }}

  void deleteKey(ZParameterKey* key);

  void addKey(ZParameterKey* keyIn, bool keepRedundant = true);

  const std::vector<std::unique_ptr<ZParameterKey>>& keys() const
  { return m_keys; }

  int numKeys() const
  { return m_keys.size(); }

  void emitKeyChangedSignal(ZParameterKey* key)
  { emit keyChanged(key); }

  QString jsonKey() const;

  // might return nullptr
  static ZParameterAnimation* create(const QString& key, const QJsonValue& value, QObject* parent = nullptr);

  void write(QJsonObject& json) const;

  // create a new key based on current view
  virtual ZParameterKey* createKey(double secs) const;

  void setCurrentTime(double secs);

  void removeRedundantKeys();

signals:

  void colorChanged(ZParameterAnimation* pa);

  void keysChanged();

  void keyChanged(ZParameterKey* key);

  void keyAboutToDelete(ZParameterKey* key);

protected:
  virtual void updateParaToTime(double secs, ZParameter* para) const;

  inline const ZParameterKey& lastKey() const
  { return *m_keys[m_keys.size() - 1]; }

protected:
  QString m_name;
  QString m_type;
  QColor m_color;
  std::vector<std::unique_ptr<ZParameterKey>> m_keys;
  ZParameter* m_boundPara = nullptr;
};

} // namespace nim

