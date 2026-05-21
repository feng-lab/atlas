#pragma once

#include "zimg.h"
#include "zmasktoroioptions.h"

#include <QDialog>

#include <vector>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLineEdit;
class QSpinBox;

namespace nim {

class ZMaskToROIImportDialog final : public QDialog
{
  Q_OBJECT

public:
  explicit ZMaskToROIImportDialog(QWidget* parent = nullptr);

  void setInitialDirectory(const QString& path);

  [[nodiscard]] QString selectedFile() const;
  [[nodiscard]] FileFormat selectedFormat() const;
  [[nodiscard]] ZMaskToROIOptions selectedOptions() const;

protected:
  void accept() override;

private:
  void browseFile();
  void updateEnabledControls();
  void loadSettings();
  void saveSettings() const;

  QStringList m_filters;
  std::vector<FileFormat> m_formats;
  QString m_initialDirectory;

  QLineEdit* m_fileEdit = nullptr;
  QComboBox* m_formatCombo = nullptr;
  QComboBox* m_outputTypeCombo = nullptr;
  QDoubleSpinBox* m_epsilonSpin = nullptr;
  QDoubleSpinBox* m_minKnotSpacingSpin = nullptr;
  QSpinBox* m_sampledTargetPointsSpin = nullptr;
  QSpinBox* m_sampledMaxPointSpacingSpin = nullptr;
  QComboBox* m_fallbackCombo = nullptr;
  QCheckBox* m_preserveHolesCheck = nullptr;
};

} // namespace nim
