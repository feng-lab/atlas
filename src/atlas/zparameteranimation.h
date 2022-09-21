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
  ZParameterAnimation(QString name,
                      QString type,
                      const QColor& color = QColor(ZRandom::instance().randInt(255),
                                                   ZRandom::instance().randInt(255),
                                                   ZRandom::instance().randInt(255)),
                      QObject* parent = nullptr);

  ~ZParameterAnimation() override;

  // not valid means parameter type is wrong or no key
  [[nodiscard]] bool isValid() const
  {
    return m_boundPara && !m_keys.empty();
  }

  void bindParameter(ZParameter& para);

  void releaseParameter();

  [[nodiscard]] ZParameter* boundParameter() const
  {
    return m_boundPara;
  }

  [[nodiscard]] inline QString name() const
  {
    return m_name;
  }

  inline void setName(const QString& n)
  {
    m_name = n;
  }

  [[nodiscard]] inline QString type() const
  {
    return m_type;
  }

  [[nodiscard]] inline QColor color() const
  {
    return m_color;
  }

  inline void setColor(const QColor& c)
  {
    if (m_color != c) {
      m_color = c;
      Q_EMIT colorChanged(this);
    }
  }

  void deleteKey(ZParameterKey* key);

  void addKey(std::unique_ptr<ZParameterKey> key, bool keepRedundant = true);

  [[nodiscard]] const std::vector<std::unique_ptr<ZParameterKey>>& keys() const
  {
    return m_keys;
  }

  [[nodiscard]] int numKeys() const
  {
    return m_keys.size();
  }

  void sortKeys();

  void emitKeyChangedSignal(ZParameterKey* key)
  {
    Q_EMIT keyChanged(key);
  }

  void emitKeysChangedSignal()
  {
    Q_EMIT keysChanged();
  }

  [[nodiscard]] QString jsonKey() const;

  // might return nullptr
  static ZParameterAnimation* create(const QString& key, const json::value& value, QObject* parent = nullptr);

  void write(json::object& json) const;

  // create a new key based on current view
  [[nodiscard]] virtual std::unique_ptr<ZParameterKey> createKey(double secs) const;

  void setCurrentTime(double secs);

  void removeRedundantKeys();

Q_SIGNALS:

  void colorChanged(ZParameterAnimation* pa);

  void keysChanged();

  void keyChanged(ZParameterKey* key);

  void keyAboutToDelete(ZParameterKey* key);

protected:
  virtual void updateParaToTime(double secs, ZParameter* para) const;

  [[nodiscard]] inline const ZParameterKey& lastKey() const
  {
    return *m_keys.back();
  }

protected:
  QString m_name;
  QString m_type;
  QColor m_color;
  std::vector<std::unique_ptr<ZParameterKey>> m_keys;
  ZParameter* m_boundPara = nullptr;
};

} // namespace nim
