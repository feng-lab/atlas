#include "zjson.h"

#include "zioutils.h"
#include <boost/charconv/to_chars.hpp>
#include <boost/json/serializer.hpp>
#include <boost/json/src.hpp>
#include <algorithm>
#include <cmath>
#include <ostream>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

constexpr size_t kIndentWidth = 4;
constexpr size_t kSerializerBufferSize = 4096;

class PrettyJsonPrinter
{
public:
  explicit PrettyJsonPrinter(std::string& out)
    : m_stringOut(&out)
  {}

  explicit PrettyJsonPrinter(std::ostream& out)
    : m_streamOut(&out)
  {}

  void print(const boost::json::value& value)
  {
    printValue(value, 0);
    append('\n');
  }

  void print(const boost::json::object& object)
  {
    printObject(object, 0);
    append('\n');
  }

  void print(const boost::json::array& array)
  {
    printArray(array, 0);
    append('\n');
  }

private:
  void append(std::string_view text)
  {
    if (m_stringOut != nullptr) {
      m_stringOut->append(text);
      return;
    }

    m_streamOut->write(text.data(), static_cast<std::streamsize>(text.size()));
  }

  void append(char ch)
  {
    if (m_stringOut != nullptr) {
      m_stringOut->push_back(ch);
      return;
    }

    m_streamOut->put(ch);
  }

  void appendIndent(size_t depth)
  {
    size_t remaining = depth * kIndentWidth;
    constexpr std::string_view spaces = "                                ";
    while (remaining > 0) {
      const size_t count = std::min(remaining, spaces.size());
      append(spaces.substr(0, count));
      remaining -= count;
    }
  }

  void appendEscapedString(boost::json::string_view text)
  {
    m_serializer.reset(text);
    while (!m_serializer.done()) {
      const auto chunk = m_serializer.read(m_serializerBuffer.data(), m_serializerBuffer.size());
      append(std::string_view(chunk.data(), chunk.size()));
    }
  }

  template<typename T>
  void appendInteger(T value)
  {
    char buffer[32];
    const auto result = boost::charconv::to_chars(buffer, buffer + sizeof(buffer), value);
    if (result.ec == std::errc{}) {
      append(std::string_view(buffer, static_cast<size_t>(result.ptr - buffer)));
      return;
    }

    append(boost::json::serialize(boost::json::value(value)));
  }

  void appendDouble(double value)
  {
    if (!std::isfinite(value)) {
      if (std::isnan(value)) {
        append("null");
      } else {
        append(std::signbit(value) ? "-1e99999" : "1e99999");
      }
      return;
    }

    char buffer[128];
    const auto result =
      boost::charconv::to_chars(buffer, buffer + sizeof(buffer), value, boost::charconv::chars_format::general);
    if (result.ec == std::errc{}) {
      const std::string_view text(buffer, static_cast<size_t>(result.ptr - buffer));
      append(text);
      if (text.find_first_of(".eE") == std::string_view::npos) {
        append(".0");
      }
      return;
    }

    append(boost::json::serialize(boost::json::value(value)));
  }

  void printValue(const boost::json::value& value, size_t depth)
  {
    switch (value.kind()) {
      case boost::json::kind::object:
        printObject(value.get_object(), depth);
        break;

      case boost::json::kind::array:
        printArray(value.get_array(), depth);
        break;

      case boost::json::kind::string:
        appendEscapedString(value.get_string());
        break;

      case boost::json::kind::uint64:
        appendInteger(value.get_uint64());
        break;

      case boost::json::kind::int64:
        appendInteger(value.get_int64());
        break;

      case boost::json::kind::double_:
        appendDouble(value.get_double());
        break;

      case boost::json::kind::bool_:
        append(value.get_bool() ? "true" : "false");
        break;

      case boost::json::kind::null:
        append("null");
        break;
    }
  }

  void printObject(const boost::json::object& object, size_t depth)
  {
    append("{\n");
    if (!object.empty()) {
      auto it = object.begin();
      for (;;) {
        appendIndent(depth + 1);
        appendEscapedString(it->key());
        append(" : ");
        printValue(it->value(), depth + 1);
        if (++it == object.end()) {
          break;
        }
        append(",\n");
      }
    }
    append('\n');
    appendIndent(depth);
    append('}');
  }

  void printArray(const boost::json::array& array, size_t depth)
  {
    append("[\n");
    if (!array.empty()) {
      auto it = array.begin();
      for (;;) {
        appendIndent(depth + 1);
        printValue(*it, depth + 1);
        if (++it == array.end()) {
          break;
        }
        append(",\n");
      }
    }
    append('\n');
    appendIndent(depth);
    append(']');
  }

  std::string* m_stringOut = nullptr;
  std::ostream* m_streamOut = nullptr;
  boost::json::serializer m_serializer;
  std::vector<char> m_serializerBuffer = std::vector<char>(kSerializerBufferSize);
};

} // namespace

namespace nim {

std::string jsonToFormattedString(const json::value& jv)
{
  std::string out;
  PrettyJsonPrinter printer(out);
  printer.print(jv);
  return out;
}

std::string jsonToString(const json::value& jv)
{
  return json::serialize(jv);
}

json::object loadJsonObject(const QString& file)
{
  // do not use readFileIntoString as the file might be qt specific
  auto fileString = readFileIntoQByteArray(file);
  if (fileString.isEmpty()) {
    return {};
  }

  json::parse_options opt; // all extensions default to off
  // opt.numbers = json::number_precision::precise;
  opt.allow_comments = true; // permit C and C++ style comments to appear in whitespace
  opt.allow_trailing_commas = true; // allow an additional trailing comma in object and array element lists
  opt.allow_infinity_and_nan = true;

  auto jv = json::parse(json::string_view(fileString.data(), fileString.size()), json::storage_ptr(), opt);
  return jv.as_object();
}

void saveJsonObject(const json::object& jo, const QString& file)
{
  auto fs = openOFStream(file, std::ios_base::out);
  PrettyJsonPrinter printer(fs);
  printer.print(jo);
}

void saveJsonArray(const json::array& ja, const QString& file)
{
  auto fs = openOFStream(file, std::ios_base::out);
  PrettyJsonPrinter printer(fs);
  printer.print(ja);
}

} // namespace nim
