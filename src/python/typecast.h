// Copyright (c) 2017-2018 Manuel Schneider

#pragma once

#include <nanobind/nanobind.h>
#include <nanobind/stl/string.h>
#include <nanobind/stl/list.h>
#include <nanobind/ndarray.h>
#include <QString>
#include <QStringList>
#include <QPolygonF>
#include "zglmutils.h"

namespace nb = nanobind;

namespace nanobind {
namespace detail {

// QString
template<> struct type_caster<QString>
{
  NB_TYPE_CASTER(QString, const_name("QString"));
  using str_caster_t = make_caster<std::string>;

  bool from_python(handle src, uint8_t flags, cleanup_list* cleanup) noexcept
  {
    str_caster_t sc;
    if (!sc.from_python(src, flags, cleanup))
      return false;
    value = QString::fromStdString(sc.operator cast_t<std::string>());
    return true;
  }

  static handle from_cpp(const QString& s, rv_policy policy, cleanup_list* cleanup) noexcept
  {
    return str_caster_t::from_cpp(s.toStdString(), policy, cleanup);
  }
};

// QList
template<>
struct type_caster<QStringList> : list_caster<QStringList, QString> {};

template<size_t L, typename T, glm::qualifier Q>
struct type_caster<glm::vec<L, T, Q>>
{
  using vec_type = glm::vec<L, T, Q>;
  NB_TYPE_CASTER(vec_type, const_name("glm_vec"));
  bool from_python(handle src, uint8_t flags, cleanup_list* cleanup) noexcept
  {
    using Arr = nb::ndarray<nb::numpy, T>;
    make_caster<Arr> ac;
    if (!ac.from_python(src, flags_for_local_caster<Arr>(flags), cleanup)) {
      return false;
    }
    auto buf = ac.operator cast_t<Arr>();
    if (buf.ndim() != 1 || buf.shape(0) != L)
      return false;
    value = vec_type();
    auto* data = buf.data();
    int64_t s0 = buf.stride(0);
    for (size_t i = 0; i < L; ++i) {
      value[i] = *(data + i * s0);
    }
    return true;
  }

  static handle from_cpp(const vec_type& v, rv_policy, cleanup_list*) noexcept
  {
    return nb::cast(nb::ndarray<nb::numpy, const T>(glm::value_ptr(v), {L})).release();
  }
};

template<size_t C, size_t R, typename T, glm::qualifier Q>
struct type_caster<glm::mat<C, R, T, Q>>
{
  using mat_type = glm::mat<C, R, T, Q>;
  NB_TYPE_CASTER(mat_type, const_name("glm_mat"));
  bool from_python(handle src, uint8_t flags, cleanup_list* cleanup) noexcept
  {
    using Arr = nb::ndarray<nb::numpy, T>;
    make_caster<Arr> ac;
    if (!ac.from_python(src, flags_for_local_caster<Arr>(flags), cleanup)) {
      return false;
    }
    auto buf = ac.operator cast_t<Arr>();
    if (buf.ndim() != 2 || buf.shape(0) != R || buf.shape(1) != C) return false;
    value = mat_type();
    auto* data = buf.data();
    int64_t s0 = buf.stride(0), s1 = buf.stride(1);
    for (size_t r = 0; r < R; ++r)
      for (size_t c = 0; c < C; ++c) {
        value[c][r] = *(data + r * s0 + c * s1);
      }
    return true;
  }

  static handle from_cpp(const mat_type& v, rv_policy, cleanup_list*) noexcept
  {
    return nb::cast(nb::ndarray<nb::numpy, const T>(glm::value_ptr(glm::transpose(v)), {R, C})).release();
  }
};

template<typename T, glm::qualifier Q>
struct type_caster<glm::tquat<T, Q>>
{
  using quat_type = glm::tquat<T, Q>;
  NB_TYPE_CASTER(quat_type, const_name("glm_quat"));
  bool from_python(handle src, uint8_t flags, cleanup_list* cleanup) noexcept
  {
    using Arr = nb::ndarray<nb::numpy, T>;
    make_caster<Arr> ac;
    if (!ac.from_python(src, flags_for_local_caster<Arr>(flags), cleanup)) {
      return false;
    }
    auto buf = ac.operator cast_t<Arr>();
    if (buf.ndim() != 1 || buf.shape(0) != 4) return false;
    value = quat_type();
    auto* data = buf.data();
    int64_t s0 = buf.stride(0);
    for (size_t i = 0; i < 4; ++i) {
      value[i] = *(data + i * s0);
    }
    return true;
  }

  static handle from_cpp(const quat_type& v, rv_policy, cleanup_list*) noexcept
  {
    return nb::cast(nb::ndarray<nb::numpy, const T>(glm::value_ptr(v), {4})).release();
  }
};

template<> struct type_caster<QPolygonF>
{
  NB_TYPE_CASTER(QPolygonF, const_name("QPolygonF"));

  static_assert(std::is_standard_layout_v<QPolygonF::value_type> && sizeof(QPolygonF::value_type) == 2 * sizeof(double), "need simple layout");

  bool from_python(handle src, uint8_t flags, cleanup_list* cleanup) noexcept
  {
    using Arr = nb::ndarray<nb::numpy, double>;
    make_caster<Arr> ac;
    if (!ac.from_python(src, flags_for_local_caster<Arr>(flags), cleanup)) {
      return false;
    }
    auto buf = ac.operator cast_t<Arr>();
    if (buf.ndim() != 2 || buf.shape(1) != 2) return false;
    value = QPolygonF();
    value.resize(buf.shape(0));
    auto* data = buf.data();
    int64_t s0 = buf.stride(0), s1 = buf.stride(1);
    for (ssize_t r = 0; r < (ssize_t) buf.shape(0); ++r) {
      value[r].setX(*(data + r * s0 + 0 * s1));
      value[r].setY(*(data + r * s0 + 1 * s1));
    }
    return true;
  }

  static handle from_cpp(const QPolygonF& v, rv_policy, cleanup_list*) noexcept
  {
    if (v.empty())
      return nb::cast(nb::ndarray<nb::numpy, const double>(nullptr, {(size_t)0, (size_t)2})).release();
    auto ptr = reinterpret_cast<const double*>(v.data());
    return nb::cast(nb::ndarray<nb::numpy, const double>(ptr, {(size_t)v.size(), (size_t)2})).release();
  }
};

} // namespace
} // namespace
