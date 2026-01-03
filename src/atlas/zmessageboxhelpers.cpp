#include "zmessageboxhelpers.h"

#include <QApplication>
#include <QCoreApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QSize>
#include <QSizePolicy>
#include <QStyle>
#include <QTextOption>
#include <QVBoxLayout>
#include <QClipboard>
#include <QGuiApplication>
#include <QAbstractButton>
#include <QPushButton>
#include <QThread>
#include <QPointer>

namespace nim {

void showCriticalWithDetails(QWidget* parent,
                             const QString& summary,
                             const QString& details,
                             const QString& windowTitle)
{
  // Qt widgets must be created on the main (GUI) thread. When errors originate from background threads
  // (e.g. the rendering thread), queue the dialog to the GUI thread instead of crashing on macOS.
  if (QCoreApplication* app = QCoreApplication::instance(); app && QThread::currentThread() != app->thread()) {
    const QString summaryCopy = summary;
    const QString detailsCopy = details;
    const QString windowTitleCopy = windowTitle;
    QPointer<QWidget> parentPtr(parent);
    QMetaObject::invokeMethod(app,
                              [parentPtr, summaryCopy, detailsCopy, windowTitleCopy]() {
                                showCriticalWithDetails(parentPtr.data(), summaryCopy, detailsCopy, windowTitleCopy);
                              },
                              Qt::QueuedConnection);
    return;
  }

  QDialog dialog(parent);
  dialog.setModal(true);
  dialog.setWindowTitle(windowTitle.isEmpty() ? QApplication::applicationName() : windowTitle);
  dialog.setSizeGripEnabled(true);
  dialog.setWindowIcon(dialog.style()->standardIcon(QStyle::SP_MessageBoxCritical));

  auto* layout = new QVBoxLayout(&dialog);

  auto* topLayout = new QHBoxLayout();

  auto* iconLabel = new QLabel(&dialog);
  iconLabel->setPixmap(dialog.style()->standardIcon(QStyle::SP_MessageBoxCritical).pixmap(32, 32));
  iconLabel->setAlignment(Qt::AlignLeft | Qt::AlignTop);
  topLayout->addWidget(iconLabel, 0, Qt::AlignTop);

  auto* summaryLabel = new QLabel(summary, &dialog);
  summaryLabel->setWordWrap(true);
  summaryLabel->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
  topLayout->addWidget(summaryLabel, 1);

  layout->addLayout(topLayout);

  QPlainTextEdit* detailsEdit = nullptr;
  if (!details.isEmpty()) {
    detailsEdit = new QPlainTextEdit(details, &dialog);
    detailsEdit->setReadOnly(true);
    detailsEdit->setMinimumHeight(180);
    detailsEdit->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
    detailsEdit->setWordWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    detailsEdit->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    layout->addWidget(detailsEdit);
  }

  auto* buttonBox = new QDialogButtonBox(QDialogButtonBox::Ok, Qt::Horizontal, &dialog);
  auto* copyButton = buttonBox->addButton(QObject::tr("Copy Details"), QDialogButtonBox::ActionRole);
  QObject::connect(buttonBox, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
  QObject::connect(buttonBox, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
  QObject::connect(copyButton, &QAbstractButton::clicked, &dialog, [summary, details]() {
    QString textToCopy = summary;
    if (!details.isEmpty()) {
      textToCopy.append("\n\n").append(details);
    }
    QGuiApplication::clipboard()->setText(textToCopy);
  });
  layout->addWidget(buttonBox);

  dialog.resize(dialog.sizeHint().expandedTo(QSize(520, 320)));
  dialog.exec();
}

} // namespace nim
