#include "zjson.h"

#include "zexception.h"
#include "zioutils.h"
#include <boost/json/src.hpp>

namespace {

void pretty_print(std::ostream& os, const boost::json::value& jv, std::string* indent = nullptr)
{
  std::string indent_;
  if (!indent) {
    indent = &indent_;
  }
  switch (jv.kind()) {
    case boost::json::kind::object: {
      os << "{\n";
      indent->append(4, ' ');
      const auto& obj = jv.get_object();
      if (!obj.empty()) {
        auto it = obj.begin();
        for (;;) {
          os << *indent << boost::json::serialize(it->key()) << " : ";
          pretty_print(os, it->value(), indent);
          if (++it == obj.end()) {
            break;
          }
          os << ",\n";
        }
      }
      os << "\n";
      indent->resize(indent->size() - 4);
      os << *indent << "}";
      break;
    }

    case boost::json::kind::array: {
      os << "[\n";
      indent->append(4, ' ');
      const auto& arr = jv.get_array();
      if (!arr.empty()) {
        auto it = arr.begin();
        for (;;) {
          os << *indent;
          pretty_print(os, *it, indent);
          if (++it == arr.end()) {
            break;
          }
          os << ",\n";
        }
      }
      os << "\n";
      indent->resize(indent->size() - 4);
      os << *indent << "]";
      break;
    }

    case boost::json::kind::string: {
      os << boost::json::serialize(jv.get_string());
      break;
    }

    case boost::json::kind::uint64:
    case boost::json::kind::int64:
    case boost::json::kind::double_:
      os << jv;
      break;

    case boost::json::kind::bool_:
      if (jv.get_bool()) {
        os << "true";
      } else {
        os << "false";
      }
      break;

    case boost::json::kind::null:
      os << "null";
      break;
  }

  if (indent->empty()) {
    os << "\n";
  }
}

} // namespace

namespace nim {

std::string jsonToFormattedString(const json::value& jv)
{
  std::ostringstream oss;
  pretty_print(oss, jv);
  return oss.str();
}

std::string jsonToString(const json::value& jv)
{
  std::ostringstream oss;
  oss << jv;
  return oss.str();
}

json::object loadJsonObject(const QString& file)
{
  auto fileString = readFileIntoByteArray(file);

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
  std::ofstream fs;
  openFileStream(fs, file, std::ios_base::out);
  pretty_print(fs, jo);
}

void saveJsonArray(const json::array& ja, const QString& file)
{
  std::ofstream fs;
  openFileStream(fs, file, std::ios_base::out);
  pretty_print(fs, ja);
}

} // namespace nim
