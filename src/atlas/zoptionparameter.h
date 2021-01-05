#pragma once

#include "zcombobox.h"
#include "zglmutils.h"
#include "zparameter.h"
#include <QString>

namespace nim {

// One of many options parameter. T is the description type, which should be
// convertible to QString if create widget. T2 is data type associated with T.
// Associated data is optional. It can be retrieved by associatedData().

template<class T, class T2 = int>
class ZOptionParameter : public ZSingleValueParameter<T>
{
public:
  explicit ZOptionParameter(const QString& name, QObject* parent = nullptr, QString prefix = "", QString suffix = "");

  [[nodiscard]] inline QString prefix() const
  { return m_prefix; }

  [[nodiscard]] inline QString suffix() const
  { return m_suffix; }

  void select(const T& value);

  void selectNext();

  [[nodiscard]] bool isSelected(const T& value) const;

  [[nodiscard]] bool isEmpty() const
  { return m_options.empty(); }

  [[nodiscard]] bool hasOption(const T& value) const
  { return contains(m_options, value); }

  [[nodiscard]] inline T2 associatedData() const
  { return m_associatedData; }

  void addOption(const T& value)
  {
    if (hasOption(value)) {
      return;
    }
    m_options.push_back(value);
    m_associatedDatas.push_back(T2());
    emit this->reservedStringSignal1(comboBoxItemString(value));
    if (!m_dataIsValid) {
      select(value);
      m_dataIsValid = true;
    }
  }

  void clearOptions()
  {
    m_options.clear();
    m_associatedDatas.clear();
    m_dataIsValid = false;
    emit this->reservedSignal1();
  }

  void removeOption(const T& value)
  {
    auto index = indexOf(m_options, value);
    if (index < 0) {
      return;
    }
    removeAt(m_options, index);
    removeAt(m_associatedDatas, index);
    emit this->reservedStringSignal2(comboBoxItemString(value));
    index = indexOf(m_options, this->m_value);
    if (index >= 0) {
      emit this->reservedIntSignal1(index);
    } else {
      emit this->reservedIntSignal1(0);
      this->set(m_options[0]);
    }
  }

  inline void addOptions(const T& op1, const T& op2)
  {
    addOption(op1);
    addOption(op2);
  }

  inline void addOptions(const T& op1, const T& op2, const T& op3)
  {
    addOption(op1);
    addOptions(op2, op3);
  }

  inline void addOptions(const T& op1, const T& op2, const T& op3, const T& op4)
  {
    addOption(op1);
    addOptions(op2, op3, op4);
  }

  inline void addOptions(const T& op1, const T& op2, const T& op3, const T& op4, const T& op5)
  {
    addOption(op1);
    addOptions(op2, op3, op4, op5);
  }

  inline void addOptions(const T& op1, const T& op2, const T& op3, const T& op4, const T& op5,
                         const T& op6)
  {
    addOption(op1);
    addOptions(op2, op3, op4, op5, op6);
  }

  inline void addOptions(const T& op1, const T& op2, const T& op3, const T& op4, const T& op5,
                         const T& op6, const T& op7)
  {
    addOption(op1);
    addOptions(op2, op3, op4, op5, op6, op7);
  }

  inline void addOptions(const T& op1, const T& op2, const T& op3, const T& op4, const T& op5,
                         const T& op6, const T& op7, const T& op8)
  {
    addOption(op1);
    addOptions(op2, op3, op4, op5, op6, op7, op8);
  }

  inline void addOptionWithData(const std::pair<T, T2>& value)
  {
    if (hasOption(value.first)) {
      return;
    }
    m_options.push_back(value.first);
    m_associatedDatas.push_back(value.second);
    emit this->reservedStringSignal1(comboBoxItemString(value.first));
    if (!m_dataIsValid) {
      select(value.first);
      m_dataIsValid = true;
    }
  }

