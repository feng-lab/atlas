#pragma once

#include "zimgalgorithm.h"
#include "zjson.h"

namespace nim {

class ZImgProcess : public ZImgAlgorithm
{
public:
  // log output
  void setLogFile(const QString& logFile)
  {
    m_logFile = logFile;
  }

  // Runs the algorithm synchronously. On error, throws ZException. On cancellation, throws ZCancellationException.
  void run();

  void loadTask(const QString& file)
  {
    auto jo = loadJsonObject(file);
    m_logFile = json::value_to<QString>(jo.at("log_file"));
    read(jo);
  }

  void saveTask(const QString& file) const
  {
    json::object jo;
    jo["log_file"] = json::value_from(m_logFile);
    write(jo);
    saveJsonObject(jo, file);
  }

  [[nodiscard]] std::string toString() const
  {
    json::object jo;
    jo["log_file"] = json::value_from(m_logFile);
    write(jo);
    return jsonToFormattedString(jo);
  }

protected:
  virtual void doWork() = 0;

  virtual void read(const json::object&) = 0;

  virtual void write(json::object&) const = 0;

private:
  QString m_logFile;
};

} // namespace nim
