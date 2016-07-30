#ifndef ZLOGQTTYPESUPPORT_H
#define ZLOGQTTYPESUPPORT_H

#include <iosfwd>
#include <QDebug>
#include <QPoint>
#include <type_traits>

namespace nim {

template<typename T>
inline QString qtTypeToQString(const T& v)
{
  QString buffer;
  QDebug out(&buffer);
  out << v;
  return buffer;
}

inline std::ostream& operator<<(std::ostream& s, const QPoint& v)
{
  return (s << qtTypeToQString(v).toUtf8().constData());
}

inline std::ostream& operator<<(std::ostream& s, const QPointF& v)
{
  return (s << qtTypeToQString(v).toUtf8().constData());
}

template<class T>
inline std::ostream& operator<<(std::ostream& s, const QList<T>& list)
{
  return (s << qtTypeToQString(list).toUtf8().constData());
}

template<typename T>
inline std::ostream& operator<<(std::ostream& s, const QVector<T>& vec)
{
  return (s << qtTypeToQString(vec).toUtf8().constData());
}

template<typename T, typename Alloc>
inline std::ostream& operator<<(std::ostream& s, const std::vector<T, Alloc>& vec)
{
  return (s << qtTypeToQString(vec).toUtf8().constData());
}

template<typename T, typename Alloc>
inline std::ostream& operator<<(std::ostream& s, const std::list<T, Alloc>& vec)
{
  return (s << qtTypeToQString(vec).toUtf8().constData());
}

template<typename Key, typename T, typename Compare, typename Alloc>
inline std::ostream& operator<<(std::ostream& s, const std::map<Key, T, Compare, Alloc>& map)
{
  return (s << qtTypeToQString(map).toUtf8().constData());
}

template<typename Key, typename T, typename Compare, typename Alloc>
inline std::ostream& operator<<(std::ostream& s, const std::multimap<Key, T, Compare, Alloc>& map)
{
  return (s << qtTypeToQString(map).toUtf8().constData());
}

template<class Key, class T>
inline std::ostream& operator<<(std::ostream& s, const QMap<Key, T>& map)
{
  return (s << qtTypeToQString(map).toUtf8().constData());
}

template<class Key, class T>
inline std::ostream& operator<<(std::ostream& s, const QHash<Key, T>& hash)
{
  return (s << qtTypeToQString(hash).toUtf8().constData());
}

template<class T1, class T2>
inline std::ostream& operator<<(std::ostream& s, const QPair<T1, T2>& pair)
{
  return (s << qtTypeToQString(pair).toUtf8().constData());
}

template<class T1, class T2>
inline std::ostream& operator<<(std::ostream& s, const std::pair<T1, T2>& pair)
{
  return (s << qtTypeToQString(pair).toUtf8().constData());
}

template<typename T>
inline std::ostream& operator<<(std::ostream& s, const QSet<T>& set)
{
  return (s << qtTypeToQString(set).toUtf8().constData());
}

template<class T>
inline std::ostream& operator<<(std::ostream& s, const QContiguousCache<T>& cache)
{
  return (s << qtTypeToQString(cache).toUtf8().constData());
}

template<class T>
inline std::ostream& operator<<(std::ostream& s, const QSharedPointer<T>& ptr)
{
  return (s << qtTypeToQString(ptr).toUtf8().constData());
}

template<typename T>
inline typename std::enable_if<QtPrivate::IsQEnumHelper<T>::Value, std::ostream&>::Type
operator<<(std::ostream& s, T value)
{
  return (s << qtTypeToQString(value).toUtf8().constData());
}

template<typename T>
inline std::ostream& operator<<(std::ostream& s, const QFlags<T>& flags)
{
  return (s << qtTypeToQString(flags).toUtf8().constData());
}

} // namespace nim


#endif // ZLOGQTTYPESUPPORT_H
