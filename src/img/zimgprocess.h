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
    if (json.contains("log_file") && json["log_file"].isString()) {
      m_logFile = json["log_file"].toString();
    }
    read(json);
  }

  void saveTask(const QString& file) const
  {
    QJsonObject json;
    if (!m_logFile.isEmpty()) {
      json["log_file"] = m_logFile;
    }
    write(json);
    saveJsonObject(json, file);
  }

  QString toQString() const
  {
    QJsonObject json;
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

  virtual void read(const QJsonObject&)
  {}

  virtual void write(QJsonObject&) const
  {}

private:
  QString m_logFile;
};

} // namespace nim

