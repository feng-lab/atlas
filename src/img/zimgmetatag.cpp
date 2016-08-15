#include "zimgmetatag.h"

namespace nim {

ZImgMetatag::ZImgMetatag()
  : m_tag(0), m_dataType(DataType::Byte), m_count(0)
{
}

ZImgMetatag::ZImgMetatag(const QString& name, const QString& value)
  : m_name(name), m_tag(0), m_dataType(DataType::Ascii)
{
  QByteArray utf8array = value.toUtf8();
  setCount(utf8array.size() + 1);
  memcpy(dataArray<char>(), utf8array.constData(), dataByteNumber());
}

QString ZImgMetatag::toQString() const
{
  QString res = m_name;
  if (m_tag != 0)
    res = res % QString(" (tag:%1)").arg(m_tag);
  res = res % QString(" %2 %3<").arg(enumToString(m_dataType)).arg(m_count);
  QString sep;
  uint64_t ct = m_count;

  switch (m_dataType) {
    case DataType::Undefined:
    case DataType::Byte:
      for (size_t i = 0; i < m_count; ++i)
        res = res % QString("%1%2").arg(sep).arg(static_cast<int>(dataAt<uint8_t>(i)), 0, 16), sep = " ";
      break;
    case DataType::SByte:
      for (size_t i = 0; i < m_count; ++i)
        res = res % QString("%1%2").arg(sep).arg(static_cast<int>(dataAt<char>(i))), sep = " ";
      break;
    case DataType::Ascii:
      if (!m_data.empty())
        res = res % QString::fromUtf8(dataArray<char>(), m_data.size() - 1);
      break;
    case DataType::Short:
      for (size_t i = 0; i < m_count; ++i)
        res = res % QString("%1%2").arg(sep).arg(dataAt<uint16_t>(i)), sep = " ";
      break;
    case DataType::SShort:
      for (size_t i = 0; i < m_count; ++i)
        res = res % QString("%1%2").arg(sep).arg(dataAt<int16_t>(i)), sep = " ";
      break;
    case DataType::Long:
      for (size_t i = 0; i < m_count; ++i)
        res = res % QString("%1%2").arg(sep).arg(dataAt<uint32_t>(i)), sep = " ";
      break;
    case DataType::SLong:
      for (size_t i = 0; i < m_count; ++i)
        res = res % QString("%1%2").arg(sep).arg(dataAt<int32_t>(i)), sep = " ";
      break;
    case DataType::Long8:
      for (size_t i = 0; i < m_count; ++i)
        res = res % QString("%1%2").arg(sep).arg(dataAt<uint64_t>(i)), sep = " ";
      break;
    case DataType::SLong8:
      for (size_t i = 0; i < m_count; ++i)
        res = res % QString("%1%2").arg(sep).arg(dataAt<int64_t>(i)), sep = " ";
      break;
    case DataType::Rational: {
      const uint32_t* lp = dataArray<uint32_t>();
      while (ct-- > 0) {
        if (lp[1] == 0)
          res = res % QString("%1Nan (%2/0)").arg(sep).arg(lp[0]);
        else
          res = res % QString("%1%2 (%3/%4)").arg(sep).arg((static_cast<double>(lp[0]) / static_cast<double>(lp[1])))
            .arg(lp[0]).arg(lp[1]);
        sep = " ";
        lp += 2;
      }
      break;
    }
    case DataType::SRational: {
      const int32_t* lp = dataArray<int32_t>();
      while (ct-- > 0) {
        if (lp[1] == 0)
          res = res % QString("%1Nan (%2/0)").arg(sep).arg(lp[0]);
        else
          res = res % QString("%1%2 (%3/%4)").arg(sep).arg((static_cast<double>(lp[0]) / static_cast<double>(lp[1])))
            .arg(lp[0]).arg(lp[1]);
        sep = " ";
        lp += 2;
      }
      break;
    }
    case DataType::Float:
      for (size_t i = 0; i < m_count; ++i)
        res = res % QString("%1%2").arg(sep).arg(dataAt<float>(i)), sep = " ";
      break;
    case DataType::Double:
      for (size_t i = 0; i < m_count; ++i)
        res = res % QString("%1%2").arg(sep).arg(dataAt<double>(i)), sep = " ";
      break;
    case DataType::IFD:
      for (size_t i = 0; i < m_count; ++i)
        res = res % QString("%1%2").arg(sep).arg(dataAt<uint32_t>(i), 0, 16), sep = " ";
      break;
    case DataType::IFD8:
      for (size_t i = 0; i < m_count; ++i)
        res = res % QString("%1%2").arg(sep).arg(dataAt<uint64_t>(i), 0, 16), sep = " ";
      break;
    default:
      break;
  }

  return res % QString(">");
}

/*
std::string ZImgMetatag::toString() const
{
  std::ostringstream res;
  if (m_tag != 0)
    res << m_name << " (tag:" << m_tag << ") " << DataTypeString[m_dataType] << " " << m_count << "<";
  else
    res << m_name << " " << DataTypeString[m_dataType] << " " << m_count << "<";
  const char* sep = "";
  uint64_t ct = m_count;

  switch (m_dataType) {
  case DataType::Undefined:
  case DataType::Byte:
    for (size_t i=0; i<m_count; ++i)
      res << sep << std::hex << static_cast<int>(dataAt<uint8_t>(i)), sep = " ";
    break;
  case DataType::SByte:
    for (size_t i=0; i<m_count; ++i)
      res << sep << static_cast<int>(dataAt<char>(i)), sep = " ";
    break;
  case DataType::Ascii:
    res << sep << dataArray<char>(), sep = " ";
    break;
  case DataType::Short:
    for (size_t i=0; i<m_count; ++i)
      res << sep << dataAt<uint16_t>(i), sep = " ";
    break;
  case DataType::SShort:
    for (size_t i=0; i<m_count; ++i)
      res << sep << dataAt<int16_t>(i), sep = " ";
    break;
  case DataType::Long:
    for (size_t i=0; i<m_count; ++i)
      res << sep << dataAt<uint32_t>(i), sep = " ";
    break;
  case DataType::SLong:
    for (size_t i=0; i<m_count; ++i)
      res << sep << dataAt<int32_t>(i), sep = " ";
    break;
  case DataType::Long8:
    for (size_t i=0; i<m_count; ++i)
      res << sep << dataAt<uint64_t>(i), sep = " ";
    break;
  case DataType::SLong8:
    for (size_t i=0; i<m_count; ++i)
      res << sep << dataAt<int64_t>(i), sep = " ";
    break;
  case DataType::Rational: {
    const uint32_t *lp = dataArray<uint32_t>();
    while (ct-- > 0) {
      if (lp[1] == 0)
        res << sep << "Nan (" << lp[0] << "/" << lp[1] << ")";
      else
        res << sep << ((double)lp[0] / (double)lp[1]);
      sep = " ";
      lp += 2;
    }
    break;
  }
  case DataType::SRational: {
    const int32_t *lp = dataArray<int32_t>();
    while (ct-- > 0) {
      if (lp[1] == 0)
        res << sep << "Nan (" << lp[0] << "/" << lp[1] << ")";
      else
        res << sep << ((double)lp[0] / (double)lp[1]);
      sep = " ";
      lp += 2;
    }
    break;
  }
  case DataType::Float:
    for (size_t i=0; i<m_count; ++i)
      res << sep << dataAt<float>(i), sep = " ";
    break;
  case DataType::Double:
    for (size_t i=0; i<m_count; ++i)
      res << sep << dataAt<double>(i), sep = " ";
    break;
  case DataType::IFD:
    for (size_t i=0; i<m_count; ++i)
      res << sep << std::hex << dataAt<uint32_t>(i), sep = " ";
    break;
  case DataType::IFD8:
    for (size_t i=0; i<m_count; ++i)
      res << sep << std::hex << dataAt<uint64_t>(i), sep = " ";
    break;
  case DataType::None:
  case DTNUMBER:
    break;
  }

  res << ">";
  return res.str();
}
*/

} // namespace
