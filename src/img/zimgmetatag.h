#ifndef ZIMGMETATAG_H
#define ZIMGMETATAG_H

#include <vector>
#include "zimginterface.h"
#include "zimginfo.h"
#include <boost/align/aligned_allocator.hpp>

namespace nim {

class ZTiff;

// memory will be allocated automatically based on current data type and count
class ZImgMetatag
{
public:
  ZImgMetatag();
  ZImgMetatag(const ZImgMetatag &other);
  // convenient function to create a name-value pair meta data
  // value will be stored as utf-8 string with datatype DataType::Ascii
  ZImgMetatag(const QString &name, const QString &value);

  void swap(ZImgMetatag &other)
  {
    m_name.swap(other.m_name);
    std::swap(m_tag, other.m_tag);
    std::swap(m_dataType, other.m_dataType);
    std::swap(m_count, other.m_count);
    m_data.swap(other.m_data);
  }

  QString toQString() const;

  inline const QString& name() const { return m_name; }
  inline void setName(const QString &n) { m_name = n; }
  inline uint32_t tag() const { return m_tag; }
  inline void setTag(uint32_t t) { m_tag = t; }
  inline DataType dataType() const { return m_dataType; }
  inline void setDataType(DataType dt) { m_dataType = dt; allocateData(); }
  inline uint64_t count() const { return m_count; }
  inline void setCount(uint64_t c) { m_count = c; allocateData(); }

  inline size_t dataByteNumber() const { return byteNumber(m_dataType) * m_count; }

  // read data as array of T, you need to know the correct dataType before calling this
  template <typename T = uint8_t>
  inline const T* dataArray() const
  {
    return bit_cast<const T*>(m_data.data());
  }
  template <typename T = uint8_t>
  inline T* dataArray()
  {
    return bit_cast<T*>(m_data.data());
  }
  template <typename T>
  inline T dataAt(size_t idx) const
  {
    return *(bit_cast<const T*>(m_data.data()) + idx);
  }
  template <typename T>
  inline T& dataAt(size_t idx)
  {
    return *(bit_cast<T*>(m_data.data()) + idx);
  }

private:
  inline void allocateData() { m_data.resize(dataByteNumber()); }

private:
  QString m_name;
  uint32_t m_tag;
  DataType m_dataType;
  uint64_t m_count;
  std::vector<uint8_t, boost::alignment::aligned_allocator<uint8_t, 32>> m_data;
};

} // namespace

#endif // ZIMGMETATAG_H
