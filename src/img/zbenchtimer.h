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
    bt.recordEvent("testFun");  // optional
    ...
    testFun1();
    bt.recordEvent("testFun1");  // optional
    testFun2();
    bt.recordEvent("testFun2");  // optional
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
    LOG(INFO) << (TIMER).toString();                     \
  }

#define STOP_AND_LOG(TIMER)          \
  {                                  \
    (TIMER).stop();                  \
    LOG(INFO) << (TIMER).toString(); \
  }

#define STOP_AND_VLOG(TIMER)       \
  {                                \
    (TIMER).stop();                \
    VLOG(1) << (TIMER).toString(); \
  }

class ZBenchTimer
{
public:
  explicit ZBenchTimer(std::string funName = "Function")
    : m_name(std::move(funName))
  {
    start();
  }

  void reset()
  {
    m_best = {};
    m_worst = {};
    m_rep = 0;
    m_time = {};
    m_events = {};
  }

  void start()
  {
    m_start = std::chrono::steady_clock::now();
    m_lastEventTime = m_start;
  }

  void resetAndStart(const std::string& newName)
  {
    m_name = newName;
    reset();
    start();
  }

  void resetAndStart()
  {
    reset();
    start();
  }

  void recordEvent(std::string_view eventName);

  void stop()
  {
    auto elapsed = std::chrono::steady_clock::now() - m_start;
    m_time += elapsed;

    m_best = m_best == std::chrono::steady_clock::duration::zero() ? elapsed : std::min(m_best, elapsed);
    m_worst = std::max(m_worst, elapsed);
    m_rep++;
  }

  void setName(const std::string& str)
  {
    m_name = str;
  }

  [[nodiscard]] std::string toString() const;

protected:
  std::chrono::time_point<std::chrono::steady_clock> m_start;
  std::chrono::steady_clock::duration m_time{};
  std::chrono::steady_clock::duration m_best{};
  std::chrono::steady_clock::duration m_worst{};
  int m_rep = 0;
  std::string m_name;

  std::chrono::time_point<std::chrono::steady_clock> m_lastEventTime;
  std::string m_events;
};

} // namespace nim
