#include "zloadimagesequencedialog.h"

#include "zselectfilewidget.h"
#include "zimg.h"
#include "zstringutils.h"
#include "zsysteminfo.h"
#include <QVBoxLayout>
#include <QFileInfo>
#include <QKeyEvent>
#include <QFileDialog>
#include <QMessageBox>
#include <QPushButton>
#include <QProgressDialog>
#include <QLabel>

namespace nim {

ZLoadImageSequenceDialog::ZLoadImageSequenceDialog(const QString& title, QWidget* parent)
  : QDialog(parent)
  , m_catDimension("Along Dimension")
{
  QStringList filters;
  QList<nim::FileFormat> formats;
  nim::ZImg::getQtReadNameFilter(filters, formats);
  m_inputImagesFileWidget = new ZSelectFileWidget(ZSelectFileWidget::FileMode::OpenMultipleFilesWithFilter,
                                                  "Input Sequence:",
                                                  filters.join(";;"), QBoxLayout::LeftToRight);
  m_inputImagesFileWidget->setStartDirQSettingLocation(ZSystemInfo::instance().lastOpenedObjPathQSettingLocation("Image"));
  m_inputImagesFileWidget->setCompareFunc(naturalSortLessThan);

  m_catDimension.addOptions("X", "Y", "Z", "C", "T");
  m_catDimension.select("Z");

  m_buttonBox = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
  connect(m_buttonBox, &QDialogButtonBox::accepted, this, &ZLoadImageSequenceDialog::accept);
  connect(m_buttonBox, &QDialogButtonBox::rejected, this, &ZLoadImageSequenceDialog::reject);

  auto mainLayout = new QVBoxLayout;
  auto hLayout = new QHBoxLayout;
  mainLayout->addWidget(m_inputImagesFileWidget);
  hLayout->addWidget(m_catDimension.createNameLabel());
  hLayout->addWidget(m_catDimension.createWidget());
  mainLayout->addLayout(hLayout);
  mainLayout->addWidget(m_buttonBox);
  setLayout(mainLayout);

  setWindowTitle(title);
}

QStringList ZLoadImageSequenceDialog::selectedFiles()
{
  return m_inputImagesFileWidget->getSelectedMultipleOpenFiles();
}

Dimension ZLoadImageSequenceDialog::alongDimension()
{
  if (m_catDimension.isSelected("X"))
    return Dimension::X;
  else if (m_catDimension.isSelected("Y"))
    return Dimension::Y;
  else if (m_catDimension.isSelected("Z"))
    return Dimension::Z;
  else if (m_catDimension.isSelected("C"))
    return Dimension::C;
  else
    return Dimension::T;
}

} // namespace nim
