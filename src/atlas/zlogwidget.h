#pragma once

#include <QPlainTextEdit>

namespace nim {

struct LogData;

class ZLogWidget : public QPlainTextEdit
{
public:
  explicit ZLogWidget(bool receiveOldMessages = false, QWidget *parent = nullptr);

private:
  void writeLogData(const QList<LogData>* messages, int start, int end);

private:
  QTextCharFormat m_normalFormat;
  QTextCharFormat m_errorFormat;
};

} // namespace nim




