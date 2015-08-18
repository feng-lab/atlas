#ifndef ZSELECTFILEWIDGET_H
#define ZSELECTFILEWIDGET_H

#include <QWidget>
#include <QBoxLayout>
class QLabel;
class QLineEdit;
class QTextEdit;
class QToolButton;
class QPushButton;

namespace nim {

class ZSelectFileWidget : public QWidget
{
  Q_OBJECT
public:
  enum class FileMode {
    OpenSingleFile,
    OpenMultipleFiles,
    SaveFile,
    Directory,
    OpenMultipleFilesWithFilter
  };
  explicit ZSelectFileWidget(FileMode mode, const QString& guiname = QString(),
                             const QString &filter = QString(), QBoxLayout::Direction direction = QBoxLayout::LeftToRight,
                             const QString &startDir = QString(), QWidget *parent = 0);

  // This variable will be changed with the widget or you can use the get* function
  void setDestination(QString *name);
  void setDestination(QStringList *namelist);

  // for multiple files sorting in open_multiple_files mode
  void setCompareFunc(bool (*lessThan)(const QString&, const QString&));

  QString getSelectedOpenFile();
  QString getSelectedSaveFile();
  QStringList getSelectedMultipleOpenFiles();
  QString getSelectedDirectory();
  void setFile(const QString &fn);
  void setFiles(const QStringList &fl);
  
signals:
  void changed();
  
public slots:
  void selectFile();

protected slots:
  void previewFilterResult();

private:
  void createWidget(QBoxLayout::Direction direction);

  FileMode m_fileMode;
  QString m_guiName;
  QString m_filter;
  QString m_lastFName;

  QString *m_destName;
  QStringList *m_destNames;
  bool (*m_lessThan)(const QString&, const QString&);

  QStringList m_multipleFNames;

  QBoxLayout *m_layout;
  QPushButton *m_selectPushButton;
  QLabel *m_label;
  QLineEdit *m_lineEdit;
  QTextEdit *m_textEdit;
  QLineEdit *m_filterLineEdit;
  QPushButton *m_previewButton;
  QToolButton *m_button;

};

} // namespace nim

#endif // ZSELECTFILEWIDGET_H
