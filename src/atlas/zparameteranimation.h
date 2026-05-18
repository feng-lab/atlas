#pragma once

#include "zrandom.h"
#include "zparameterkey.h"
#include "zparameter.h"
#include <QObject>
#include <QColor>
#include <mutex>
#include <vector>

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

  [[nodiscard]] QString name() const
  {
    return m_name;
  }

  void setName(const QString& n)
  {
    m_name = n;
  }

  [[nodiscard]] QString type() const
  {
    return m_type;
  }

  [[nodiscard]] QColor color() const
  {
    return m_color;
  }

  void setColor(const QColor& c)
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

  virtual void write(json::object& json) const;

  // create a new key based on current view
  [[nodiscard]] virtual std::unique_ptr<ZParameterKey> createKey(double secs) const;

  void setCurrentTime(double secs) const;

  void removeRedundantKeys();

  // Replace the entire key list and emit a single keysChanged().
  // Keys are sorted by time; redundant keys are preserved.
  void replaceKeys(std::vector<std::unique_ptr<ZParameterKey>> keys);

  void clearKeys()
  {
    replaceKeys({});
  }

Q_SIGNALS:
  void colorChanged(ZParameterAnimation* pa);

  void keysChanged();

  void keyChanged(ZParameterKey* key);

  void keyAboutToDelete(ZParameterKey* key);

protected:
  virtual void updateParaToTime(double secs, ZParameter* para) const;

  [[nodiscard]] const ZParameterKey& lastKey() const
  {
    return *m_keys.back();
  }

protected:
  QString m_name;
  QString m_type;
  QColor m_color;
  std::vector<std::unique_ptr<ZParameterKey>> m_keys;
  ZParameter* m_boundPara = nullptr;
  // Animation key evaluation can run on the bound parameter's owning thread
  // (e.g., the Z3DRenderingEngine thread), while UI edits happen on the UI thread.
  // Protect the key list and related derived caches (e.g., camera splines) against
  // concurrent access. Recursive to allow direct Qt signal handlers (keysChanged →
  // buildSpline) to re-enter while mutations hold the lock.
  mutable std::recursive_mutex m_keysMutex;
};

} // namespace nim
