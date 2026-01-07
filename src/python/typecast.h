// Copyright (c) 2017-2018 Manuel Schneider

#pragma once

#include <nanobind/nanobind.h>
#include <nanobind/stl/string.h>
#include <nanobind/stl/list.h>
#include <nanobind/ndarray.h>
#include <QString>
#include <QStringList>
#include <array>
#include <cstring>
#include "zglmutils.h"

namespace nb = nanobind;

namespace nanobind {
namespace detail {

// QString
template<>
struct type_caster<QString>
{
  NB_TYPE_CASTER(QString, const_name("QString"));
  using str_caster_t = make_caster<std::string>;

  bool from_python(handle src, uint8_t flags, cleanup_list* cleanup) noexcept
  {
    str_caster_t sc;
    if (!sc.from_python(src, flags, cleanup)) {
      return false;
    }
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
struct type_caster<QStringList> : list_caster<QStringList, QString>
{};

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
    if (buf.ndim() != 1 || buf.shape(0) != L) {
      return false;
    }
    value = vec_type();
    auto* data = buf.data();
    int64_t s0 = buf.stride(0);
    for (size_t i = 0; i < L; ++i) {
      value[i] = *(data + i * s0);
    }
    return true;
  }

  static handle from_cpp(const vec_type& v, rv_policy, cleanup_list* cleanup) noexcept
  {
    // `type_caster<...>::from_cpp` must be `noexcept`. Avoid `nb::cast(...)` here
    // since it may throw (e.g., if numpy is unavailable), which would `terminate`.
    try {
      const std::array<T, L> buffer = [&] {
        std::array<T, L> tmp{};
        std::memcpy(tmp.data(), glm::value_ptr(v), sizeof(T) * L);
        return tmp;
      }();

      nb::ndarray<nb::numpy, const T> array(buffer.data(), {L});
      return type_caster<decltype(array)>::from_cpp(array, rv_policy::copy, cleanup);
    }
    catch (nanobind::python_error& e) {
      e.restore();
      return nullptr;
    }
    catch (const std::exception& e) {
      if (!PyErr_Occurred()) {
        PyErr_SetString(PyExc_RuntimeError, e.what());
      }
      return nullptr;
    }
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
    if (buf.ndim() != 2 || buf.shape(0) != R || buf.shape(1) != C) {
      return false;
    }
    value = mat_type();
    auto* data = buf.data();
    int64_t s0 = buf.stride(0), s1 = buf.stride(1);
    for (size_t r = 0; r < R; ++r) {
      for (size_t c = 0; c < C; ++c) {
        value[c][r] = *(data + r * s0 + c * s1);
      }
    }
    return true;
  }

  static handle from_cpp(const mat_type& v, rv_policy, cleanup_list* cleanup) noexcept
  {
    // `type_caster<...>::from_cpp` must be `noexcept`. Avoid `nb::cast(...)` here
    // since it may throw (e.g., if numpy is unavailable), which would `terminate`.
    try {
      std::array<T, C * R> buffer{};
      for (size_t r = 0; r < R; ++r) {
        for (size_t c = 0; c < C; ++c) {
          buffer[r * C + c] = v[c][r];
        }
      }

      nb::ndarray<nb::numpy, const T> array(buffer.data(), {R, C});
      return type_caster<decltype(array)>::from_cpp(array, rv_policy::copy, cleanup);
    }
    catch (nanobind::python_error& e) {
      e.restore();
      return nullptr;
    }
    catch (const std::exception& e) {
      if (!PyErr_Occurred()) {
        PyErr_SetString(PyExc_RuntimeError, e.what());
      }
      return nullptr;
    }
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
    if (buf.ndim() != 1 || buf.shape(0) != 4) {
      return false;
    }
    value = quat_type();
    auto* data = buf.data();
    int64_t s0 = buf.stride(0);
    for (size_t i = 0; i < 4; ++i) {
      value[i] = *(data + i * s0);
    }
    return true;
  }

  static handle from_cpp(const quat_type& v, rv_policy, cleanup_list* cleanup) noexcept
  {
    // `type_caster<...>::from_cpp` must be `noexcept`. Avoid `nb::cast(...)` here
    // since it may throw (e.g., if numpy is unavailable), which would `terminate`.
    try {
      std::array<T, 4> buffer{};
      std::memcpy(buffer.data(), glm::value_ptr(v), sizeof(T) * 4);
      nb::ndarray<nb::numpy, const T> array(buffer.data(), {4});
      return type_caster<decltype(array)>::from_cpp(array, rv_policy::copy, cleanup);
    }
    catch (nanobind::python_error& e) {
      e.restore();
      return nullptr;
    }
    catch (const std::exception& e) {
      if (!PyErr_Occurred()) {
        PyErr_SetString(PyExc_RuntimeError, e.what());
      }
      return nullptr;
    }
  }
};

} // namespace detail
} // namespace nanobind
