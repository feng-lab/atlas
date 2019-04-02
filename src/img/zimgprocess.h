#pragma once

#include "zimgalgorithm.h"
#include "zjson.h"

namespace nim {

class ZImgProcess : public ZImgAlgorithmBaseWithProgressReporter
{
Q_OBJECT
public:

  // log output
  void setLogFile(const QString& logFile)
  { m_logFile = logFile; }

  void run();

  void runInPython();

  void loadTask(const QString& file)
  {
    QJsonObject json = loadJsonObject(file);
    m_logFile = readString(json, "log_file");
    read(json);
  }

  void saveTask(const QString& file) const
  {
    QJsonObject json;
    json["log_file"] = m_logFile;
    write(json);
    saveJsonObject(json, file);
  }

  QString toQString() const
  {
    QJsonObject json;
    json["log_file"] = m_logFile;
    write(json);
    QJsonDocument jsonDoc(json);
    return jsonDoc.toJson(QJsonDocument::Indented);
  }

signals:

  void canceled();

  void processError(QString);

  void finished();

protected:
  virtual void doWork() = 0;

  virtual void read(const QJsonObject&) = 0;

  virtual void write(QJsonObject&) const = 0;

private:
  QString m_logFile;
};

} // namespace nim

