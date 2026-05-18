#include "zflagsettingsdialog.h"

#include "zflagsettingsregistry.h"
#include "zparameter.h"
#include "zstringparameter.h"
#include "zsysteminfo.h"

#include "zcommandlineflags.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QFont>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QTabWidget>
#include <QUrl>
#include <QVBoxLayout>
#include <QWidget>

namespace nim {
namespace {

constexpr int kChoiceValueRole = Qt::UserRole;
constexpr int kChoiceInvalidRole = Qt::UserRole + 1;

QString htmlCode(const QString& value)
{
  return QStringLiteral("<code>%1</code>").arg(value.toHtmlEscaped());
}

QString editorLabel(const ZFlagSettingSpec& spec)
{
  return spec.advanced ? QStringLiteral("%1 (Advanced)").arg(spec.label) : spec.label;
}

QString helpLabelText(const ZFlagSettingSpec& spec, const ZCommandLineFlagInfo& info, const QString& effectiveValue)
{
  QString text = QStringLiteral("%1<br/>%2<br/><b>Compiled default:</b> %3")
                   .arg(htmlCode(QStringLiteral("--") + spec.name),
                        QString::fromStdString(info.description).toHtmlEscaped(),
                        htmlCode(QString::fromStdString(info.defaultValue)));

  const QString currentValue = QString::fromStdString(info.currentValue);
  if (currentValue != effectiveValue) {
    text += QStringLiteral("<br/><b>Current session:</b> %1").arg(htmlCode(currentValue));
  }
  return text;
}

QString normalizeChoiceValue(const QString& raw, const QStringList& choices)
{
  for (const QString& choice : choices) {
    if (choice.compare(raw, Qt::CaseInsensitive) == 0) {
      return choice;
    }
  }
  return {};
}

bool openLocalFile(const QString& path)
{
  return QDesktopServices::openUrl(QUrl::fromLocalFile(path));
}

} // namespace

ZFlagSettingsDialog::ZFlagSettingsDialog(QWidget* parent)
  : QDialog(parent)
{
  setWindowTitle(tr("Atlas Settings"));
  resize(920, 760);

  QString error;
  if (!m_document.load(atlasUserSettingsFlagfilePath(), atlasManagedFlagNames(), &error)) {
    QMessageBox::critical(this,
                          QApplication::applicationName(),
                          tr("Could not read %1:\n%2").arg(atlasUserSettingsFlagfilePath(), error));
  }
  m_preservedManualLines = m_document.preservedManualLines();

  buildUi();
  populateInitialValues();
  updateBanner();
}

ZFlagSettingsDialog::~ZFlagSettingsDialog() = default;

void ZFlagSettingsDialog::buildUi()
{
  auto* layout = new QVBoxLayout(this);

  m_bannerLabel = new QLabel(this);
  m_bannerLabel->setWordWrap(true);
  m_bannerLabel->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::LinksAccessibleByMouse);
  layout->addWidget(m_bannerLabel);

  m_warningLabel = new QLabel(this);
  m_warningLabel->setWordWrap(true);
  m_warningLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
  m_warningLabel->hide();
  layout->addWidget(m_warningLabel);

  createTabs();
  layout->addWidget(m_tabs, 1);

