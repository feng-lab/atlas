#pragma once

#include <boost/stl_interfaces/view_interface.hpp>

namespace nim {

// A subrange is simply an iterator-sentinel pair.  This one is a bit simpler
// than the one in std::ranges; its missing a bunch of constructors, prev(),
// next(), and advance().
template<typename Iterator, typename Sentinel>
struct subrange
  : boost::stl_interfaces::view_interface<subrange<Iterator, Sentinel>>
{
  subrange() = default;
  constexpr subrange(Iterator it, Sentinel s) : first_(it), last_(s) {}

  constexpr auto begin() const { return first_; }
  constexpr auto end() const { return last_; }

private:
  Iterator first_;
  Sentinel last_;
};

} // namespace nim
