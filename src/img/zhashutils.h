#pragma once

#include <boost/functional/hash.hpp>

namespace nim {

template<typename K>
struct ZHashCompare
{
  static size_t hash(const K& key)
  {
    return boost::hash<K>{}(key);
  }

  static bool equal(const K& key1, const K& key2)
  {
    return key1 == key2;
  }
};

} // namespace nim
