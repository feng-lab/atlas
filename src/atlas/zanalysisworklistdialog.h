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
  virtual void reject() override;

  void onNew();

  void onOpen();

  void onSave();

  void onSaveAs();

  void onGenerate();

  void dataModified();

  void createWidget();

private:
  QTableView* m_view;
  ZAnalysisWorklistModel* m_model;
  QPushButton* m_saveButton;
  QString m_filename;
};

} // namespace nim


