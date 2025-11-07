#pragma once

#include "zcombobox.h"
#include "zglmutils.h"
#include "zparameter.h"
#include <type_traits>

namespace nim {

// Helper for static_assert in dependent contexts
template<typename>
struct dependent_false : std::false_type
{};

// One of many options parameter. T is the description type, which should be
// convertible to QString if create widget. T2 is data type associated with T.
// Associated data is optional. It can be retrieved by associatedData().

template<class T, class T2 = int>
class ZOptionParameter : public ZSingleValueParameter<T>
{
public:
  explicit ZOptionParameter(const QString& name, QObject* parent = nullptr, QString prefix = "", QString suffix = "");

  [[nodiscard]] QString prefix() const
  {
    return m_prefix;
  }

  [[nodiscard]] QString suffix() const
  {
    return m_suffix;
  }

  void select(const T& value);

  void selectNext();

  [[nodiscard]] bool isSelected(const T& value) const;

  [[nodiscard]] bool isEmpty() const
  {
    return m_options.empty();
  }

  [[nodiscard]] bool hasOption(const T& value) const
  {
    return contains(m_options, value);
  }

  [[nodiscard]] T2 associatedData() const
  {
    return m_associatedData;
  }

  void addOption(const T& value)
  {
    if (hasOption(value)) {
      return;
    }
    m_options.push_back(value);
    m_associatedDatas.push_back(T2());
    Q_EMIT this->reservedStringSignal1(comboBoxItemString(value));
    if (!m_dataIsValid) {
      select(value);
      m_dataIsValid = true;
    }
  }

  // Introspection helpers for schema/capabilities dumping
  const std::vector<T>& options() const
  {
    return m_options;
  }
  const std::vector<T2>& optionAssociatedData() const
  {
    return m_associatedDatas;
  }

  void clearOptions()
  {
    m_options.clear();
    m_associatedDatas.clear();
    m_dataIsValid = false;
    Q_EMIT this->reservedSignal1();
  }

  void removeOption(const T& value)
  {
    auto index = indexOf(m_options, value);
    if (index < 0) {
      return;
    }
    m_options.erase(m_options.begin() + index);
    m_associatedDatas.erase(m_associatedDatas.begin() + index);
    Q_EMIT this->reservedStringSignal2(comboBoxItemString(value));
    index = indexOf(m_options, this->m_value);
    if (index >= 0) {
      Q_EMIT this->reservedIntSignal1(index);
    } else {
      Q_EMIT this->reservedIntSignal1(0);
      this->set(m_options[0]);
    }
  }

  void addOptions(const T& op1, const T& op2)
  {
    addOption(op1);
    addOption(op2);
  }

  void addOptions(const T& op1, const T& op2, const T& op3)
  {
    addOption(op1);
    addOptions(op2, op3);
  }

  void addOptions(const T& op1, const T& op2, const T& op3, const T& op4)
  {
    addOption(op1);
    addOptions(op2, op3, op4);
  }

  void addOptions(const T& op1, const T& op2, const T& op3, const T& op4, const T& op5)
  {
    addOption(op1);
    addOptions(op2, op3, op4, op5);
  }

  void addOptions(const T& op1, const T& op2, const T& op3, const T& op4, const T& op5, const T& op6)
  {
    addOption(op1);
    addOptions(op2, op3, op4, op5, op6);
  }

  void addOptions(const T& op1, const T& op2, const T& op3, const T& op4, const T& op5, const T& op6, const T& op7)
  {
    addOption(op1);
    addOptions(op2, op3, op4, op5, op6, op7);
  }

  void addOptions(const T& op1,
                  const T& op2,
                  const T& op3,
                  const T& op4,
                  const T& op5,
                  const T& op6,
                  const T& op7,
                  const T& op8)
  {
    addOption(op1);
    addOptions(op2, op3, op4, op5, op6, op7, op8);
  }

  void addOptionWithData(const std::pair<T, T2>& value)
  {
    if (hasOption(value.first)) {
      return;
    }
    m_options.push_back(value.first);
    m_associatedDatas.push_back(value.second);
    Q_EMIT this->reservedStringSignal1(comboBoxItemString(value.first));
    if (!m_dataIsValid) {
      select(value.first);
      m_dataIsValid = true;
    }
  }

  void addOptionsWithData(const std::pair<T, T2>& op1, const std::pair<T, T2>& op2)
  {
    addOptionWithData(op1);
    addOptionWithData(op2);
  }