  auto* buttonBox = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, this);
  auto* restartButton = buttonBox->addButton(tr("Save and Restart"), QDialogButtonBox::ActionRole);
  auto* resetButton = buttonBox->addButton(tr("Reset to Defaults"), QDialogButtonBox::ResetRole);
  auto* editFileButton = buttonBox->addButton(tr("Edit Config Flag File..."), QDialogButtonBox::ActionRole);
  auto* openFolderButton = buttonBox->addButton(tr("Open Config Folder"), QDialogButtonBox::ActionRole);
  layout->addWidget(buttonBox);

  connect(buttonBox, &QDialogButtonBox::accepted, this, &ZFlagSettingsDialog::saveAndAccept);
  connect(buttonBox, &QDialogButtonBox::rejected, this, &QDialog::reject);
  connect(restartButton, &QPushButton::clicked, this, &ZFlagSettingsDialog::saveAndRestart);
  connect(resetButton, &QPushButton::clicked, this, &ZFlagSettingsDialog::restoreDefaults);
  connect(editFileButton, &QPushButton::clicked, this, &ZFlagSettingsDialog::openConfigFileInEditor);
  connect(openFolderButton, &QPushButton::clicked, this, []() {
    QDesktopServices::openUrl(QUrl::fromLocalFile(ZSystemInfo::configDir().absolutePath()));
  });

  const QStringList duplicates = m_document.duplicateManagedFlags();
  if (!duplicates.isEmpty()) {
    m_warningLabel->setText(
      tr("Duplicate managed flags were found in the saved file. Atlas is using the last occurrence for each duplicate, "
         "and saving will normalize the file.\n%1")
        .arg(duplicates.join(", ")));
    m_warningLabel->show();
  }
}

void ZFlagSettingsDialog::createTabs()
{
  m_tabs = new QTabWidget(this);

  QHash<QString, QVBoxLayout*> pages;

  const auto& specs = atlasFlagSettingSpecs();
  m_flagInfos.reserve(specs.size());
  m_fields.reserve(specs.size());
  for (const auto& spec : specs) {
    if (!pages.contains(spec.category)) {
      auto* tabContainer = new QWidget(this);
      auto* tabLayout = new QVBoxLayout(tabContainer);
      tabLayout->setContentsMargins(0, 0, 0, 0);

      auto* scrollArea = new QScrollArea(this);
      scrollArea->setWidgetResizable(true);
      scrollArea->setFrameShape(QFrame::NoFrame);

      auto* page = new QWidget(scrollArea);
      auto* pageLayout = new QVBoxLayout(page);
      pageLayout->setContentsMargins(12, 12, 12, 12);
      pageLayout->setSpacing(10);
      page->setLayout(pageLayout);

      scrollArea->setWidget(page);
      tabLayout->addWidget(scrollArea);
      m_tabs->addTab(tabContainer, spec.category);
      pages.insert(spec.category, pageLayout);
    }

    auto info = std::make_unique<ZCommandLineFlagInfo>();
    const QByteArray flagName = spec.name.toUtf8();
    *info = getCommandLineFlagInfoOrDie(flagName.constData());
    auto* infoPtr = info.get();
    m_flagInfos.push_back(std::move(info));

    FieldWidgets field;
    field.spec = &spec;
    field.info = infoPtr;

    const QString effectiveValue = m_document.hasManagedValue(spec.name)
                                     ? m_document.managedValue(spec.name)
                                     : QString::fromStdString(infoPtr->defaultValue);

    auto* fieldContainer = new QWidget(this);
    auto* fieldLayout = new QVBoxLayout(fieldContainer);
    fieldLayout->setContentsMargins(0, 0, 0, 0);
    fieldLayout->setSpacing(6);

    QLabel* nameLabel = new QLabel(editorLabel(spec), fieldContainer);
    QFont labelFont = nameLabel->font();
    labelFont.setBold(true);
    nameLabel->setFont(labelFont);
    nameLabel->setWordWrap(true);
    nameLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    fieldLayout->addWidget(nameLabel);

    if (spec.editor == ZFlagSettingEditorKind::Choice) {
      auto* combo = new QComboBox(fieldContainer);
      for (const QString& choice : spec.choices) {
        combo->addItem(choice, choice);
      }
      field.comboBox = combo;
      field.editor = combo;
      fieldLayout->addWidget(combo);
    } else if (QString::fromStdString(infoPtr->type) == QLatin1String("bool")) {
      auto parameter = std::make_unique<ZBoolParameter>(spec.label, nullptr);
      field.parameter = std::move(parameter);
      field.editor = field.parameter->createWidget(fieldContainer);
      fieldLayout->addWidget(field.editor);
    } else {
      auto parameter = std::make_unique<ZStringParameter>(spec.label, effectiveValue, nullptr);
      parameter->setDescription(QString::fromStdString(infoPtr->description));
      field.parameter = std::move(parameter);
      auto* lineEdit = qobject_cast<QLineEdit*>(field.parameter->createWidget(fieldContainer));
      CHECK(lineEdit) << "Expected QLineEdit widget for string-backed settings field";
      lineEdit->setClearButtonEnabled(true);
      field.editor = lineEdit;
      field.lineEdit = lineEdit;

      if (spec.editor == ZFlagSettingEditorKind::FilePath || spec.editor == ZFlagSettingEditorKind::DirectoryPath) {
        auto* pathRow = new QWidget(fieldContainer);
        auto* pathLayout = new QHBoxLayout(pathRow);
        pathLayout->setContentsMargins(0, 0, 0, 0);
        pathLayout->setSpacing(6);
        lineEdit->setParent(pathRow);
        pathLayout->addWidget(lineEdit, 1);
        auto* browseButton = new QPushButton(tr("Browse..."), pathRow);
        pathLayout->addWidget(browseButton);
        const auto editorKind = field.spec->editor;
        connect(browseButton, &QPushButton::clicked, this, [this, lineEdit, editorKind, browseButton]() {
          CHECK(lineEdit) << "Browse button requires a line edit";
          const QString startPath = lineEdit->text().trimmed();
          QString selected;
          if (editorKind == ZFlagSettingEditorKind::DirectoryPath) {
            selected = QFileDialog::getExistingDirectory(this,
                                                         tr("Choose Directory"),
                                                         startPath.isEmpty() ? ZSystemInfo::configDir().absolutePath()
                                                                             : startPath);
          } else {
            selected = QFileDialog::getOpenFileName(
              this,
              tr("Choose File"),
              QFileInfo(startPath).exists() ? startPath : ZSystemInfo::configDir().absolutePath());
          }
          if (!selected.isEmpty()) {
            lineEdit->setText(selected);
          }
          browseButton->clearFocus();
        });
        fieldLayout->addWidget(pathRow);
      } else {
        fieldLayout->addWidget(lineEdit);
      }
    }

    auto* helpLabel = new QLabel(helpLabelText(spec, *infoPtr, effectiveValue), fieldContainer);
    helpLabel->setWordWrap(true);
    helpLabel->setTextFormat(Qt::RichText);
    helpLabel->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::LinksAccessibleByMouse);
    fieldLayout->addWidget(helpLabel);

    pages.value(spec.category)->addWidget(fieldContainer);

    auto* separator = new QFrame(fieldContainer);
    separator->setFrameShape(QFrame::HLine);
    separator->setFrameShadow(QFrame::Sunken);
    pages.value(spec.category)->addWidget(separator);

    m_fields.push_back(std::move(field));
  }

  for (auto* pageLayout : pages) {
    pageLayout->addStretch(1);
  }
}

