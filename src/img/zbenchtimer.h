#pragma once

#include <chrono>
#include <string>

namespace nim {

/* usage:
  bench with repeats:
    ZBenchTimer bt;
    BENCH_AND_LOG(bt,10,5,testFun(),"fun1")
  bench without repeats:
    ZBenchTimer bt;
    testFun();
    STOP_AND_LOG(bt)
  */

#define BENCH_AND_LOG(TIMER, TRIES, REP, CODE, FUNCNAME) \
  {                                                      \
    (TIMER).reset();                                     \
    (TIMER).setName(FUNCNAME);                           \
    for (decltype(TRIES) i = 0; i < (TRIES); ++i) {      \
      (TIMER).start();                                   \
      for (decltype(REP) j = 0; j < (REP); ++j) {        \
        CODE;                                            \
      }                                                  \
      (TIMER).stop();                                    \
    }                                                    \
    VLOG(1) << (TIMER).toString();                       \
  }

#define STOP_AND_LOG(TIMER)        \
  {                                \
    (TIMER).stop();                \
    VLOG(1) << (TIMER).toString(); \
  }

class ZBenchTimer
{
public:
  explicit ZBenchTimer(std::string funName = "");

  void reset()
  {
    m_best = std::numeric_limits<double>::max();
    m_worst = -1;
    m_total = 0;
    m_rep = 0;
    m_time = 0;
    m_pauseTime = 0;
    m_totalPauseTime = 0;
    m_paused = false;
    m_average = 0;
    m_averagePauseTime = 0;
  }

  void resetAndStart(const std::string& newName)
  {
    setName(newName);
    reset();
    start();
  }

  void resetAndStart()
  {
    reset();
    start();
  }

  void start();

  void stop();

  void pause();

  void resume();

  // elapsed time in seconds
  [[nodiscard]] double time() const
  {
    return m_time;
  }

  // average elapsed time in seconds.
  [[nodiscard]] double average() const
  {
    return m_average;
  }

  // best elapsed time in seconds
  [[nodiscard]] double best() const
  {
    return m_best;
  }

  // total elapsed time in seconds.
  [[nodiscard]] double total() const
  {
    return m_total;
  }

  // elapsed pause time in seconds
  [[nodiscard]] double pauseTime() const
  {
    return m_pauseTime;
  }

  // total elapsed pause time in seconds.
  [[nodiscard]] double totalPauseTime() const
  {
    return m_totalPauseTime;
  }

  void setName(const std::string& str)
  {
    m_name = str;
  }

  [[nodiscard]] std::string toString() const;

protected:
  std::chrono::time_point<std::chrono::high_resolution_clock> m_start;
  double m_time;
  double m_best;
  double m_worst;
  double m_average;
  size_t m_rep;
  double m_total;
  std::string m_name;

  double m_pauseTime;
  double m_totalPauseTime;
  double m_averagePauseTime;
  bool m_paused;
};

} // namespace nim
