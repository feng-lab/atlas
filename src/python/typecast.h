// Copyright (c) 2017-2018 Manuel Schneider

#pragma once

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <pybind11/numpy.h>
#include <QString>
#include <QStringList>
#include "zglmutils.h"

namespace py = pybind11;

namespace pybind11 {
namespace detail {

// QString
template<> struct type_caster<QString>
{
  PYBIND11_TYPE_CASTER(QString, _("str[QString]"));
private:
  using str_caster_t = make_caster<std::string>;
  str_caster_t str_caster;
public:
  bool load(handle src, bool convert)
  {
    if (str_caster.load(src, convert)) {
      value = QString::fromStdString(str_caster);
      return true;
    }
    return false;
  }

  static handle cast(const QString& s, return_value_policy policy, handle parent)
  {
    return str_caster_t::cast(s.toStdString(), policy, parent);
  }
};

// QList
template<>
struct type_caster<QStringList> : list_caster<QStringList, QString> {};

template<size_t L, typename T, glm::qualifier Q>
struct type_caster<glm::vec<L, T, Q>>
{
  using vec_type = glm::vec<L, T, Q>;
  PYBIND11_TYPE_CASTER(vec_type, _("np.ndarray[") + npy_format_descriptor_name<T>::name + _<L>() + _("]"));
  bool load(handle src, bool convert)
  {
    // If we're in no-convert mode, only load if given an array of the correct type
    if (!convert && !isinstance<array_t<T>>(src)) return false;

    // Coerce into an array, but don't do type conversion yet; the copy below handles it.
    auto buf = array_t<T>::ensure(src);

    if (!buf) return false;

    auto dims = buf.ndim();
    if (dims != 1 || buf.shape(0) != L) return false;

    // Allocate the new type, then build a numpy reference into it
    value = vec_type();
    for (size_t i = 0; i < L; ++i) {
      value[i] = *buf.data(i);
    }
    return true;
  }

  static handle cast(const vec_type& v, return_value_policy, handle)
  {
    return array(L, glm::value_ptr(v)).release();
  }
};

template<size_t C, size_t R, typename T, glm::qualifier Q>
struct type_caster<glm::mat<C, R, T, Q>>
{
  using mat_type = glm::mat<C, R, T, Q>;
  PYBIND11_TYPE_CASTER(mat_type, _("np.ndarray[") + npy_format_descriptor_name<T>::name + _<R>() + _("x") + _<C>() + _("]"));
  bool load(handle src, bool convert)
  {
    // If we're in no-convert mode, only load if given an array of the correct type
    if (!convert && !isinstance<array_t<T>>(src)) return false;

    // Coerce into an array, but don't do type conversion yet; the copy below handles it.
    auto buf = array_t<T>::ensure(src);

    if (!buf) return false;

    auto dims = buf.ndim();
    if (dims != 2 || buf.shape(0) != R || buf.shape(1) != C) return false;

    // Allocate the new type, then build a numpy reference into it
    value = mat_type();
    auto b = buf.template unchecked<2>(); // x must have ndim = 2; can be non-writeable
    for (size_t c = 0; c < C; ++c) {
      for (size_t r = 0; r < R; ++r) {
        value[c][r] = b(r, c);
      }
    }
    return true;
  }

  static handle cast(const mat_type& v, return_value_policy, handle)
  {
    return array({R, C}, {}, glm::value_ptr(glm::transpose(v))).release();
  }
};

template<typename T, glm::qualifier Q>
struct type_caster<glm::tquat<T, Q>>
{
  using quat_type = glm::tquat<T, Q>;
  PYBIND11_TYPE_CASTER(quat_type, _("np.ndarray[") + npy_format_descriptor_name<T>::name + _("_quat4]"));
  bool load(handle src, bool convert)
  {
    // If we're in no-convert mode, only load if given an array of the correct type
    if (!convert && !isinstance<array_t<T>>(src)) return false;

    // Coerce into an array, but don't do type conversion yet; the copy below handles it.
    auto buf = array_t<T>::ensure(src);

    if (!buf) return false;

    auto dims = buf.ndim();
    if (dims != 1 || buf.shape(0) != 4) return false;

    // Allocate the new type, then build a numpy reference into it
    value = quat_type();
    for (size_t i = 0; i < 4; ++i) {
      value[i] = *buf.data(i);
    }
    return true;
  }

  static handle cast(const quat_type& v, return_value_policy, handle)
  {
    return array(4, glm::value_ptr(v)).release();
  }
};

} // namespace
} // namespace


