#ifndef ZANALYSISWORKLISTDIALOG_H
#define ZANALYSISWORKLISTDIALOG_H

#include <QDialog>

class QTableView;
class QPushButton;

namespace nim {

class ZAnalysisWorklistModel;

class ZAnalysisWorklistDialog : public QDialog
{
  Q_OBJECT
public:
  explicit ZAnalysisWorklistDialog(QWidget *parent = 0);
  virtual ~ZAnalysisWorklistDialog();

public slots:
  virtual void reject() override;

protected slots:
  void onNew();
  void onOpen();
  void onSave();
  void onSaveAs();
  void onGenerate();

  void dataModified();

protected:
  void createWidget();

private:
  QTableView* m_view;
  ZAnalysisWorklistModel* m_model;
  QPushButton* m_saveButton;
  QString m_filename;
};

} // namespace nim


#endif // ZANALYSISWORKLISTDIALOG_H