void ZFlagSettingsDialog::populateInitialValues()
{
  for (auto& field : m_fields) {
    const QString initialValue = m_document.hasManagedValue(field.spec->name)
                                   ? m_document.managedValue(field.spec->name)
                                   : QString::fromStdString(field.info->defaultValue);
    setFieldValue(field, initialValue, true);
  }
}

void ZFlagSettingsDialog::updateBanner()
{
  QString text = tr("Saved settings file: %1<br/>").arg(htmlCode(atlasUserSettingsFlagfilePath()));
  if (m_document.fileExistedAtLoad()) {
    text += tr("Editing the saved flagfile. Atlas may normalize the managed section when you save.");
  } else {
    text += tr("No saved flagfile exists yet. Saving will create %1.").arg(htmlCode(atlasUserSettingsFlagfileName()));
  }
  text += tr("<br/>Changes apply the next time Atlas starts.");
  text += tr("<br/>The running Atlas session may currently use different values if Atlas was started with command-line "
             "flags such as %1. This dialog edits the saved flagfile for the next launch.")
            .arg(htmlCode("--v=1"));
  if (!m_preservedManualLines.isEmpty()) {
    text += tr("<br/>Preserved manual block lines: %1. Atlas will keep these custom lines unchanged when you save "
               "from the GUI.")
              .arg(m_preservedManualLines.size());
  }
  m_bannerLabel->setText(text);
}

