#pragma once

#include <QPlainTextEdit>
#include <deque>

namespace nim {

struct LogData;

class ZLogWidget : public QPlainTextEdit
{
public:
  explicit ZLogWidget(bool receiveOldMessages = false, QWidget* parent = nullptr);

private:
  void writeLogData(const std::vector<LogData>* messages, size_t start, size_t end);

private:
  QTextCharFormat m_normalFormat;
  QTextCharFormat m_errorFormat;
};

} // namespace nim
