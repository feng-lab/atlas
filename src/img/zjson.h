#pragma once

#include "zexception.h"
#include <QStringList>
#ifdef __clang__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-W#warnings"
#define BOOST_JSON_STANDALONE
#include <boost/json.hpp>
#pragma GCC diagnostic pop
namespace json = boost::json;
#else
#define BOOST_JSON_STANDALONE
#include <boost/json.hpp>
namespace json = boost::json;
#endif
#include <vector>
#include <iosfwd>

inline void tag_invoke(const json::value_from_tag&, json::value& jv, const QString& str)
{
  jv = qUtf8Printable(str);
}

inline QString tag_invoke(const json::value_to_tag<QString>&, const json::value& jv)
{
  const auto& s = jv.as_string();
  return QString::fromUtf8(s.data(), s.size());
}

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

namespace nim {

void pretty_print(std::ostream& os, const json::value& jv, std::string* indent = nullptr);

QString formatJsonToQString(const json::value& jv);

std::string formatJsonToString(const json::value& jv);

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