QString ZFlagSettingsDialog::defaultValueForField(const FieldWidgets& field) const
{
  return QString::fromStdString(field.info->defaultValue);
}

void ZFlagSettingsDialog::setFieldValue(FieldWidgets& field, const QString& value, bool allowInvalidChoice)
{
  if (field.comboBox != nullptr) {
    const QString normalized = normalizeChoiceValue(value, field.spec->choices);
    if (!normalized.isEmpty()) {
      const int invalidIndex = field.comboBox->findData(true, kChoiceInvalidRole);
      if (invalidIndex >= 0) {
        field.comboBox->removeItem(invalidIndex);
      }
      const int idx = field.comboBox->findData(normalized, kChoiceValueRole);
      CHECK(idx >= 0) << "Choice-backed field missing canonical option";
      field.comboBox->setCurrentIndex(idx);
      return;
    }

    if (allowInvalidChoice) {
      int invalidIndex = field.comboBox->findData(true, kChoiceInvalidRole);
      if (invalidIndex < 0) {
        field.comboBox->insertItem(0, tr("Invalid in file: %1").arg(value), value);
        field.comboBox->setItemData(0, true, kChoiceInvalidRole);
        invalidIndex = 0;
      } else {
        field.comboBox->setItemText(invalidIndex, tr("Invalid in file: %1").arg(value));
        field.comboBox->setItemData(invalidIndex, value, kChoiceValueRole);
      }
      field.comboBox->setCurrentIndex(invalidIndex);
    } else {
      const int idx = field.comboBox->findData(field.spec->choices.front(), kChoiceValueRole);
      if (idx >= 0) {
        field.comboBox->setCurrentIndex(idx);
      }
    }
    return;
  }

  if (auto* boolParameter = dynamic_cast<ZBoolParameter*>(field.parameter.get())) {
    const QString lower = value.trimmed().toLower();
    const bool checked = lower == QLatin1String("1") || lower == QLatin1String("true") || lower == QLatin1String("t");
    boolParameter->setValue(checked);
    return;
  }

  if (auto* stringParameter = dynamic_cast<ZStringParameter*>(field.parameter.get())) {
    stringParameter->set(value);
  }
}

bool ZFlagSettingsDialog::validateField(const FieldWidgets& field, QString* canonicalValue, QString* error) const
{
  QString rawValue;
  if (field.comboBox != nullptr) {
    if (field.comboBox->currentData(kChoiceInvalidRole).toBool()) {
      if (error != nullptr) {
        *error = tr("Choose a valid value for %1.").arg(field.spec->label);
      }
      return false;
    }
    rawValue = field.comboBox->currentData(kChoiceValueRole).toString();
  } else if (auto* boolParameter = dynamic_cast<ZBoolParameter*>(field.parameter.get())) {
    rawValue = boolParameter->get() ? QStringLiteral("true") : QStringLiteral("false");
  } else if (auto* stringParameter = dynamic_cast<ZStringParameter*>(field.parameter.get())) {
    rawValue = stringParameter->get().trimmed();
  } else {
    if (error != nullptr) {
      *error = tr("Unsupported settings field for %1.").arg(field.spec->label);
    }
    return false;
  }

  const QString flagType = QString::fromStdString(field.info->type);
  if (flagType != QLatin1String("string") && rawValue.isEmpty()) {
    if (error != nullptr) {
      *error = tr("%1 requires a value.").arg(field.spec->label);
    }
    return false;
  }

  absl::FlagSaver saver;
  const QByteArray name = field.spec->name.toUtf8();
  const QByteArray value = rawValue.toUtf8();
  std::string parseError;
  if (!setCommandLineOption(name.constData(), value.constData(), &parseError)) {
    if (error != nullptr) {
      *error = tr("Invalid value for --%1: %2").arg(field.spec->name, rawValue);
    }
    return false;
  }

  std::string current;
  CHECK(getCommandLineOption(name.constData(), &current)) << "Failed to read canonical flag value";
  if (canonicalValue != nullptr) {
    *canonicalValue = QString::fromStdString(current);
  }
  return true;
}

