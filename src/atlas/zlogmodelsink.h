// Copyright (c) 2015, Axel Gembe <axel@gembe.net>
// All rights reserved.

// Redistribution and use in source and binary forms, with or without modification,
// are permitted provided that the following conditions are met:

// * Redistributions of source code must retain the above copyright notice, this
//   list of conditions and the following disclaimer.
// * Redistributions in binary form must reproduce the above copyright notice, this
//   list of conditions and the following disclaimer in the documentation and/or other
//   materials provided with the distribution.
// * The name of the contributors may not be used to endorse or promote products
//   derived from this software without specific prior written permission.

// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
// ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
// WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
// IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
// INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
// BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
// LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
// OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED
// OF THE POSSIBILITY OF SUCH DAMAGE.

#pragma once

#include "zlog.h"

#include <QAbstractTableModel>
#include <QReadWriteLock>

#include <limits>
#include <QList>

class QTimer;

namespace nim {

class ZLogModelSink : public QAbstractTableModel, public LogSink
{
Q_OBJECT
public:
  static ZLogModelSink& instance();

  // delete copy and move constructors and assign operators
  ZLogModelSink(const ZLogModelSink&) = delete;             // Copy construct
  ZLogModelSink(ZLogModelSink&&) = delete;                  // Move construct
  ZLogModelSink& operator=(const ZLogModelSink&) = delete;  // Copy assign
  ZLogModelSink& operator=(ZLogModelSink&&) = delete;      // Move assign

  static const char* const Type;

  enum Column
  {
    TimeColumn = 0,
    LevelNameColumn = 1,
    MessageColumn = 2,
    FormattedMessageColumn = 100
  };

  void addEntry(const LogData& message);

  void clear();

  LogData at(size_t index);

protected:
  explicit ZLogModelSink(int max_items = std::numeric_limits<int>::max());

public:
  // LogSink interface
  virtual void send(LogSeverity severity, const char* full_filename, const char* base_filename, int line,
                    const tm* tm_time, const char* message, size_t prefix_len, size_t message_len) override;

  // QAbstractTableModel overrides
  virtual int columnCount(const QModelIndex& parent = QModelIndex()) const override;

  virtual int rowCount(const QModelIndex& parent = QModelIndex()) const override;

  virtual QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;

  virtual QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;

  // return the list and valid range [start, end)
  static inline std::tuple<const QList<LogData>&, int, int> logMessagesSoFar()
  {
    return std::make_tuple(std::cref(ZLogModelSink::instance().m_logDatas),
                           0, ZLogModelSink::instance().m_unsendLogDataStart);
  }

  template<typename Func1>
  static inline QMetaObject::Connection
  receiveFutureLogMessages(const typename QtPrivate::FunctionPointer<Func1>::Object* receiver, Func1 slot)
  {
    return QObject::connect(&ZLogModelSink::instance(), &ZLogModelSink::logDataReady,
                            receiver, slot,
                            Qt::QueuedConnection);
  }

private:
  void sendLogData();

signals:
  void logDataReady(const QList<LogData>* messages, int start, int end);

private:
  QList<LogData> m_logDatas;
  mutable QReadWriteLock m_messagesLock;
  int m_maxItems;
  QTimer* m_timer;
  int m_unsendLogDataStart = 0;
};

} // namespace nim

