#pragma once

#include "zimgalgorithm.h"
#include "zjson.h"

namespace nim {

class ZImgProcess : public ZImgAlgorithmBaseWithProgressReporter
{
  Q_OBJECT

public:
  // log output
  inline void setLogFile(const QString& logFile)
  {
    m_logFile = logFile;
  }

  void run();

  void runInPython();

  inline void loadTask(const QString& file)
  {
    auto jo = loadJsonObject(file);
    m_logFile = json::value_to<QString>(jo.at("log_file"));
    read(jo);
  }

  inline void saveTask(const QString& file) const
  {
    json::object jo;
    jo["log_file"] = json::value_from(m_logFile);
    write(jo);
    saveJsonObject(jo, file);
  }

  [[nodiscard]] inline QString toQString() const
  {
    json::object jo;
    jo["log_file"] = json::value_from(m_logFile);
    write(jo);
    return jsonToFormattedQString(jo);
  }

  [[nodiscard]] inline std::string toString() const
  {
    json::object jo;
    jo["log_file"] = json::value_from(m_logFile);
    write(jo);
    return jsonToFormattedString(jo);
  }

Q_SIGNALS:

  void processError(QString);

  void finished();

protected:
  virtual void doWork() = 0;

  virtual void read(const json::object&) = 0;

  virtual void write(json::object&) const = 0;

private:
  QString m_logFile;
};

} // namespace nim
