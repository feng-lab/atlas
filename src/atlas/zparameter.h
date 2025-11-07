#pragma once

// Base class for all parameters that algorithms or renderers need.
// A parameter can Q_EMIT changed() signal while changed. And it has
// createWidget functions which can be used for UI
// generation. The changed() signal can be used to change algorithm
// or renderer behavior dynamicly.

#include "zjson.h"
#include "zlog.h"
#include <QObject>
#include <QStringList>
#include <mutex>
#include <set>

class QWidget;

class QLayout;

class QLabel;

namespace nim {

class ZParameter : public QObject
{
  Q_OBJECT

public:
  explicit ZParameter(QString name, QObject* parent = nullptr);

  [[nodiscard]] QString name() const
  {
    return m_name;
  }

  void setName(const QString& name);

  [[nodiscard]] QString type() const;

  // Optional human-readable description for tooling/LLMs/UIs
  [[nodiscard]] QString description() const
  {
    return m_description;
  }
  void setDescription(const QString& d)
  {
    m_description = d;
  }

  [[nodiscard]] QString style() const
  {
    return m_style;
  }

  // if style does not exist, fall back to "DEFAULT"
  void setStyle(const QString& style);

  // create widget based on current style
  QLabel* createNameLabel(QWidget* parent = nullptr);

  QWidget* createWidget(QWidget* parent = nullptr);

  [[nodiscard]] bool isVisible() const
  {
    return m_isWidgetsVisible;
  }

  [[nodiscard]] bool isEnabled() const
  {
    return m_isWidgetsEnabled;
  }

  [[nodiscard]] QString jsonKey() const;

  [[nodiscard]] virtual json::value jsonValue() const = 0;

  virtual void readValue(const json::value& value) = 0;

  void read(const json::object& json);

  void write(json::object& json) const;

  // JSON Schema for the parameter's VALUE shape (not the surrounding
  // animation key object). Every subclass must implement this.
  [[nodiscard]] virtual json::object valueSchema() const = 0;

  // set everything same as
  virtual void setSameAs(const ZParameter& rhs) = 0;

  // set value same as
  virtual void setValueSameAs(const ZParameter& rhs) = 0;

  // for option parameter
  virtual void forceSetValueSameAs(const ZParameter& rhs)
  {
    setValueSameAs(rhs);
  }

  [[nodiscard]] bool isSameType(const ZParameter& rhs) const
  {
    return metaObject()->className() == rhs.metaObject()->className();
  }

  // return true if new value can be interpolated between two state
  [[nodiscard]] virtual bool supportInterpolation() const
  {
    return true;
  }

  // if interpolation is not supported, assign dest to one of prev (if progress < 1) and current (if progress >= 1)
  virtual void interpolate(const ZParameter& prev, double progress, ZParameter& dest) = 0;

  void setVisible(bool s);

  void setEnabled(bool s);

  void updateFromSender();

Q_SIGNALS:
  void nameChanged(const QString&);

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

protected:
  // some templated subclass might need this
  virtual void reservedIntSlot1(int) {}

  virtual void reservedIntSlot2(int) {}

  virtual void reservedStringSlot1(const QString&) {}

  virtual void reservedStringSlot2(const QString&) {}

  virtual void reservedSlot1() {}

  virtual void reservedSlot2() {}

  void addStyle(const QString& style)
  {
    m_allStyles.push_back(style);
  }

  // all subclass should implement this function
  virtual QWidget* actualCreateWidget(QWidget* parent) = 0;

protected:
  QString m_name;
  QString m_description;
  QString m_style{"DEFAULT"};
  QStringList m_allStyles;

  // std::set<QWidget*> m_widgets;
  bool m_isWidgetsEnabled = true;
  bool m_isWidgetsVisible = true;
};

// parameter contains a single value
template<class T>
class ZSingleValueParameter : public ZParameter
{
public:
  ZSingleValueParameter(const QString& name, const T& value, QObject* parent = nullptr);

  explicit ZSingleValueParameter(const QString& name, QObject* parent = nullptr);

  void set(const T& valueIn);

  [[nodiscard]] const T& get() const
  {
    return m_value;
  }

  [[nodiscard]] T& get()
  {
    return m_value;
  }

  void setValueSameAs(const ZParameter& rhs) override
  {
    CHECK(this->isSameType(rhs));
    set(static_cast<const ZSingleValueParameter<T>*>(&rhs)->get());
  }

  void interpolate(const ZParameter& prev, double progress, ZParameter& dest) override
  {
    CHECK(this->isSameType(prev) && this->isSameType(dest));
    dest.setValueSameAs(progress >= 1.0 ? *this : prev);
  }

protected:
  // Produce the canonical value before hooks run. Subclasses may adjust
  // the incoming value here; set() calls this before any signals.
  // Default: no-op.
  virtual void makeValid(T& value) const;

  // Read-only hook: emit "will change" signals or sync dependent UI/state
  // based on the new value about to be committed. Never mutate 'value'.
  virtual void beforeChange(const T& value);

  // Read-only hook: emit typed changed signals or perform side effects
  // after commit. Never mutate 'value'.
  virtual void afterChange(const T& value);

protected:
  T m_value;
  std::mutex m_mutex;
};

//---------------------------------------------------------------------------

template<class T>
ZSingleValueParameter<T>::ZSingleValueParameter(const QString& name, const T& value, QObject* parent)
  : ZParameter(name, parent)
  , m_value(value)
{}

template<class T>
ZSingleValueParameter<T>::ZSingleValueParameter(const QString& name, QObject* parent)
  : ZParameter(name, parent)
  , m_value{}
{}

template<class T>
void ZSingleValueParameter<T>::set(const T& valueIn)
{
  if (m_mutex.try_lock()) {
    m_mutex.unlock();
  } else {
    return; // prevent widget change echo back
  }
  if (m_value != valueIn) {
    T value = valueIn;
    makeValid(value);
    if (m_value != value) {
      std::scoped_lock locker(m_mutex);
      beforeChange(value);
      m_value = value;
      VLOG(2) << m_name;
      Q_EMIT valueChanged();
      afterChange(value);
    }
  }
}

template<class T>
void ZSingleValueParameter<T>::makeValid(T& /*value*/) const
{}

template<class T>
void ZSingleValueParameter<T>::beforeChange(const T& /*value*/)
{}

template<class T>
void ZSingleValueParameter<T>::afterChange(const T& /*value*/)
{}

//-----------------------------------------------------------------------------------------------

class ZBoolParameter : public ZSingleValueParameter<bool>
{
  Q_OBJECT

public:
  explicit ZBoolParameter(const QString& name, QObject* parent = nullptr);

  ZBoolParameter(const QString& name, bool value, QObject* parent = nullptr);

  // ZParameter interface

public:
  void setSameAs(const ZParameter& rhs) override;

  [[nodiscard]] bool supportInterpolation() const override
  {
    return false;
  }

  [[nodiscard]] json::value jsonValue() const override;

  void readValue(const json::value& jsonValue) override;

  [[nodiscard]] json::object valueSchema() const override;

  void setValue(bool v);

Q_SIGNALS:
  void valueWillChange(bool);

  void boolChanged(bool);

protected:
  void beforeChange(const bool& value) override;

  void afterChange(const bool& value) override;

  QWidget* actualCreateWidget(QWidget* parent) override;
};

} // namespace nim
