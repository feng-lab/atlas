#pragma once

#include "zjson.h"
#include <iostream>
#include <limits>

namespace nim {

template<typename Point>
class ZBBox
{
public:
  // inverse box
  ZBBox()
  { reset(); }

  explicit ZBBox(const Point& minmaxCorner)
    : minCorner(minmaxCorner), maxCorner(minmaxCorner)
  {}

  ZBBox(const Point& minCorner, const Point& maxCorner)
    : minCorner(minCorner), maxCorner(maxCorner)
  {}

  ZBBox(ZBBox&&) noexcept = default;

  ZBBox& operator=(ZBBox&&) noexcept = default;

  ZBBox(const ZBBox&) = default;

  ZBBox& operator=(const ZBBox&) = default;

  inline void reset()
  {
    for (size_t i = 0; i < minCorner.length(); ++i) {
      minCorner[i] = std::numeric_limits<typename Point::value_type>::max();
      maxCorner[i] = std::numeric_limits<typename Point::value_type>::lowest();
    }
  }

  inline void setMinCorner(const Point& mc)
  { minCorner = mc; }

  inline void setMaxCorner(const Point& mc)
  { maxCorner = mc; }

  //
  [[nodiscard]] inline bool empty() const
  {
    for (size_t i = 0; i < minCorner.length(); ++i) {
      if (minCorner[i] > maxCorner[i])
        return true;
    }
    return false;
  }

  //
  inline Point size() const
  { return maxCorner - minCorner; }

  //
  inline typename Point::value_type volume() const
  {
    Point sz = size();
    typename Point::value_type res = 1;
    for (size_t i = 0; i < sz.length(); ++i) {
      res *= sz[i];
    }
    return res;
  }

  //comparison
  inline bool operator==(const ZBBox& other) const
  { return minCorner == other.minCorner && maxCorner == other.maxCorner; }

  inline bool operator!=(const ZBBox& other) const
  { return minCorner != other.minCorner || maxCorner != other.maxCorner; }

  //expand to contain
  inline void expand(const ZBBox& other)
  {
    minCorner = min(minCorner, other.minCorner);
    maxCorner = max(maxCorner, other.maxCorner);
  }

  inline void expand(const Point& other)
  {
    minCorner = min(minCorner, other);
    maxCorner = max(maxCorner, other);
  }

  inline void expand(typename Point::value_type v)
  {
    minCorner -= v;
    maxCorner += v;
  }

  inline ZBBox& operator+=(const ZBBox& other)
  {
    expand(other);
    return *this;
  }

  inline ZBBox& operator+=(const Point& other)
  {
    expand(other);
    return *this;
  }

  //intersect
  inline ZBBox& intersect(const ZBBox& other)
  {
    minCorner = max(minCorner, other.minCorner);
    maxCorner = min(maxCorner, other.maxCorner);
    return *this;
  }

  //
  inline bool contains(const ZBBox& other) const
  {
    for (size_t i = 0; i < minCorner.length(); ++i) {
      if (other.minCorner[i] < minCorner[i] || other.maxCorner[i] > maxCorner[i])
        return false;
    }
    return true;
  }

  inline bool contains(const Point& other) const
  {
    for (size_t i = 0; i < minCorner.length(); ++i) {
      if (other[i] < minCorner[i] || other[i] > maxCorner[i])
        return false;
    }
    return true;
  }

  inline bool containedBy(const ZBBox& other) const
  { return other.contains(*this); }

  // tests if bounding boxes (and points) are disjoint (empty intersection)
  inline bool disjoint(const ZBBox& other) const
  {
    const Point sz = min(other.maxCorner, maxCorner) - max(other.minCorner, minCorner);
    for (size_t i = 0; i < sz.length(); ++i) {
      if (sz[i] < 0)
        return true;
    }
    return false;
  }

  inline bool disjoint(const Point& other) const
  {
    const Point sz = min(other, maxCorner) - max(other, minCorner);
    for (size_t i = 0; i < sz.length(); ++i) {
      if (sz[i] < 0)
        return true;
    }
    return false;
  }

  // tests if bounding boxes (and points) are conjoint (non-empty intersection)
  inline bool conjoint(const ZBBox& other) const
  { return !disjoint(other); }

  inline bool conjoint(const Point& other) const
  { return !disjoint(other); }

  Point minCorner;
  Point maxCorner;
};

// merges bounding boxes and points, same as expand
template<typename T>
inline ZBBox<T> merge(const ZBBox<T>& a, const T& b)
{ return ZBBox<T>(min(a.minCorner, b), max(a.maxCorner, b)); }

template<typename T>
inline ZBBox<T> merge(const ZBBox<T>& a, const ZBBox<T>& b)
{ return ZBBox<T>(min(a.minCorner, b.minCorner), max(a.maxCorner, b.maxCorner)); }

template<typename T>
inline ZBBox<T> merge(const ZBBox<T>& a, const ZBBox<T>& b, const ZBBox<T>& c)
{ return merge(a, merge(b, c)); }

//
template<typename T>
inline ZBBox<T> intersect(const ZBBox<T>& a, const ZBBox<T>& b)
{ return ZBBox<T>(max(a.minCorner, b.minCorner), min(a.maxCorner, b.maxCorner)); }

template<typename T>
inline ZBBox<T> intersect(const ZBBox<T>& a, const ZBBox<T>& b, const ZBBox<T>& c)
{ return intersect(a, intersect(b, c)); }

//
template<typename T>
inline bool disjoint(const ZBBox<T>& a, const ZBBox<T>& b)
{ return a.disjoint(b); }

template<typename T>
inline bool disjoint(const ZBBox<T>& a, const T& b)
{ return a.disjoint(b); }

template<typename T>
inline bool disjoint(const T& a, const ZBBox<T>& b)
{ return b.disjoint(a); }

//
template<typename T>
inline bool conjoint(const ZBBox<T>& a, const ZBBox<T>& b)
{ return a.conjoint(b); }

template<typename T>
inline bool conjoint(const ZBBox<T>& a, const T& b)
{ return a.conjoint(b); }

template<typename T>
inline bool conjoint(const T& a, const ZBBox<T>& b)
{ return b.conjoint(a); }

template<typename T>
inline void tag_invoke(const json::value_from_tag&, json::value& jv, const ZBBox<T>& box)
{
  auto& jo = jv.emplace_object();
  jo["min"] = json::value_from(box.minCorner);
  jo["max"] = json::value_from(box.maxCorner);
}

template<typename T>
inline ZBBox<T> tag_invoke(const json::value_to_tag<ZBBox<T>>&, const json::value& jv)
{
  ZBBox<T> res;
  res.minCorner = json::value_to<T>(jv.at("min"));
  res.maxCorner = json::value_to<T>(jv.at("max"));
  return res;
}

} // namespace nim
