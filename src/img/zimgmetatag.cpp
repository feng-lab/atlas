#include "zimgmetatag.h"

#include <utility>

namespace nim {

ZImgMetatag::ZImgMetatag(QString name, const QString& value)
  : m_name(std::move(name))
  , m_dataType(DataType::Ascii)
{
  auto utf8array = value.toUtf8();
  setCount(utf8array.size() + 1);
  std::memcpy(dataArray<char>(), utf8array.constData(), dataByteNumber());
}

QString ZImgMetatag::toQString() const
{
  QString res = QString(" %1 (tag:%2) %3 %4<").arg(m_name).arg(m_tag).arg(enumToQString(m_dataType)).arg(m_count);
  QString sep;
  uint64_t ct = m_count;

  switch (m_dataType) {
    case DataType::Undefined:
    case DataType::Byte:
      for (size_t i = 0; i < m_count; ++i) {
        res = res % QString("%1%2").arg(sep).arg(static_cast<int>(dataAt<uint8_t>(i)), 0, 16), sep = " ";
      }
      break;
    case DataType::SByte:
      for (size_t i = 0; i < m_count; ++i) {
        res = res % QString("%1%2").arg(sep).arg(static_cast<int>(dataAt<char>(i))), sep = " ";
      }
      break;
    case DataType::Ascii:
      if (m_data.size() > 1) {
        res = res % QString::fromUtf8(dataArray<char>(), m_data.size() - 1);
      }
      break;
    case DataType::Short:
      for (size_t i = 0; i < m_count; ++i) {
        res = res % QString("%1%2").arg(sep).arg(dataAt<uint16_t>(i)), sep = " ";
      }
      break;
    case DataType::SShort:
      for (size_t i = 0; i < m_count; ++i) {
        res = res % QString("%1%2").arg(sep).arg(dataAt<int16_t>(i)), sep = " ";
      }
      break;
    case DataType::Long:
      for (size_t i = 0; i < m_count; ++i) {
        res = res % QString("%1%2").arg(sep).arg(dataAt<uint32_t>(i)), sep = " ";
      }
      break;
    case DataType::SLong:
      for (size_t i = 0; i < m_count; ++i) {
        res = res % QString("%1%2").arg(sep).arg(dataAt<int32_t>(i)), sep = " ";
      }
      break;
    case DataType::Long8:
      for (size_t i = 0; i < m_count; ++i) {
        res = res % QString("%1%2").arg(sep).arg(dataAt<uint64_t>(i)), sep = " ";
      }
      break;
    case DataType::SLong8:
      for (size_t i = 0; i < m_count; ++i) {
        res = res % QString("%1%2").arg(sep).arg(dataAt<int64_t>(i)), sep = " ";
      }
      break;
    case DataType::Rational: {
      const auto* lp = dataArray<uint32_t>();
      while (ct-- > 0) {
        if (lp[1] == 0) {
          res = res % QString("%1Nan (%2/0)").arg(sep).arg(lp[0]);
        } else {
          res = res % QString("%1%2 (%3/%4)")
                        .arg(sep)
                        .arg((static_cast<double>(lp[0]) / static_cast<double>(lp[1])))
                        .arg(lp[0])
                        .arg(lp[1]);
        }
        sep = " ";
        lp += 2;
      }
      break;
    }
    case DataType::SRational: {
      const auto* lp = dataArray<int32_t>();
      while (ct-- > 0) {
        if (lp[1] == 0) {
          res = res % QString("%1Nan (%2/0)").arg(sep).arg(lp[0]);
        } else {
          res = res % QString("%1%2 (%3/%4)")
                        .arg(sep)
                        .arg((static_cast<double>(lp[0]) / static_cast<double>(lp[1])))
                        .arg(lp[0])
                        .arg(lp[1]);
        }
        sep = " ";
        lp += 2;
      }
      break;
    }
    case DataType::Float:
      for (size_t i = 0; i < m_count; ++i) {
        res = res % QString("%1%2").arg(sep).arg(dataAt<float>(i)), sep = " ";
      }
      break;
    case DataType::Double:
      for (size_t i = 0; i < m_count; ++i) {
        res = res % QString("%1%2").arg(sep).arg(dataAt<double>(i)), sep = " ";
      }
      break;
    case DataType::IFD:
      for (size_t i = 0; i < m_count; ++i) {
        res = res % QString("%1%2").arg(sep).arg(dataAt<uint32_t>(i), 0, 16), sep = " ";
      }
      break;
    case DataType::IFD8:
      for (size_t i = 0; i < m_count; ++i) {
        res = res % QString("%1%2").arg(sep).arg(dataAt<uint64_t>(i), 0, 16), sep = " ";
      }
      break;
    default:
      break;
  }

  return res % QString(">");
}

std::string ZImgMetatag::toString() const
{
  std::string res;
  // res.reserve(256); // Pre-allocate some space to reduce reallocations

  fmt::format_to(std::back_inserter(res), "{} (tag:{}) {} {}<", m_name, m_tag, enumToString(m_dataType), m_count);

  switch (m_dataType) {
    case DataType::Undefined:
    case DataType::Byte:
      fmt::format_to(std::back_inserter(res),
                     "{}>",
                     fmt::join(dataArray<uint8_t>(), dataArray<uint8_t>() + m_count, " "));
      break;
    case DataType::SByte:
      fmt::format_to(std::back_inserter(res),
                     "{}>",
                     fmt::join(dataArray<int8_t>(), dataArray<int8_t>() + m_count, " "));
      break;
    case DataType::Ascii:
      fmt::format_to(std::back_inserter(res),
                     "{}>",
                     m_data.size() > 1 ? std::string_view(dataArray<char>(), m_data.size() - 1) : std::string_view());
      break;
    case DataType::Short:
      fmt::format_to(std::back_inserter(res),
                     "{}>",
                     fmt::join(dataArray<uint16_t>(), dataArray<uint16_t>() + m_count, " "));
      break;
    case DataType::SShort:
      fmt::format_to(std::back_inserter(res),
                     "{}>",
                     fmt::join(dataArray<int16_t>(), dataArray<int16_t>() + m_count, " "));
      break;
    case DataType::Long:
      fmt::format_to(std::back_inserter(res),
                     "{}>",
                     fmt::join(dataArray<uint32_t>(), dataArray<uint32_t>() + m_count, " "));
      break;
    case DataType::SLong:
      fmt::format_to(std::back_inserter(res),
                     "{}>",
                     fmt::join(dataArray<int32_t>(), dataArray<int32_t>() + m_count, " "));
      break;
    case DataType::Long8:
      fmt::format_to(std::back_inserter(res),
                     "{}>",
                     fmt::join(dataArray<uint64_t>(), dataArray<uint64_t>() + m_count, " "));
      break;
    case DataType::SLong8:
      fmt::format_to(std::back_inserter(res),
                     "{}>",
                     fmt::join(dataArray<int64_t>(), dataArray<int64_t>() + m_count, " "));
      break;
    case DataType::Rational: {
      const auto* lp = dataArray<uint32_t>();
      uint64_t ct = m_count;
      while (ct-- > 0) {
        if (lp[1] == 0) {
          fmt::format_to(std::back_inserter(res), " Nan ({} / 0)", lp[0]);
        } else {
          fmt::format_to(std::back_inserter(res),
                         " {} ({} / {})",
                         static_cast<double>(lp[0]) / static_cast<double>(lp[1]),
                         lp[0],
                         lp[1]);
        }
        lp += 2;
      }
      res.append(">");
      break;
    }
    case DataType::SRational: {
      const auto* lp = dataArray<int32_t>();
      uint64_t ct = m_count;
      while (ct-- > 0) {
        if (lp[1] == 0) {
          fmt::format_to(std::back_inserter(res), " Nan ({} / 0)", lp[0]);
        } else {
          fmt::format_to(std::back_inserter(res),
                         " {} ({} / {})",
                         static_cast<double>(lp[0]) / static_cast<double>(lp[1]),
                         lp[0],
                         lp[1]);
        }
        lp += 2;
      }
      res.append(">");
      break;
    }
    case DataType::Float:
      fmt::format_to(std::back_inserter(res), "{}>", fmt::join(dataArray<float>(), dataArray<float>() + m_count, " "));
      break;
    case DataType::Double:
      fmt::format_to(std::back_inserter(res),
                     "{}>",
                     fmt::join(dataArray<double>(), dataArray<double>() + m_count, " "));
      break;
    case DataType::IFD:
      fmt::format_to(std::back_inserter(res),
                     "{:#x}>",
                     fmt::join(dataArray<uint32_t>(), dataArray<uint32_t>() + m_count, " "));
      break;
    case DataType::IFD8:
      fmt::format_to(std::back_inserter(res),
                     "{:#x}>",
                     fmt::join(dataArray<uint64_t>(), dataArray<uint64_t>() + m_count, " "));
      break;
    default:
      break;
  }

  return res;
}

} // namespace nim
