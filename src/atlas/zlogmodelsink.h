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
#include <deque>

namespace nim {

class ZLogModelSink : public QAbstractTableModel
{
  Q_OBJECT

public:
  static const char* const Type;

  enum Column
  {
    TimeColumn = 0,
    LevelNameColumn = 1,
    MessageColumn = 2,
    FormattedMessageColumn = 100
  };

  explicit ZLogModelSink(size_t max_items = size_t(std::numeric_limits<int>::max()));

  void addEntry(const LogData& message);

  void clear();

  LogData at(size_t index);

public:
  // QAbstractTableModel overrides
  [[nodiscard]] int columnCount(const QModelIndex& parent) const override;

  [[nodiscard]] int rowCount(const QModelIndex& parent) const override;

  [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;

  [[nodiscard]] QVariant headerData(int section, Qt::Orientation orientation, int role) const override;

private:
  std::deque<LogData> m_logDatas;
  mutable QReadWriteLock m_messagesLock;
  size_t m_maxItems;
};

} // namespace nim
