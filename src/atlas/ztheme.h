#pragma once

#include "zglobal.h"
#include <QBrush>
#include <QObject>
#include <QPalette>
#include <QIcon>
#include <vector>
#include <map>

namespace nim {

class ZTheme : public QObject
{
Q_OBJECT

  Q_ENUMS(Color)
  Q_ENUMS(Icon)

public:
  static ZTheme& instance();

  ZTheme();

  void updateTheme();

  QString currentTheme() const
  { return m_currentTheme; }

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
    CollapseIcon,
    ExpandIcon,
    DragIcon,
    SelectionIcon,
    CleanupIcon,
    SplineIcon,
    PolygonIcon,
    EllipseIcon,
    RectangleIcon,
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
  };

  const QIcon& icon(Icon file) const
  { return m_icons.at(file); }

  const QString& iconFile(Icon file) const
  { return m_iconFiles.at(file); }

private:
  void loadTheme(const QString& file);

  const QColor& color(Color role) const
  { return m_colors.at(role).first; }

  QPalette palette() const;

  std::pair<QColor, QString> readNamedColor(const QString& color) const;

  std::map<QString, QColor> m_palette;
  std::vector<std::pair<QColor, QString>> m_colors;
  std::vector<QIcon> m_icons;
  std::vector<QString> m_iconFiles;

  QString m_currentTheme;
};

} // namespace nim
