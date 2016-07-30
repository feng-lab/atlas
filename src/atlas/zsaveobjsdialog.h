#ifndef ZSAVEOBJSDIALOG_H
#define ZSAVEOBJSDIALOG_H

#include <QDialog>
#include <QList>

class QLabel;

class QTreeWidget;

class QDialogButtonBox;

namespace nim {

class ZDoc;

class ZSaveObjsDialog : public QDialog
{
Q_OBJECT
public:
  explicit ZSaveObjsDialog(const ZDoc& doc, const QList<size_t>& objs, QWidget* parent = 0);

  const QList<size_t>& objsToSave() const
  { return m_objsToSave; }

protected:
  void createWidget();

private:
  void collectObjsToSave();

  void discard();

  void updateSaveButton();

  void adjustButtonWidths();

private:
  const ZDoc& m_doc;
  QList<size_t> m_objsToSave;

  QLabel* m_label;
  QTreeWidget* m_treeWidget;
  QDialogButtonBox* m_buttonBox;
};

} // namespace nim

#endif // ZSAVEOBJSDIALOG_H
