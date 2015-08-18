#include "zloadimagesequencedialog.h"

#include <QVBoxLayout>
#include <QFileInfo>
#include <QKeyEvent>
#include <QFileDialog>
#include <QMessageBox>
#include <QPushButton>
#include <QProgressDialog>
#include "zselectfilewidget.h"
#include "zimg.h"
#include "zstringutils.h"

namespace nim {

ZLoadImageSequenceDialog::ZLoadImageSequenceDialog(const QString &title, const QString &startDir, QWidget *parent)
  : QDialog(parent)
{
  QStringList filters;
  QList<nim::FileFormat> formats;
  nim::ZImg::getQtReadNameFilter(filters, formats);
  m_inputImagesFileWidget = new ZSelectFileWidget(ZSelectFileWidget::FileMode::OpenMultipleFilesWithFilter, "Input Sequence:",
                                                  filters.join(";;"), QBoxLayout::LeftToRight, startDir);
  m_inputImagesFileWidget->setCompareFunc(naturalSortLessThan);

  m_buttonBox = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
  connect(m_buttonBox, SIGNAL(accepted()), this, SLOT(accept()));
  connect(m_buttonBox, SIGNAL(rejected()), this, SLOT(reject()));

  QVBoxLayout *mainLayout = new QVBoxLayout;
  mainLayout->addWidget(m_inputImagesFileWidget);
  mainLayout->addWidget(m_buttonBox);
  setLayout(mainLayout);

  setWindowTitle(title);
}

QStringList ZLoadImageSequenceDialog::getSelectedFiles()
{
  return m_inputImagesFileWidget->getSelectedMultipleOpenFiles();
}

} // namespace nim
