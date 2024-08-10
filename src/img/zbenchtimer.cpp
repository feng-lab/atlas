#include "zbenchtimer.h"

#include <fmt/chrono.h>

#include "zlog.h"

namespace nim {

static_assert(std::ratio_less_equal_v<std::chrono::steady_clock::period, std::micro>,
              "Current C++ compiler doesn't provide a precise enough steady_clock.");

void ZBenchTimer::recordEvent(const std::string& eventName)
{
  auto now = std::chrono::steady_clock::now();
  fmt::format_to(std::back_inserter(m_events),
                 "  {:>30} : {:>16.6} {:>16.6}\n",
                 eventName,
                 std::chrono::duration<double>(now - m_lastEventTime),
                 std::chrono::duration<double>(now - m_start));
  m_lastEventTime = now;
}

std::string ZBenchTimer::toString() const
{
  if (m_rep == 1) {
    if (m_events.empty()) {
      return fmt::format("{} took {}", m_name, std::chrono::duration<double>(m_time));
    }
    return fmt::format("{} took {} :\n                           Event :       Since Last      Since Start\n{}",
                       m_name,
                       std::chrono::duration<double>(m_time),
                       m_events);
  } else if (m_rep > 1) {
    return fmt::format("{} took on average {} (out of {} repeats, best: {} worst: {})",
                       m_name,
                       std::chrono::duration<double>(m_time) / m_rep,
                       m_rep,
                       m_best,
                       m_worst);
  }
  return {};
}

} // namespace nim
