#ifndef ZPARAMETER_H
#define ZPARAMETER_H

// Base class for all parameters that algorithms or renderers need.
// A parameter can emit changed() signal while changed. And it has
// createWidget functions which can be used for UI
// generation. The changed() signal can be used to change algorithm
// or renderer behavior dynamicly.

#include <QObject>
#include <QStringList>
#include <set>
#include <cassert>
#include <QJsonObject>
#include <QJsonValue>

class QWidget;
class QLayout;
class QLabel;

namespace nim {

class ZParameter : public QObject
{
  Q_OBJECT
public:
  explicit ZParameter(const QString& name, QObject *parent = 0);
  virtual ~ZParameter();

  inline QString name() const {return m_name;}
  void setName(const QString& name);
  QString type() const;

  inline QString style() const {return m_style;}
  // if style does not exist, fall back to "DEFAULT"
  void setStyle(const QString& style);

  // create widget based on current style
  QLabel* createNameLabel(QWidget* parent = NULL);
  QWidget* createWidget(QWidget* parent = NULL);

  bool isVisible() const { return m_isWidgetsVisible; }
  bool isEnabled() const { return m_isWidgetsEnabled; }

  QString jsonKey() const;
  virtual QJsonValue jsonValue() const = 0;
  virtual void readValue(const QJsonValue &value) = 0;
  void read(const QJsonObject &json);
  void write(QJsonObject &json) const;

  // set everything same as
  virtual void setSameAs(const ZParameter &rhs) = 0;
  // set value same as
  virtual void setValueSameAs(const ZParameter &rhs) = 0;
  // for option parameter
  virtual void forceSetValueSameAs(const ZParameter &rhs) { setValueSameAs(rhs); }
  inline bool isSameType(const ZParameter &rhs) const { return metaObject()->className() == rhs.metaObject()->className(); }
  // return true if new value can be interpolated between two state
  virtual bool supportInterpolation() const { return true; }
  // if interpolation is not supported, assign dest to one of prev (if progress < 1) and current (if progress >= 1)
  virtual void interpolate(const ZParameter& prev, double progress, ZParameter& dest) = 0;

public slots:
  void setVisible(bool s);
  void setEnabled(bool s);
  void updateFromSender();
  
signals:
  void nameChanged(const QString &);
  void valueChanged();

  void setWidgetsEnabled(bool s);
  void setWidgetsVisible(bool s);

  // some templated subclass might need this
  void reservedIntSignal1(int);
  void reservedIntSignal2(int);
  void reservedStringSignal1(QString);
  void reservedStringSignal2(QString);
  void reservedSignal1();
  void reservedSignal2();

protected slots:
  // some templated subclass might need this
  virtual void reservedIntSlot1(int) {}
  virtual void reservedIntSlot2(int) {}
  virtual void reservedStringSlot1(QString) {}
  virtual void reservedStringSlot2(QString) {}
  virtual void reservedSlot1() {}
  virtual void reservedSlot2() {}

protected:
  inline void addStyle(const QString& style) {m_allStyles.push_back(style);}
  // all subclass should implement this function
  virtual QWidget* actualCreateWidget(QWidget* parent) = 0;

  QString m_name;
  QString m_style;
  QStringList m_allStyles;

  //std::set<QWidget*> m_widgets;
  bool m_isWidgetsEnabled;
  bool m_isWidgetsVisible;
};

// parameter contains a single value
template<class T>
class ZSingleValueParameter : public ZParameter {
public:
  ZSingleValueParameter(const QString &name, const T &value, QObject *parent = NULL);
  ZSingleValueParameter(const QString &name, QObject *parent = NULL);

  virtual ~ZSingleValueParameter();

  void set(const T &valueIn);

  inline const T& get() const { return m_value; }
  inline T& get() {return m_value; }

  virtual void setValueSameAs(const ZParameter &rhs) override
  {
    assert(this->isSameType(rhs));
    set(static_cast<const ZSingleValueParameter<T>*>(&rhs)->get());
  }

  virtual void interpolate(const ZParameter& prev, double progress, ZParameter& dest) override
  {
    assert(this->isSameType(prev) && this->isSameType(dest));
    dest.setValueSameAs(progress >= 1.0 ? *this : prev);
  }

protected:
  // subclass can use this function to change input value to a valid value
  // default implement do nothing
  virtual void makeValid(T& value) const;
  // subclass can use this function to emit customized signal before m_value
  // is changed or do other update, input is new value
  virtual void beforeChange(T& value);
  //
  virtual void afterChange(T& value);

  T m_value;
  bool m_locked;
};

//---------------------------------------------------------------------------

template<class T>
ZSingleValueParameter<T>::ZSingleValueParameter(const QString& name, const T &value, QObject *parent)
  : ZParameter(name, parent)
  , m_value(value), m_locked(false)
{}

template<class T>
ZSingleValueParameter<T>::ZSingleValueParameter(const QString& name, QObject *parent)
  : ZParameter(name, parent), m_locked(false)
{}

template<class T>
ZSingleValueParameter<T>::~ZSingleValueParameter()
{}

template<class T>
void ZSingleValueParameter<T>::set(const T &valueIn)
{
  if (m_locked)
    return;    // prevent widget change echo back
  if (m_value != valueIn) {
    T value = valueIn;
    makeValid(value);
    if (m_value != value) {
      m_locked = true;
      beforeChange(value);
      m_value = value;
      emit valueChanged();
      afterChange(value);
      m_locked = false;
    }
  }
}

template<class T>
void ZSingleValueParameter<T>::makeValid(T &/*value*/) const
{
}

template<class T>
void ZSingleValueParameter<T>::beforeChange(T &/*value*/)
{
}

template<class T>
void ZSingleValueParameter<T>::afterChange(T &/*value*/)
{
}

//-----------------------------------------------------------------------------------------------

class ZBoolParameter : public ZSingleValueParameter<bool>
{
  Q_OBJECT
public:
  ZBoolParameter(const QString& name, QObject *parent = NULL);
  ZBoolParameter(const QString& name, bool value, QObject *parent = NULL);

signals:
  void valueWillChange(bool);
  void boolChanged(bool);

public slots:
  void setValue(bool v);

protected:
  virtual void beforeChange(bool &value) override;
  virtual void afterChange(bool &value) override;
  virtual QWidget* actualCreateWidget(QWidget *parent) override;

  // ZParameter interface
public:
  virtual void setSameAs(const ZParameter &rhs) override;
  virtual bool supportInterpolation() const override { return false; }
  virtual QJsonValue jsonValue() const override;
  virtual void readValue(const QJsonValue &jsonValue) override;
};

} // namespace nim

#endif // ZPARAMETER_H
