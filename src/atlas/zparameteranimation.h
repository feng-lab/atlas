#ifndef ZPARAMETERANIMATION_H
#define ZPARAMETERANIMATION_H

#include <QObject>
#include <QColor>
#include "zrandom.h"
#include "zparameterkey.h"
#include "zparameter.h"

namespace nim {

class ZParameterAnimation : public QObject
{
  Q_OBJECT
public:
  ZParameterAnimation(const QString &name, const QString &type,
                      const QColor &color = QColor(ZRandomInstance.randInt(255),
                                                   ZRandomInstance.randInt(255),
                                                   ZRandomInstance.randInt(255)),
                      QObject *parent = 0);
  virtual ~ZParameterAnimation();

  // not valid means parameter type is wrong or no key
  bool isValid() const { return m_boundPara && !m_keys.empty(); }

  void bindParameter(ZParameter &para);
  void releaseParameter();
  ZParameter* boundParameter() const { return m_boundPara; }

  inline QString name() const { return m_name; }
  inline void setName(const QString &n) { m_name = n; }
  inline QString type() const { return m_type; }

  inline QColor color() const { return m_color; }
  inline void setColor(QColor c) { if (m_color != c) { m_color = c; emit colorChanged(this); } }

  void deleteKey(ZParameterKey* key);
  void addKey(ZParameterKey* key, bool keepRedundant = true);
  const QList<ZParameterKey*>& keys() const { return m_keys; }
  int numKeys() const { return m_keys.size(); }
  void emitKeyChangedSignal(ZParameterKey* key) { emit keyChanged(key); }

  QString jsonKey() const;
  // might return nullptr
  static ZParameterAnimation* create(const QString& key, const QJsonValue &value, QObject *parent = nullptr);
  void write(QJsonObject &json) const;

  // create a new key based on current view
  virtual ZParameterKey* createKey(double secs) const;

signals:
  void colorChanged(ZParameterAnimation* pa);
  void keyChanged();
  void keyChanged(ZParameterKey* key);
  void keyAboutToDelete(ZParameterKey* key);

public slots:
  void setCurrentTime(double secs);
  void removeRedundantKeys();
  virtual void updateParaToTime(double secs, ZParameter* para) const;

protected:
  inline const ZParameterKey* lastKey() const { return m_keys[m_keys.size()-1]; }

protected:
  QString m_name;
  QString m_type;
  QColor m_color;
  QList<ZParameterKey*> m_keys;
  ZParameter *m_boundPara;
};

} // namespace nim

#endif // ZPARAMETERANIMATION_H