  inline void addOptionsWithData(const std::pair<T, T2>& op1, const std::pair<T, T2>& op2)
  {
    addOptionWithData(op1);
    addOptionWithData(op2);
  }

  inline void addOptionsWithData(const std::pair<T, T2>& op1, const std::pair<T, T2>& op2, const std::pair<T, T2>& op3)
  {
    addOptionWithData(op1);
    addOptionsWithData(op2, op3);
  }

  inline void
  addOptionsWithData(const std::pair<T, T2>& op1, const std::pair<T, T2>& op2, const std::pair<T, T2>& op3,
                     const std::pair<T, T2>& op4)
  {
    addOptionWithData(op1);
    addOptionsWithData(op2, op3, op4);
  }

  inline void
  addOptionsWithData(const std::pair<T, T2>& op1, const std::pair<T, T2>& op2, const std::pair<T, T2>& op3,
                     const std::pair<T, T2>& op4,
                     const std::pair<T, T2>& op5)
  {
    addOptionWithData(op1);
    addOptionsWithData(op2, op3, op4, op5);
  }

  inline void
  addOptionsWithData(const std::pair<T, T2>& op1, const std::pair<T, T2>& op2, const std::pair<T, T2>& op3,
                     const std::pair<T, T2>& op4,
                     const std::pair<T, T2>& op5, const std::pair<T, T2>& op6)
  {
    addOptionWithData(op1);
    addOptionsWithData(op2, op3, op4, op5, op6);
  }

  inline void
  addOptionsWithData(const std::pair<T, T2>& op1, const std::pair<T, T2>& op2, const std::pair<T, T2>& op3,
                     const std::pair<T, T2>& op4,
                     const std::pair<T, T2>& op5, const std::pair<T, T2>& op6, const std::pair<T, T2>& op7)
  {
    addOptionWithData(op1);
    addOptionsWithData(op2, op3, op4, op5, op6, op7);
  }

  inline void
  addOptionsWithData(const std::pair<T, T2>& op1, const std::pair<T, T2>& op2, const std::pair<T, T2>& op3,
                     const std::pair<T, T2>& op4,
                     const std::pair<T, T2>& op5, const std::pair<T, T2>& op6, const std::pair<T, T2>& op7,
                     const std::pair<T, T2>& op8)
  {
    addOptionWithData(op1);
    addOptionsWithData(op2, op3, op4, op5, op6, op7, op8);
  }

  void setSameAs(const ZParameter& rhs) override;

  [[nodiscard]] bool supportInterpolation() const override
  { return false; }

  [[nodiscard]] json::value jsonValue() const override;

  void readValue(const json::value& jsonValue) override;

  void forceSetValueSameAs(const ZParameter& rhs) override;

protected:
  void reservedIntSlot1(int v) override;

  QWidget* actualCreateWidget(QWidget* parent) override;

  void beforeChange(T& value) override;

  void makeValid(T& value) const override;

  [[nodiscard]] QString comboBoxItemString(const T& value) const;

private:
  std::vector<T> m_options;
  T2 m_associatedData;
  std::vector<T2> m_associatedDatas;

  bool m_dataIsValid = false;
  QString m_prefix;
  QString m_suffix;
};

class ZStringIntOptionParameter : public ZOptionParameter<QString, int>
{
Q_OBJECT
public:
  explicit ZStringIntOptionParameter(const QString& name, QObject* parent = nullptr, const QString& prefix = "",
                                     const QString& suffix = "");
};

class ZStringStringOptionParameter : public ZOptionParameter<QString, QString>
{
Q_OBJECT
public:
  explicit ZStringStringOptionParameter(const QString& name, QObject* parent = nullptr, const QString& prefix = "",
                                        const QString& suffix = "");
};

class ZIntIntOptionParameter : public ZOptionParameter<int, int>
{
Q_OBJECT
public:
  explicit ZIntIntOptionParameter(const QString& name, QObject* parent = nullptr, const QString& prefix = "",
                                  const QString& suffix = "");
};

} // namespace nim

