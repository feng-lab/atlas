#pragma once

#include <boost/functional/hash.hpp>

namespace nim {

template<typename K>
struct ZHashCompare
{
  static size_t hash(const K& key)
  {
    return m_hasher(key);
  }

  static bool equal(const K& key1, const K& key2)
  {
    return key1 == key2;
  }

private:
  static constexpr auto m_hasher = boost::hash<K>{};
};

} // namespace nim
