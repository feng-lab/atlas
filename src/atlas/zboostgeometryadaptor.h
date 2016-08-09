#pragma once

#include <boost/geometry.hpp>
#include <boost/geometry/geometries/point_xy.hpp>
#include <boost/geometry/geometries/polygon.hpp>
#include <boost/geometry/geometries/register/point.hpp>
#include <boost/geometry/geometries/register/ring.hpp>
#include <QPointF>
#include <QRectF>
#include <QPolygonF>

namespace bg = boost::geometry;

// Adapt a QPointF such that it can be handled by Boost.Geometry
BOOST_GEOMETRY_REGISTER_POINT_2D_GET_SET(QPointF, double, cs::cartesian, x, y, setX, setY)

// Adapt a QPolygonF as well.
// A QPolygonF has no holes (interiors) so it is similar to a Boost.Geometry ring
BOOST_GEOMETRY_REGISTER_RING(QPolygonF)

// Register the QT rectangle. The macro(s) does not offer (yet) enough flexibility to do this in one line,
// but the traits classes do their job perfectly.
namespace boost {
namespace geometry {
namespace traits {

template<>
struct tag<QRectF>
{
  typedef box_tag type;
};
template<>
struct point_type<QRectF>
{
  typedef QPointF type;
};

template<size_t C, size_t D>
struct indexed_access<QRectF, C, D>
{
  static inline double get(const QRectF& qr)
  {
    return C == min_corner && D == 0 ? qr.x()
                                     : C == min_corner && D == 1 ? qr.y()
                                                                 : C == max_corner && D == 0 ? qr.x() + qr.width()
                                                                                             : C == max_corner && D == 1
                                                                                               ? qr.y() + qr.height()
                                                                                               : 0;
  }

  static inline void set(QRectF& qr, const double& value)
  {
    if (C == min_corner && D == 0) qr.setX(value);
    else if (C == min_corner && D == 1) qr.setY(value);
    else if (C == max_corner && D == 0) qr.setWidth(value - qr.x());
    else if (C == max_corner && D == 1) qr.setHeight(value - qr.y());
  }
};
}
}
}

#if 0
// ----------------------------------------------------------------------------------
//       boost adaptor
namespace boost { namespace geometry { namespace traits {

template<> struct tag<nim::ZVoxelCoordinate>
{
  typedef point_tag type;
};

template<> struct coordinate_type< nim::ZVoxelCoordinate >
{
  typedef nim::CoordinateType type;
};

template<> struct coordinate_system< nim::ZVoxelCoordinate >
{
  typedef cs::cartesian type;
};

template<> struct dimension< nim::ZVoxelCoordinate > : boost::mpl::int_<6>
{
  //static const std::size_t value = 6;
};
template <std::size_t I>
struct access<nim::ZVoxelCoordinate, I>
{
  static inline nim::CoordinateType get(nim::ZVoxelCoordinate const& p)
  {
    return p.get<I>();
  }

  static inline void set(nim::ZVoxelCoordinate & p, nim::CoordinateType const& v)
  {
    p.set<I>(v);
  }
};

}}}
#endif