  void addOptionsWithData(const std::pair<T, T2>& op1, const std::pair<T, T2>& op2, const std::pair<T, T2>& op3)
  {
    addOptionWithData(op1);
    addOptionsWithData(op2, op3);
  }

  void addOptionsWithData(const std::pair<T, T2>& op1,
                          const std::pair<T, T2>& op2,
                          const std::pair<T, T2>& op3,
                          const std::pair<T, T2>& op4)
  {
    addOptionWithData(op1);
    addOptionsWithData(op2, op3, op4);
  }

  void addOptionsWithData(const std::pair<T, T2>& op1,
                          const std::pair<T, T2>& op2,
                          const std::pair<T, T2>& op3,
                          const std::pair<T, T2>& op4,
                          const std::pair<T, T2>& op5)
  {
    addOptionWithData(op1);
    addOptionsWithData(op2, op3, op4, op5);
  }

  void addOptionsWithData(const std::pair<T, T2>& op1,
                          const std::pair<T, T2>& op2,
                          const std::pair<T, T2>& op3,
                          const std::pair<T, T2>& op4,
                          const std::pair<T, T2>& op5,
                          const std::pair<T, T2>& op6)
  {
    addOptionWithData(op1);
    addOptionsWithData(op2, op3, op4, op5, op6);
  }

  void addOptionsWithData(const std::pair<T, T2>& op1,
                          const std::pair<T, T2>& op2,
                          const std::pair<T, T2>& op3,
                          const std::pair<T, T2>& op4,
                          const std::pair<T, T2>& op5,
                          const std::pair<T, T2>& op6,
                          const std::pair<T, T2>& op7)
  {
    addOptionWithData(op1);
    addOptionsWithData(op2, op3, op4, op5, op6, op7);
  }

  void addOptionsWithData(const std::pair<T, T2>& op1,
                          const std::pair<T, T2>& op2,
                          const std::pair<T, T2>& op3,
                          const std::pair<T, T2>& op4,
                          const std::pair<T, T2>& op5,
                          const std::pair<T, T2>& op6,
                          const std::pair<T, T2>& op7,
                          const std::pair<T, T2>& op8)
  {
    addOptionWithData(op1);
    addOptionsWithData(op2, op3, op4, op5, op6, op7, op8);
  }

  void setSameAs(const ZParameter& rhs) override;

  [[nodiscard]] bool supportInterpolation() const override
  {
    return false;
  }

  [[nodiscard]] json::value jsonValue() const override;

  void readValue(const json::value& jsonValue) override;

  void forceSetValueSameAs(const ZParameter& rhs) override;

  [[nodiscard]] json::object valueSchema() const override
  {
    json::object o;
    if constexpr (std::is_same_v<T, QString>) {
      o["type"] = "string";
      if (!m_options.empty()) {
        json::array enums;
        for (const auto& s : m_options) {
          enums.emplace_back(s.toStdString());
        }
        o["enum"] = enums;
      }
    } else if constexpr (std::is_same_v<T, int>) {
      o["type"] = "integer";
      if (!m_options.empty()) {
        json::array enums;
        for (const auto& v : m_options) {
          enums.emplace_back(v);
        }
        o["enum"] = enums;
      }
    } else {
      static_assert(dependent_false<T>::value,
                    "ZOptionParameter::valueSchema: unsupported option value type T."
                    " Add a specialization/branch for this type.");
    }
    return o;
  }

protected:
  void reservedIntSlot1(int v) override;

  QWidget* actualCreateWidget(QWidget* parent) override;

  void beforeChange(const T& value) override;

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
  explicit ZStringIntOptionParameter(const QString& name,
                                     QObject* parent = nullptr,
                                     const QString& prefix = "",
                                     const QString& suffix = "");
};

class ZStringStringOptionParameter : public ZOptionParameter<QString, QString>
{
  Q_OBJECT

public:
  explicit ZStringStringOptionParameter(const QString& name,
                                        QObject* parent = nullptr,
                                        const QString& prefix = "",
                                        const QString& suffix = "");
};

class ZIntIntOptionParameter : public ZOptionParameter<int, int>
{
  Q_OBJECT

public:
  explicit ZIntIntOptionParameter(const QString& name,
                                  QObject* parent = nullptr,
                                  const QString& prefix = "",
                                  const QString& suffix = "");
};

} // namespace nim
