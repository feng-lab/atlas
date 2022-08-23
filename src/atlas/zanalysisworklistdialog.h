#pragma once

#include <QDialog>

class QTableView;

class QPushButton;

namespace nim {

class ZAnalysisWorklistModel;

class ZAnalysisWorklistDialog : public QDialog
{
  Q_OBJECT

public:
  explicit ZAnalysisWorklistDialog(QWidget* parent = nullptr);

protected:
  void reject() override;

  void onNew();

  void onOpen();

  void onSave();

  void onSaveAs();

  void onGenerate();

  void dataModified();

  void createWidget();

private:
  QTableView* m_view = nullptr;
  ZAnalysisWorklistModel* m_model = nullptr;
  QPushButton* m_saveButton = nullptr;
  QString m_filename;
};

} // namespace nim
