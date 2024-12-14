#include "zimgmetatag.h"

#include <utility>
#include <cstring>

namespace nim {

ZImgMetatag::ZImgMetatag(std::string name, std::string_view value, uint32_t tag)
  : m_name(std::move(name))
  , m_tag(tag)
  , m_dataType(DataType::Ascii)
{
  setCount(value.size() + 1);
  CHECK(dataArray()[m_count - 1] == 0) << dataArray()[m_count - 1];
  std::copy_n(value.data(), value.size(), dataArray<char>());
}

ZImgMetatag::ZImgMetatag(std::string name, QStringView value, uint32_t tag)
  : m_name(std::move(name))
  , m_tag(tag)
  , m_dataType(DataType::Ascii)
{
  auto u8 = value.toUtf8();
  setCount(u8.size() + 1);
  CHECK(dataArray()[m_count - 1] == 0) << dataArray()[m_count - 1];
  std::copy_n(u8.data(), u8.size(), dataArray<char>());
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
