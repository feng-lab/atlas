#pragma once

#include "zglobal.h"
#include <QBrush>
#include <QObject>
#include <QPalette>
#include <QIcon>
#include <vector>
#include <map>

class QMenu;

namespace nim {

class ZTheme : public QObject
{
  Q_OBJECT

  Q_ENUMS(Color)
  Q_ENUMS(Icon)
  Q_ENUMS(ThemePreference)

public:
  enum ThemePreference
  {
    SystemTheme,
    LightTheme,
    DarkTheme,
    QtDarkTheme,
  };

  static ZTheme& instance();

  void updateTheme();

Q_SIGNALS:
  void themeChanged();

  void themePreferenceChanged(ThemePreference preference);

public:
  [[nodiscard]] QString currentTheme() const
  {
    return m_currentTheme;
  }

  [[nodiscard]] ThemePreference themePreference() const
  {
    return m_themePreference;
  }

  [[nodiscard]] bool isDarkTheme() const
  {
    return m_currentTheme == "dark";
  }

  void setThemePreference(ThemePreference preference);

  enum Color
  {
    /* Palette for QPalette */

    PaletteWindow,
    PaletteWindowText,
    PaletteBase,
    PaletteAlternateBase,
    PaletteToolTipBase,
    PaletteToolTipText,
    PaletteText,
    PaletteButton,
    PaletteButtonText,
    PaletteBrightText,
    PaletteHighlight,
    PaletteHighlightedText,
    PaletteLink,
    PaletteLinkVisited,

    PaletteLight,
    PaletteMidlight,
    PaletteDark,
    PaletteMid,
    PaletteShadow,

    PaletteWindowDisabled,
    PaletteWindowTextDisabled,
    PaletteBaseDisabled,
    PaletteAlternateBaseDisabled,
    PaletteToolTipBaseDisabled,
    PaletteToolTipTextDisabled,
    PaletteTextDisabled,
    PaletteButtonDisabled,
    PaletteButtonTextDisabled,
    PaletteBrightTextDisabled,
    PaletteHighlightDisabled,
    PaletteHighlightedTextDisabled,
    PaletteLinkDisabled,
    PaletteLinkVisitedDisabled,

    PaletteLightDisabled,
    PaletteMidlightDisabled,
    PaletteDarkDisabled,
    PaletteMidDisabled,
    PaletteShadowDisabled,

    /* Log */

    LogErrorMessageTextColor,
    LogNormalMessageTextColor,
    LogWarningMessageTextColor,
  };

  enum Icon
  {
    LoadObjectIcon,
    OpenFolderIcon,
    SaveIcon,
    SaveAsIcon,
    BackgroundIcon,
    AxisIcon,
    ScreenshotIcon,
    ZoomInIcon,
    ZoomOutIcon,
    CamcoderIcon,
    UndoIcon,
    RedoIcon,
    NormalViewIcon,
    ProjectionViewIcon,
    MontageViewIcon,
    CollapseIcon,
    ExpandIcon,
    DragIcon,
    SelectionIcon,
    CleanupIcon,
    SplineIcon,
    SplineCutIcon,
    PolygonIcon,
    EllipseIcon,
    RectangleIcon,
    TraceIcon,
    AutoTraceIcon,
    RunCommandIcon,
    RunTestIcon,
    PlayIcon,
    PauseIcon,
    ReturnToStartIcon,
    GoToEndIcon,
    ReversePlayIcon,
    RepeatIcon,
    EyeOpenIcon,
    EyeHalfIcon,
    EyeCloseIcon,
    ArrowDownIcon,
    ArrowRightIcon,
    SettingsIcon,
    LockedIcon,
    UnlockedIcon,
    FlipHorizontalIcon,
    FlipVerticalIcon,
    ClearIcon,
    CopyIcon,
    HelpIcon,
    CancelIcon,
  };

  const QIcon& icon(Icon file) const
  {
    return m_icons.at(file);
  }

  const QString& iconFile(Icon file) const
  {
    return m_iconFiles.at(file);
  }

  QMenu* addThemeMenu(QMenu* parentMenu);

  [[nodiscard]] QString treeViewIndicatorStyleSheet() const;

private:
  ZTheme();

  void loadTheme(const QString& file, bool applyPaletteOverrides);

  QColor color(Color role) const
  {
    return m_colors.at(role).first;
  }

  [[nodiscard]] QString detectCurrentTheme() const;

  [[nodiscard]] QString resolvedTheme() const;

  void syncQtColorSchemePreference() const;

  void resetApplicationPaletteToStyleDefault() const;

  QPalette palette() const;

  std::pair<QColor, QString> readNamedColor(const QString& color, const std::map<QString, QColor>& palette) const;

  std::map<QString, QColor> m_palette;
  std::vector<std::pair<QColor, QString>> m_colors;
  std::vector<QIcon> m_icons;
  std::vector<QString> m_iconFiles;

  QString m_currentTheme;
  ThemePreference m_themePreference = SystemTheme;
  bool m_updatingTheme = false;
};

} // namespace nim
