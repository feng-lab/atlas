#include "zselectfilewidget.h"
#include "zfileutils.h"
#include "zlog.h"

#include <QtWidgets>

namespace nim {

ZSelectFileWidget::ZSelectFileWidget(FileMode mode, const QString& guiname, const QString& filter,
                                     const QString& startDirQSettingLocation,
                                     const QString& alternativeStartDir,
                                     QBoxLayout::Direction direction, QWidget* parent)
  : QWidget(parent)
  , m_fileMode(mode)
  , m_guiName(guiname)
  , m_filter(filter)
  , m_startDirQSettingLocation(startDirQSettingLocation)
{
  //LOG(INFO) << alternativeDir;
  m_startDir = alternativeStartDir;
  if (!m_startDirQSettingLocation.isEmpty()) {
    QSettings settings;
    QString res = settings.value(m_startDirQSettingLocation).toString();
    //LOG(INFO) << res;
    if (!res.isEmpty()) {
      m_startDir = res;
    }
  }
  createWidget(direction);
}

void ZSelectFileWidget::setDestination(QString* name)
{
  if (m_fileMode != FileMode::OpenMultipleFiles && m_fileMode != FileMode::OpenMultipleFilesWithFilter)
    m_destName = name;
}

void ZSelectFileWidget::setDestination(QStringList* namelist)
{
  if (m_fileMode == FileMode::OpenMultipleFiles || m_fileMode == FileMode::OpenMultipleFilesWithFilter)
    m_destNames = namelist;
}

void ZSelectFileWidget::setCompareFunc(bool (* lessThan)(const QString&, const QString&))
{
  m_lessThan = lessThan;
}

QString ZSelectFileWidget::getSelectedOpenFile()
{
  if (m_fileMode == FileMode::OpenSingleFile)
    return m_lineEdit->text();

  return QString();
}

QString ZSelectFileWidget::getSelectedSaveFile()
{
  if (m_fileMode == FileMode::SaveFile)
    return m_lineEdit->text();

  return QString();
}

QStringList ZSelectFileWidget::getSelectedMultipleOpenFiles()
{
  if (m_fileMode == FileMode::OpenMultipleFiles) {
    return m_multipleFNames;
  } else if (m_fileMode == FileMode::OpenMultipleFilesWithFilter) {
#if 1
    QString regPattern = QRegularExpression::escape(m_filterLineEdit->text());
    regPattern.replace(QString(R"(\[)"), QString("["));
    regPattern.replace(QString(R"(\])"), QString("]"));
    regPattern.replace(QString(R"(\?)"), QString("."));
    regPattern.replace(QString(R"(\*)"), QString(".*"));
    QRegularExpression regExp(regPattern, QRegularExpression::CaseInsensitiveOption);
#else
    QRegExp regExp(m_filterLineEdit->text(), Qt::CaseInsensitive, QRegExp::Wildcard);
#endif
    return m_multipleFNames.filter(regExp);
  } else {
    return QStringList();
  }
}

QString ZSelectFileWidget::getSelectedDirectory()
{
  if (m_fileMode == FileMode::Directory)
    return m_lineEdit->text();

  return QString();
}

void ZSelectFileWidget::setFile(const QString& fn)
{
  if (m_fileMode != FileMode::OpenMultipleFiles && m_fileMode != FileMode::OpenMultipleFilesWithFilter
      && m_lineEdit->text() != fn) {
    m_lineEdit->setText(fn);
    emit changed();
  }
}

void ZSelectFileWidget::setFiles(const QStringList& fl)
{
  if (m_fileMode == FileMode::OpenMultipleFiles || m_fileMode == FileMode::OpenMultipleFilesWithFilter) {
    m_multipleFNames = fl;
    m_textEdit->setText(QString("%1").arg(fl.join("\n")));
    emit changed();
  }
}

void ZSelectFileWidget::createWidget(QBoxLayout::Direction direction)
{
  if (m_fileMode == FileMode::OpenMultipleFiles) {
    m_layout = new QBoxLayout(direction, this);
    m_layout->setContentsMargins(0, 0, 0, 0);
    m_selectPushButton = new QPushButton(m_guiName, this);
    connect(m_selectPushButton, &QPushButton::clicked, this, &ZSelectFileWidget::selectFile);
    m_textEdit = new QTextEdit(this);
    m_textEdit->setReadOnly(true);
    m_layout->addWidget(m_selectPushButton);
    m_layout->addWidget(m_textEdit);
  } else if (m_fileMode == FileMode::OpenMultipleFilesWithFilter) {
    auto lo = new QBoxLayout(direction);
    lo->setContentsMargins(0, 0, 0, 0);
    m_layout = new QBoxLayout(QBoxLayout::TopToBottom, this);
    m_layout->setContentsMargins(0, 0, 0, 0);
    m_selectPushButton = new QPushButton(m_guiName, this);
    connect(m_selectPushButton, &QPushButton::clicked, this, &ZSelectFileWidget::selectFile);
    m_textEdit = new QTextEdit(this);
    m_textEdit->setReadOnly(true);
    lo->addWidget(m_selectPushButton);
    lo->addWidget(m_textEdit);
    m_layout->addLayout(lo);
    m_label = new QLabel("Filter:", this);
    m_label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_label->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_filterLineEdit = new QLineEdit(this);
    m_filterLineEdit->setReadOnly(false);
    m_filterLineEdit->setText("*");
    m_previewButton = new QPushButton("Preview", this);
    connect(m_filterLineEdit, &QLineEdit::returnPressed, this, &ZSelectFileWidget::previewFilterResult);
    connect(m_previewButton, &QPushButton::clicked, this, &ZSelectFileWidget::previewFilterResult);
    lo = new QBoxLayout(QBoxLayout::LeftToRight);
    lo->setContentsMargins(40, 0, 0, 40);
    lo->addWidget(m_label);
    lo->addWidget(m_filterLineEdit);
    lo->addWidget(m_previewButton);
    m_layout->addLayout(lo);
  } else {
    m_layout = new QBoxLayout(direction, this);
    m_layout->setContentsMargins(0, 0, 0, 0);
    m_label = new QLabel(m_guiName, this);
    m_label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_label->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_lineEdit = new QLineEdit(this);
    m_lineEdit->setReadOnly(true);
    //LOG(INFO) << m_startDir;
    if (m_fileMode != FileMode::OpenSingleFile)
      m_lineEdit->setText(m_startDir);
    m_button = new QToolButton(this);
    m_button->setText(tr("..."));
    connect(m_button, &QToolButton::clicked, this, &ZSelectFileWidget::selectFile);
    m_layout->addWidget(m_label);
    m_layout->addWidget(m_lineEdit);
    m_layout->addWidget(m_button);
  }
}

void ZSelectFileWidget::selectFile()
{
  if (m_fileMode == FileMode::OpenMultipleFiles || m_fileMode == FileMode::OpenMultipleFilesWithFilter) {
    QStringList tmp;
    tmp = QFileDialog::getOpenFileNames(this, m_guiName, getStartDir(), m_filter,
                                        nullptr);
    if (!tmp.isEmpty()) {
      setStartDir(tmp[0]);
      m_multipleFNames.clear();
      m_multipleFNames = tmp;
      if (m_lessThan)
        std::sort(m_multipleFNames.begin(), m_multipleFNames.end(), m_lessThan);
      else
        std::sort(m_multipleFNames.begin(), m_multipleFNames.end());
      m_textEdit->setText(QString("%1").arg(m_multipleFNames.join("\n")));
      if (m_destNames)
        *m_destNames = m_multipleFNames;
      emit changed();
    }
  } else if (m_fileMode == FileMode::OpenSingleFile) {
    QString fileName = QFileDialog::getOpenFileName(
      this, m_guiName, getStartDir(), m_filter,
      nullptr);
    if (!fileName.isEmpty()) {
      setStartDir(fileName);
      m_lineEdit->setText(fileName);
      if (m_destName)
        *m_destName = fileName;
      emit changed();
    }
  } else if (m_fileMode == FileMode::SaveFile) {
    QString outputFileName = ZFileUtils::getSaveFileName(
      this, m_guiName, getStartDir(), m_filter,
      &m_selectedFilter);
    if (!outputFileName.isEmpty()) {
      setStartDir(outputFileName);
      m_lineEdit->setText(outputFileName);
      if (m_destName)
        *m_destName = outputFileName;
      emit changed();
    }
  } else if (m_fileMode == FileMode::Directory) {
    QString dir = QFileDialog::getExistingDirectory(
      this, m_guiName, getStartDir());
    if (!dir.isEmpty()) {
      setStartDir(dir);
      m_lineEdit->setText(dir);
      if (m_destName)
        *m_destName = dir;
      emit changed();
    }
  }
}

void ZSelectFileWidget::previewFilterResult()
{
  setFiles(getSelectedMultipleOpenFiles());
}

QString ZSelectFileWidget::getStartDir()
{
  if (!m_startDirQSettingLocation.isEmpty()) {
    QSettings settings;
    QString res = settings.value(m_startDirQSettingLocation).toString();
    if (!res.isEmpty()) {
      m_startDir = res;
    }
  }
  return m_startDir;
}

void ZSelectFileWidget::setStartDir(const QString& path)
{
  if (m_fileMode == FileMode::Directory) {
    m_startDir = QDir(path).canonicalPath();
  } else if (m_fileMode == FileMode::SaveFile) {
    m_startDir = path;
  } else {
    m_startDir = QFileInfo(path).canonicalPath();
  }
  //LOG(INFO) << path << m_startDir;
  if (!m_startDirQSettingLocation.isEmpty()) {
    QSettings settings;
    settings.setValue(m_startDirQSettingLocation, m_startDir);
  }
}

} // namespace nim
