#pragma once

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
  enum class FileMode
  {
    OpenSingleFile,
    OpenMultipleFiles,
    SaveFile,
    Directory,
    OpenMultipleFilesWithFilter
  };

  // if set, last opened dir will be retrieved from QSetting, and if QSetting is empty, alternativeDir will be used
  // first parameter can be an empty string, then second parameter will be used to init startDir
  explicit ZSelectFileWidget(FileMode mode, const QString& guiname = QString(),
                             const QString& filter = QString(),
                             const QString& startDirQSettingLocation = QString(),
                             const QString& alternativeStartDir = QString(),
                             QBoxLayout::Direction direction = QBoxLayout::LeftToRight,
                             QWidget* parent = nullptr);

  // This variable will be changed with the widget or you can use the get* function
  void setDestination(QString* name);

  void setDestination(QStringList* namelist);

  // for multiple files sorting in open_multiple_files mode
  void setCompareFunc(bool (* lessThan)(const QString&, const QString&));

  QString getSelectedOpenFile();

  QString getSelectedSaveFile();

  QStringList getSelectedMultipleOpenFiles();

  QString getSelectedDirectory();

  void setFile(const QString& fn);

  void setFiles(const QStringList& fl);

  inline void setSelectedFilter(const QString& f)
  { m_selectedFilter = f; }

  inline QString selectedFilter() const
  { return m_selectedFilter; }

signals:

  void changed();

private:
  void selectFile();

  void previewFilterResult();

  void createWidget(QBoxLayout::Direction direction);

  QString getStartDir();

  void setStartDir(const QString& path);

private:
  FileMode m_fileMode;
  QString m_guiName;
  QString m_filter;
  QString m_startDirQSettingLocation;
  QString m_startDir;

  QString* m_destName = NULL;
  QStringList* m_destNames = nullptr;

  bool (* m_lessThan)(const QString&, const QString&) = nullptr;

  QStringList m_multipleFNames;

  QBoxLayout* m_layout;
  QPushButton* m_selectPushButton;
  QLabel* m_label;
  QLineEdit* m_lineEdit;
  QTextEdit* m_textEdit;
  QLineEdit* m_filterLineEdit;
  QPushButton* m_previewButton;
  QToolButton* m_button;

  QString m_selectedFilter;
};

} // namespace nim

