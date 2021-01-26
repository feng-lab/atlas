// Copyright (c) 2017-2018 Manuel Schneider

#pragma once

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <QString>
#include <QStringList>
#include "zglmutils.h"

namespace py = pybind11;

namespace pybind11 {
namespace detail {

#if 1

// QString
template<> struct type_caster<QString>
{
  PYBIND11_TYPE_CASTER(QString, _("QString"));
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


//template <> struct type_caster<QString> {
//public:
//    PYBIND11_TYPE_CASTER(QString, _("str"));
//    bool load(handle src, bool) {
//        PyObject *source = src.ptr();
//        if (!PyUnicode_Check(source)) return false;
//        value = PyUnicode_AsUTF8(source);
//        return true;
//    }
//    static handle cast(QString src, return_value_policy /* policy */, handle /* parent */) {
//        return PyUnicode_FromString(src.toUtf8().constData());
//    }
//};

#else

/* Create a TypeCaster for auto python string <--> QString conversion */
template <> struct type_caster<QString>
{
public:
  /**
* This macro establishes the name 'QString' in
* function signatures and declares a local variable
* 'value' of type QString
*/
  PYBIND11_TYPE_CASTER(QString, _("QString"));

  /**
*  @brief Conversion part 1 (Python->C++): convert a PyObject into a QString
* instance or return false upon failure. The second argument
* indicates whether implicit conversions should be applied.
* @param src
* @return boolean
*/
  bool load(handle src, bool)
  {
    if(!src) {
      return false;
    }
    object temp;
    handle load_src = src;
    if(PyUnicode_Check(load_src.ptr())) {
      temp = reinterpret_steal<object>(PyUnicode_AsUTF8String(load_src.ptr()));
      if(!temp) { /* A UnicodeEncodeError occured */
        PyErr_Clear();
        return false;
      }
      load_src = temp;
    }
    char* buffer = nullptr;
    ssize_t length = 0;
    int err = PYBIND11_BYTES_AS_STRING_AND_SIZE(load_src.ptr(), &buffer, &length);
    if(err == -1) { /* A TypeError occured */
      PyErr_Clear();
      return false;
    }
    value = QString::fromUtf8(buffer, static_cast<int>(length));
    return true;
  }

  /**
   * @brief Conversion part 2 (C++ -> Python): convert an QString instance into
   * a Python object. The second and third arguments are used to
   * indicate the return value policy and parent object (for
   * ``return_value_policy::reference_internal``) and are generally
   * ignored by implicit casters.
   *
   * @param src
   * @return
   */
  static handle cast(const QString& src, return_value_policy /* policy */, handle /* parent */)
  {
    assert(sizeof(QChar) == 2);
    return PyUnicode_FromKindAndData(PyUnicode_2BYTE_KIND, src.constData(), src.length());
  }
};

#endif

// QList
template<>
struct type_caster<QStringList> : list_caster<QStringList, QString> {};

//template<size_t L, typename T, glm::qualifier Q>
//struct type_caster<glm::vec<L, T, Q>> : array_caster<glm::vec<L, T, Q>, T, false, L> {};
//
//template<size_t C, size_t R, typename T, glm::qualifier Q>
//struct type_caster<glm::mat<C, R, T, Q>> : array_caster<glm::mat<C, R, T, Q>, T, false, C> {};
//
//template<typename T, glm::qualifier Q>
//struct type_caster<glm::tquat<T, Q>> : array_caster<glm::tquat<T, Q>, T, false, 4> {};

} // namespace
} // namespace