void ZFlagSettingsDialog::restoreDefaults()
{
  const auto choice = QMessageBox::warning(
    this,
    QApplication::applicationName(),
    tr("Reset all GUI-managed settings to their compiled defaults and clear preserved manual entries?\n"
       "You can still cancel afterwards; the file is only updated when you click Save."),
    QMessageBox::Reset | QMessageBox::Cancel,
    QMessageBox::Cancel);
  if (choice != QMessageBox::Reset) {
    return;
  }

  m_preservedManualLines.clear();
  for (auto& field : m_fields) {
    setFieldValue(field, defaultValueForField(field), false);
  }
  updateBanner();
}

void ZFlagSettingsDialog::openConfigFileInEditor()
{
  const QString path = atlasUserSettingsFlagfilePath();
  if (!QFileInfo::exists(path)) {
    QString error;
    if (!ZFlagfileDocument::writeFile(path, atlasDefaultFlagfileEntries(), {}, &error)) {
      QMessageBox::critical(this, QApplication::applicationName(), tr("Could not create %1:\n%2").arg(path, error));
      return;
    }

    if (!m_document.load(path, atlasManagedFlagNames(), &error)) {
      QMessageBox::warning(this,
                           QApplication::applicationName(),
                           tr("Created %1, but could not refresh the dialog state:\n%2").arg(path, error));
    } else {
      m_preservedManualLines = m_document.preservedManualLines();
      updateBanner();
    }
  }

  if (!openLocalFile(path)) {
    QMessageBox::critical(this,
                          QApplication::applicationName(),
                          tr("Could not open %1 in an external editor.").arg(path));
  }
}

void ZFlagSettingsDialog::saveAndAccept()
{
  if (!saveChanges()) {
    return;
  }
  m_restartRequested = false;
  accept();
}

void ZFlagSettingsDialog::saveAndRestart()
{
  if (!saveChanges()) {
    return;
  }
  m_restartRequested = true;
  accept();
}

bool ZFlagSettingsDialog::saveChanges()
{
  QString conflictError;
  if (!m_document.matchesFileOnDisk(atlasUserSettingsFlagfilePath(), &conflictError)) {
    QMessageBox::warning(this,
                         QApplication::applicationName(),
                         tr("%1\n\nClose this dialog and reopen it before saving.").arg(conflictError));
    return false;
  }

  std::vector<ZManagedFlagfileEntry> entries;
  entries.reserve(m_fields.size());

  for (const auto& field : m_fields) {
    QString canonicalValue;
    QString validationError;
    if (!validateField(field, &canonicalValue, &validationError)) {
      QMessageBox::warning(this, QApplication::applicationName(), validationError);
      if (field.lineEdit != nullptr) {
        field.lineEdit->setFocus();
        field.lineEdit->selectAll();
      } else if (field.comboBox != nullptr) {
        field.comboBox->setFocus();
      } else if (field.editor != nullptr) {
        field.editor->setFocus();
      }
      return false;
    }

    ZManagedFlagfileEntry entry;
    entry.category = field.spec->category;
    entry.label = field.spec->label;
    entry.name = field.spec->name;
    entry.description = QString::fromStdString(field.info->description);
    entry.value = canonicalValue;
    entries.push_back(std::move(entry));
  }

  QString error;
  if (!ZFlagfileDocument::writeFile(atlasUserSettingsFlagfilePath(), entries, m_preservedManualLines, &error)) {
    QMessageBox::critical(this,
                          QApplication::applicationName(),
                          tr("Could not write %1:\n%2").arg(atlasUserSettingsFlagfilePath(), error));
    return false;
  }

  return true;
}

} // namespace nim
