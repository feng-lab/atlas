#pragma once

#include <QString>

#ifdef __clang__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-W#warnings"
#include <boost/json.hpp>
#pragma GCC diagnostic pop
#else
#include <boost/json.hpp>
#endif
namespace json = boost::json;

inline QString tag_invoke(const json::value_to_tag<QString>&, const json::value& jv)
{
  const auto& s = jv.as_string();
  return QString::fromUtf8(s.data(), s.size());
}

inline void tag_invoke(const json::value_from_tag&, json::value& jv, const QString& str)
{
  auto u8 = str.toUtf8();
  jv.emplace_string() = std::string_view(u8.data(), u8.size());
}

namespace nim {

std::string jsonToFormattedString(const json::value& jv);

template<typename T>
std::string jsonToFormattedString(const T& v)
{
  return jsonToFormattedString(json::value_from(v));
}

inline QString jsonToFormattedQString(const json::value& jv)
{
  return QString::fromStdString(jsonToFormattedString(jv));
}

template<typename T>
QString jsonToFormattedQString(const T& v)
{
  return QString::fromStdString(jsonToFormattedString(json::value_from(v)));
}

std::string jsonToString(const json::value& jv);

template<typename T>
std::string jsonToString(const T& v)
{
  return jsonToString(json::value_from(v));
}

json::object loadJsonObject(const QString& file);

void saveJsonObject(const json::object& jo, const QString& file);

void saveJsonArray(const json::array& ja, const QString& file);

inline QString asQString(const json::value& jv)
{
  return json::value_to<QString>(jv);
}

// json::value jv;
//*JsonValueProxy(jv)["user:config"]["authority"]["router"][0]["users"] = 42;

class JsonValueProxy
{
  json::value& jv_;

public:
  explicit JsonValueProxy(json::value& jv) noexcept
    : jv_(jv)
  {}

  JsonValueProxy operator[](json::string_view key) const
  {
    json::object* obj;
    if (jv_.is_null()) {
      obj = &jv_.emplace_object();
    } else {
      obj = &jv_.as_object();
    }
    return JsonValueProxy((*obj)[key]);
  }

  JsonValueProxy operator[](std::size_t index) const
  {
    json::array* arr;
    if (jv_.is_null()) {
      arr = &jv_.emplace_array();
    } else {
      arr = &jv_.as_array();
    }
    if (arr->size() <= index) {
      arr->resize(index + 1);
    }
    return JsonValueProxy((*arr)[index]);
  }

  json::value& operator*() const noexcept
  {
    return jv_;
  }
};

// json::value jv;
// JsonValuePath(jv, "user:config", "authority", "router", 0, "users") = 42;

inline json::value& JsonValuePath(json::value& jv, std::size_t index)
{
  json::array* arr;
  if (jv.is_null()) {
    arr = &jv.emplace_array();
  } else {
    arr = &jv.as_array();
  }
  if (arr->size() <= index) {
    arr->resize(index + 1);
  }
  return (*arr)[index];
}

inline json::value& JsonValuePath(json::value& jv, json::string_view key)
{
  json::object* obj;
  if (jv.is_null()) {
    obj = &jv.emplace_object();
  } else {
    obj = &jv.as_object();
  }
  return (*obj)[key];
}

template<class Arg0, class Arg1, class... Args>
json::value& JsonValuePath(json::value& jv, const Arg0& arg0, const Arg1& arg1, const Args&... args)
{
  return JsonValuePath(JsonValuePath(jv, arg0), arg1, args...);
}

} // namespace nim
