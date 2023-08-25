#include "zbenchtimer.h"

#include <utility>

#include "zlog.h"

namespace nim {

ZBenchTimer::ZBenchTimer(std::string funName)
  : m_name(std::move(funName))
{
  reset();
  start();
}

void ZBenchTimer::start()
{
  m_time = 0.0;
  m_pauseTime = 0.0;
  m_paused = false;
  m_start = std::chrono::high_resolution_clock::now();
}

void ZBenchTimer::stop()
{
  double elapsed = std::chrono::duration<double>(std::chrono::high_resolution_clock::now() - m_start).count();
  if (m_paused) {
    m_pauseTime += elapsed;
  } else {
    m_time += elapsed;
  }

  m_paused = false;

  m_best = std::min(m_best, m_time);
  m_worst = std::max(m_worst, m_time);
  m_rep++;
  m_total += m_time;
  m_average = m_total / m_rep;
  m_totalPauseTime += m_pauseTime;
  m_averagePauseTime = m_totalPauseTime / m_rep;
}

void ZBenchTimer::pause()
{
  if (m_paused) {
    return;
  }

  m_time += std::chrono::duration<double>(std::chrono::high_resolution_clock::now() - m_start).count();
  m_paused = true;
  m_start = std::chrono::high_resolution_clock::now();
}

void ZBenchTimer::resume()
{
  if (!m_paused) {
    return;
  }

  m_pauseTime += std::chrono::duration<double>(std::chrono::high_resolution_clock::now() - m_start).count();
  m_paused = false;
  m_start = std::chrono::high_resolution_clock::now();
}

std::string ZBenchTimer::toString() const
{
  if (m_rep == 1) {
    return fmt::format("{} took {} seconds (paused {} seconds)",
                       m_name.empty() ? "Function" : m_name,
                       m_time,
                       m_pauseTime);
  } else if (m_rep > 1) {
    return fmt::format("{} took on average {} seconds (out of {} repeats, best: {} worst: {} paused on average: {})",
                       m_name.empty() ? "Function" : m_name,
                       m_average,
                       m_rep,
                       m_best,
                       m_worst,
                       m_averagePauseTime);
  }
  return {};
}

} // namespace nim
