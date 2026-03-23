#pragma once

#include "zflagfiledocument.h"

#include <QDialog>

#include <memory>
#include <vector>

class QLabel;
class QComboBox;
class QLineEdit;
class QTabWidget;
class QWidget;

namespace gflags {
struct CommandLineFlagInfo;
}

namespace nim {

class ZBoolParameter;
class ZParameter;
class ZStringParameter;
class ZFlagSettingsDialog : public QDialog
{
  Q_OBJECT

public:
  explicit ZFlagSettingsDialog(QWidget* parent = nullptr);
  ~ZFlagSettingsDialog() override;

  [[nodiscard]] bool restartRequested() const
  {
    return m_restartRequested;
  }

private:
  struct FieldWidgets
  {
    const ZFlagSettingSpec* spec = nullptr;
    gflags::CommandLineFlagInfo* info = nullptr;
    std::unique_ptr<ZParameter> parameter;
    QWidget* editor = nullptr;
    QLineEdit* lineEdit = nullptr;
    QComboBox* comboBox = nullptr;
  };

private:
  void buildUi();
  void createTabs();
  void populateInitialValues();
  void updateBanner();
  void restoreDefaults();
  void openConfigFileInEditor();
  void saveAndRestart();
  void saveAndAccept();
  [[nodiscard]] bool saveChanges();

  [[nodiscard]] QString defaultValueForField(const FieldWidgets& field) const;
  void setFieldValue(FieldWidgets& field, const QString& value, bool allowInvalidChoice);
  [[nodiscard]] bool validateField(const FieldWidgets& field, QString* canonicalValue, QString* error) const;

private:
  ZFlagfileDocument m_document;
  QStringList m_preservedManualLines;
  std::vector<std::unique_ptr<gflags::CommandLineFlagInfo>> m_flagInfos;
  std::vector<FieldWidgets> m_fields;

  QLabel* m_bannerLabel = nullptr;
  QLabel* m_warningLabel = nullptr;
  QTabWidget* m_tabs = nullptr;
  bool m_restartRequested = false;
};

} // namespace nim
