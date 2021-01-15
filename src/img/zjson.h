#pragma once

#include "zexception.h"

#ifdef __clang__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-W#warnings"
#define BOOST_JSON_STANDALONE
#include <boost/json.hpp>
#pragma GCC diagnostic pop
#else
#define BOOST_JSON_STANDALONE
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
  jv.emplace_string() = qUtf8Printable(str);
}

#if 0
inline void tag_invoke(const json::value_from_tag&, json::value& jv, const QStringList& sl)
{
  auto& sa = jv.emplace_array();
  sa.reserve(sl.size());
  for (size_t i = 0; i < sa.size(); ++i) {
    sa.emplace_back(qUtf8Printable(sl[i]));
  }
}

inline QStringList tag_invoke(const json::value_to_tag<QStringList>&, const json::value& jv)
{
  QStringList res;
  for (const auto& sv : jv.as_array()) {
    const auto& s = sv.as_string();
    res.push_back(QString::fromUtf8(s.data(), s.size()));
  }
  return res;
}
#endif

namespace nim {

QString jsonToFormattedQString(const json::value& jv);

std::string jsonToFormattedString(const json::value& jv);

template<typename T>
inline QString jsonToFormattedQString(const T& v)
{
  return jsonToFormattedQString(json::value_from(v));
}

template<typename T>
inline std::string jsonToFormattedString(const T& v)
{
  return jsonToFormattedString(json::value_from(v));
}

QString jsonToQString(const json::value& jv);

std::string jsonToString(const json::value& jv);

template<typename T>
inline QString jsonToQString(const T& v)
{
  return jsonToQString(json::value_from(v));
}

template<typename T>
inline std::string jsonToString(const T& v)
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

// tuple-like types
template<class T,
  typename std::enable_if<(std::tuple_size<json::detail::remove_cvref<T>>::value > 0)>::type* = nullptr>
inline T tag_invoke(const json::value_to_tag<T>&, const json::value& jv)
{
  constexpr std::size_t n = std::tuple_size<json::detail::remove_cvref<T>>::value;
  const auto& ja = jv.as_array();
  if (ja.size() < n) {
    throw ZIOException("json array too short");
  }
  T res;
  for (size_t i = 0; i < n; ++i) {
    res[i] = json::value_to<typename T::value_type>(ja[i]);
  }
  return res;
}

//json::value jv;
//*JsonValueProxy(jv)["user:config"]["authority"]["router"][0]["users"] = 42;

class JsonValueProxy
{
  json::value& jv_;

public:
  explicit JsonValueProxy(json::value& jv) noexcept
    : jv_(jv)
  {
  }

  inline JsonValueProxy operator[](json::string_view key)
  {
    json::object* obj;
    if (jv_.is_null()) {
      obj = &jv_.emplace_object();
    } else {
      obj = &jv_.as_object();
    }
    return JsonValueProxy((*obj)[key]);
  }

  inline JsonValueProxy operator[](std::size_t index)
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

  json::value& operator*() noexcept
  {
    return jv_;
  }
};

//json::value jv;
//JsonValuePath(jv, "user:config", "authority", "router", 0, "users") = 42;

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
inline json::value& JsonValuePath(json::value& jv, Arg0 const& arg0, Arg1 const& arg1, Args const& ... args)
{
  return JsonValuePath(JsonValuePath(jv, arg0), arg1, args...);
}

} // namespace nim

namespace glm {

// tuple-like types
template<class T,
  typename std::enable_if<(std::tuple_size<json::detail::remove_cvref<T>>::value > 0)>::type* = nullptr>
inline T tag_invoke(const json::value_to_tag<T>&, const json::value& jv)
{
  constexpr std::size_t n = std::tuple_size<json::detail::remove_cvref<T>>::value;
  const auto& ja = jv.as_array();
  if (ja.size() < n) {
    throw nim::ZIOException("json array too short");
  }
  T res;
  for (size_t i = 0; i < n; ++i) {
    res[i] = json::value_to<typename T::value_type>(ja[i]);
  }
  return res;
}

} // namespace glm

