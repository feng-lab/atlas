#include "zinformationdialog.h"

#include <QDialogButtonBox>
#include <QTextBrowser>
#include <QVBoxLayout>

namespace nim {

ZInformationDialog::ZInformationDialog(QWidget* parent)
  : QDialog(parent)
{
  setWindowTitle(tr("Information"));

  auto* layout = new QVBoxLayout(this);

  m_view = new QTextBrowser(this);
  m_view->setOpenExternalLinks(true);
  m_view->setReadOnly(true);
  layout->addWidget(m_view);

  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok, this);
  connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
  layout->addWidget(buttons);

  resize(560, 360);
}

void ZInformationDialog::setText(const QString& html)
{
  m_view->setHtml(html);
}

} // namespace nim
