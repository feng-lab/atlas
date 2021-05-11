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
#include "zlogmodelsink.h"
#include <QDialog>

class QModelIndex;

namespace Ui {
class LogWindow;
} // namespace Ui

namespace nim {

class ZLogFilterProxyModel;

class ZLogDialog : public QDialog
{
Q_OBJECT

public:
  explicit ZLogDialog(const LogSinkPtr& destination, QWidget* parent = nullptr);

  virtual ~ZLogDialog();

  virtual bool eventFilter(QObject* obj, QEvent* event);

private:

  void OnPauseClicked();

  void OnSaveClicked();

  void OnClearClicked();

  void OnCopyClicked();

  void OnLevelChanged(int value);

  void OnAutoScrollChanged(bool checked);

  void ModelRowsInserted(const QModelIndex& parent, int start, int last);

private:
  void copySelection() const;

  void saveSelection();

  QString getSelectionText() const;

  ZLogModelSink* mModelDestination;
  Ui::LogWindow* mUi;
  ZLogFilterProxyModel* mProxyModel;
  bool mIsPaused;
  bool mHasAutoScroll;
};

}  // namespace nim

