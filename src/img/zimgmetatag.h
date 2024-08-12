#pragma once

#include "zimginterface.h"
#include <boost/align/aligned_allocator.hpp>
#include <utility>
#include <vector>

namespace nim {

class ZTiff;

// memory will be allocated automatically based on current data type and count
class ZImgMetatag
{
public:
  ZImgMetatag() = default;

  // convenient function to create a name-value pair meta data
  // value will be stored as utf-8 string with datatype DataType::Ascii
  // input value string should has utf-8 encoding
  ZImgMetatag(std::string name, const std::string& value, uint32_t tag = 0);

  ZImgMetatag(std::string name, const char* value, size_t maxLen, uint32_t tag = 0);

  ZImgMetatag(std::string name, const QString& value, uint32_t tag = 0);

  ZImgMetatag(ZImgMetatag&&) = default;

  ZImgMetatag& operator=(ZImgMetatag&&) = default;

  ZImgMetatag(const ZImgMetatag&) = default;

  ZImgMetatag& operator=(const ZImgMetatag&) = default;

  void swap(ZImgMetatag& other) noexcept
  {
    m_name.swap(other.m_name);
    std::swap(m_tag, other.m_tag);
    std::swap(m_dataType, other.m_dataType);
    std::swap(m_count, other.m_count);
    m_data.swap(other.m_data);
  }

  [[nodiscard]] std::string toString() const;

  [[nodiscard]] QString toQString() const
  {
    return QString::fromStdString(toString());
  }

  [[nodiscard]] const std::string& name() const
  {
    return m_name;
  }

  void setName(const std::string& n)
  {
    m_name = n;
  }

  [[nodiscard]] uint32_t tag() const
  {
    return m_tag;
  }

  void setTag(uint32_t t)
  {
    m_tag = t;
  }

  [[nodiscard]] DataType dataType() const
  {
    return m_dataType;
  }

  void setDataType(DataType dt)
  {
    m_dataType = dt;
    allocateData();
  }

  [[nodiscard]] uint64_t count() const
  {
    return m_count;
  }

  void setCount(uint64_t c)
  {
    m_count = c;
    allocateData();
  }

  [[nodiscard]] size_t dataByteNumber() const
  {
    return byteNumber(m_dataType) * m_count;
  }

  // read data as array of T, you need to know the correct dataType before calling this
  template<typename T = uint8_t>
  const T* dataArray() const
  {
    return reinterpret_cast<const T*>(m_data.data());
  }

  template<typename T = uint8_t>
  T* dataArray()
  {
    return reinterpret_cast<T*>(m_data.data());
  }

  template<typename T>
  T dataAt(size_t idx) const
  {
    return *(reinterpret_cast<const T*>(m_data.data()) + idx);
  }

  template<typename T>
  T& dataAt(size_t idx)
  {
    return *(reinterpret_cast<T*>(m_data.data()) + idx);
  }

private:
  void allocateData()
  {
    m_data.resize(dataByteNumber());
  }

private:
  std::string m_name;
  uint32_t m_tag = 0;
  DataType m_dataType = DataType::Byte;
  uint64_t m_count = 0;
  std::vector<uint8_t, boost::alignment::aligned_allocator<uint8_t, 64>> m_data;
};

} // namespace nim
