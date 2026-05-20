#include "ztheme.h"

#include "zjson.h"
#include "zlog.h"

#ifdef Q_OS_MACOS
#include <private/qcore_mac_p.h>
#endif

#include <QMetaEnum>
#include <QFile>
#include <QApplication>
#include <QGuiApplication>
#include <QSettings>
#include <QScopedValueRollback>
#include <QStyle>
#include <QStyleFactory>
#include <QStyleHints>
#include <QAction>
#include <QActionGroup>
#include <QAbstractButton>
#include <QMenu>
#include <array>

namespace nim {

namespace {

constexpr char kThemePreferenceSettingsKey[] = "appearance/theme";

QString themePreferenceSettingsValue(ZTheme::ThemePreference preference)
{
  switch (preference) {
    case ZTheme::SystemTheme:
      return QStringLiteral("system");
    case ZTheme::LightTheme:
      return QStringLiteral("light");
    case ZTheme::DarkTheme:
      return QStringLiteral("dark");
    case ZTheme::QtDarkTheme:
      return QStringLiteral("qt_dark");
  }
  LOG(FATAL) << "Unknown theme preference: " << preference;
  return QStringLiteral("system");
}

QString themePreferenceLabel(ZTheme::ThemePreference preference)
{
  switch (preference) {
    case ZTheme::SystemTheme:
      return ZTheme::tr("System");
    case ZTheme::LightTheme:
      return ZTheme::tr("Light");
    case ZTheme::DarkTheme:
      return ZTheme::tr("Dark");
    case ZTheme::QtDarkTheme:
      return ZTheme::tr("Qt Dark");
  }
  LOG(FATAL) << "Unknown theme preference: " << preference;
  return {};
}

ZTheme::ThemePreference themePreferenceFromSettingsValue(const QString& value)
{
  const QString normalized = value.trimmed().toLower();
  if (normalized.isEmpty() || normalized == QStringLiteral("system")) {
    return ZTheme::SystemTheme;
  }
  if (normalized == QStringLiteral("light")) {
    return ZTheme::LightTheme;
  }
  if (normalized == QStringLiteral("dark")) {
    return ZTheme::DarkTheme;
  }
  if (normalized == QStringLiteral("qt_dark") || normalized == QStringLiteral("qt-dark")) {
    return ZTheme::QtDarkTheme;
  }

  LOG(WARNING) << "Unknown saved theme preference " << value << "; falling back to system";
  return ZTheme::SystemTheme;
}

} // namespace

ZTheme& ZTheme::instance()
{
  static ZTheme theme;
  return theme;
}

ZTheme::ZTheme()
{
  const QMetaObject& m = *metaObject();
  m_colors.resize(m.enumerator(m.indexOfEnumerator("Color")).keyCount());
  m_icons.resize(m.enumerator(m.indexOfEnumerator("Icon")).keyCount());
  m_iconFiles.resize(m_icons.size());
  m_themePreference = themePreferenceFromSettingsValue(
    QSettings().value(kThemePreferenceSettingsKey, QStringLiteral("system")).toString());

#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
  if (auto* styleHints = QGuiApplication::styleHints()) {
    connect(styleHints, &QStyleHints::colorSchemeChanged, this, [this](Qt::ColorScheme) {
      updateTheme();
    });
  }
#endif

  updateTheme();
}

void ZTheme::updateTheme()
{
  if (m_updatingTheme) {
    return;
  }

  QScopedValueRollback<bool> updating(m_updatingTheme, true);

#if defined(Q_OS_WIN)
  QApplication::setStyle(QStyleFactory::create("Fusion"));
#elif !defined(Q_OS_MACOS)
  QApplication::setStyle(QStyleFactory::create("Fusion"));
#endif
  syncQtColorSchemePreference();
  m_currentTheme = resolvedTheme();
  LOG(INFO) << "Current Theme: " << m_currentTheme;
  resetApplicationPaletteToStyleDefault();
  const QString themePath = QString(":Resources/themes/%1.atlastheme").arg(m_currentTheme);
  if (!QFile::exists(themePath)) {
    LOG(WARNING) << "Theme file not found: " << themePath << ". Falling back to Qt defaults.";
    return;
  }
  loadTheme(themePath, shouldApplyAtlasPalette());
  Q_EMIT themeChanged();
}

bool ZTheme::event(QEvent* event)
{
  if (event->type() == QEvent::ApplicationPaletteChange && !m_updatingTheme) {
    updateTheme();
  }
  return QObject::event(event);
}

void ZTheme::setThemePreference(ThemePreference preference)
{
  if (m_themePreference == preference) {
    return;
  }

  m_themePreference = preference;
  QSettings().setValue(kThemePreferenceSettingsKey, themePreferenceSettingsValue(preference));
  Q_EMIT themePreferenceChanged(m_themePreference);
  updateTheme();
}

void ZTheme::loadTheme(const QString& fn, bool applyPaletteOverrides)
{
  const auto& loadObj = loadJsonObject(fn);
  if (!loadObj.contains("AtlasTheme") || !loadObj.at("AtlasTheme").is_object()) {
    throw ZException("File is not AtlasTheme format");
  }

  const auto& themeObj = loadObj.at("AtlasTheme").as_object();

  const QMetaObject& m = *metaObject();
  std::map<QString, QColor> paletteNames;
  std::vector<std::pair<QColor, QString>> colors(m_colors.size());
  std::vector<QIcon> icons(m_icons.size());
  std::vector<QString> iconFiles(m_iconFiles.size());

  for (const auto& [key, value] : themeObj) {
    if (applyPaletteOverrides && key == "Palette") {
      auto& po = value.as_object();
      for (const auto& [pkey, pvalue] : po) {
        QString qpkey = QString::fromUtf8(pkey.data(), pkey.size());
        paletteNames[qpkey] = readNamedColor(asQString(pvalue), paletteNames).first;
      }
    }
  }

  for (const auto& [key, value] : themeObj) {
    if (applyPaletteOverrides && key == "Colors") {
      auto& po = value.as_object();
      QMetaEnum e = m.enumerator(m.indexOfEnumerator("Color"));
      for (int i = 0, total = e.keyCount(); i < total; ++i) {
        auto ekey = json::string_view(e.key(i));
        if (!po.contains(ekey)) {
          continue;
        }
        colors[i] = readNamedColor(asQString(po.at(ekey)), paletteNames);
      }
    } else if (key == "Icons") {
      auto& po = value.as_object();
      QMetaEnum e = m.enumerator(m.indexOfEnumerator("Icon"));
      for (int i = 0, total = e.keyCount(); i < total; ++i) {
        auto ekey = json::string_view(e.key(i));
        if (!po.contains(ekey)) {
          throw ZException(fmt::format("theme icon {} missing", e.key(i)));
        }
        iconFiles[i] = asQString(po.at(ekey));
        QIcon(iconFiles[i]).swap(icons[i]);
      }
    }
  }

  m_palette = std::move(paletteNames);
  m_colors = std::move(colors);
  m_iconFiles = std::move(iconFiles);
  m_icons = std::move(icons);
  QApplication::setPalette(palette());
}

std::pair<QColor, QString> ZTheme::readNamedColor(const QString& color, const std::map<QString, QColor>& palette) const
{
  if (palette.contains(color)) {
    return std::make_pair(palette.at(color), color);
  }

  const QColor col(color.startsWith('#') ? color : '#' + color);
  if (!col.isValid()) {
    throw ZException(fmt::format("theme color {} invalid", color));
  }
  return std::make_pair(col, QString());
}

QString ZTheme::resolvedTheme() const
{
  switch (m_themePreference) {
    case SystemTheme:
      return detectCurrentTheme();
    case LightTheme:
      return QStringLiteral("light");
    case DarkTheme:
      return QStringLiteral("dark");
    case QtDarkTheme:
      return QStringLiteral("dark");
  }
  LOG(FATAL) << "Unknown theme preference: " << m_themePreference;
  return QStringLiteral("dark");
}

bool ZTheme::shouldApplyAtlasPalette() const
{
  if (m_themePreference == QtDarkTheme) {
    return false;
  }
  if (m_themePreference == SystemTheme) {
    return detectCurrentTheme() == QStringLiteral("dark");
  }
  return m_themePreference == DarkTheme;
}

void ZTheme::syncQtColorSchemePreference() const
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
  auto* styleHints = QGuiApplication::styleHints();
  CHECK(styleHints != nullptr);
  switch (m_themePreference) {
    case SystemTheme:
      styleHints->unsetColorScheme();
      return;
    case LightTheme:
      styleHints->setColorScheme(Qt::ColorScheme::Light);
      return;
    case DarkTheme:
      styleHints->setColorScheme(Qt::ColorScheme::Dark);
      return;
    case QtDarkTheme:
      styleHints->setColorScheme(Qt::ColorScheme::Dark);
      return;
  }
  LOG(FATAL) << "Unknown theme preference: " << m_themePreference;
#endif
}

QString ZTheme::detectCurrentTheme() const
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
  if (auto* styleHints = QGuiApplication::styleHints()) {
    switch (styleHints->colorScheme()) {
      case Qt::ColorScheme::Dark:
        return QStringLiteral("dark");
      case Qt::ColorScheme::Light:
        return QStringLiteral("light");
      default:
        break;
    }
  }
#endif

#if defined(Q_OS_MACOS)
  return qt_mac_applicationIsInDarkMode() ? QStringLiteral("dark") : QStringLiteral("light");
#elif defined(Q_OS_WIN)
  constexpr char regkey[] = "HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize";
  bool ok;
  const auto setting = QSettings(regkey, QSettings::NativeFormat).value("AppsUseLightTheme").toInt(&ok);
  return (ok && setting == 0) ? QStringLiteral("dark") : QStringLiteral("light");
#else
  return QStringLiteral("dark");
#endif
}

void ZTheme::resetApplicationPaletteToStyleDefault() const
{
  if (QApplication::style()) {
    QApplication::setPalette(QApplication::style()->standardPalette());
  } else {
    QApplication::setPalette(QPalette{});
  }
}

void ZTheme::bindIcon(QAction* action, Icon file)
{
  CHECK(action != nullptr);
  action->setIcon(icon(file));
  connect(this, &ZTheme::themeChanged, action, [this, action, file]() {
    action->setIcon(icon(file));
  });
}

void ZTheme::bindIcon(QAbstractButton* button, Icon file)
{
  CHECK(button != nullptr);
  button->setIcon(icon(file));
  connect(this, &ZTheme::themeChanged, button, [this, button, file]() {
    button->setIcon(icon(file));
  });
}

QMenu* ZTheme::addThemeMenu(QMenu* parentMenu)
{
  CHECK(parentMenu != nullptr);
  auto* themeMenu = parentMenu->addMenu(tr("&Theme"));
  auto* group = new QActionGroup(themeMenu);
  group->setExclusive(true);

  auto* systemAction = new QAction(themePreferenceLabel(SystemTheme), themeMenu);
  auto* lightAction = new QAction(themePreferenceLabel(LightTheme), themeMenu);
  auto* darkAction = new QAction(themePreferenceLabel(DarkTheme), themeMenu);
  auto* qtDarkAction = new QAction(themePreferenceLabel(QtDarkTheme), themeMenu);
  const std::array<std::pair<QAction*, ThemePreference>, 4> actions = {
    std::make_pair(systemAction, SystemTheme),
    std::make_pair(lightAction, LightTheme),
    std::make_pair(darkAction, DarkTheme),
    std::make_pair(qtDarkAction, QtDarkTheme),
  };

  for (const auto& actionAndPreference : actions) {
    QAction* action = actionAndPreference.first;
    const ThemePreference preference = actionAndPreference.second;
    action->setCheckable(true);
    group->addAction(action);
    themeMenu->addAction(action);
    connect(action, &QAction::triggered, this, [this, preference]() {
      setThemePreference(preference);
    });
  }

  auto syncActions = [this, systemAction, lightAction, darkAction, qtDarkAction]() {
    systemAction->setChecked(m_themePreference == SystemTheme);
    lightAction->setChecked(m_themePreference == LightTheme);
    darkAction->setChecked(m_themePreference == DarkTheme);
    qtDarkAction->setChecked(m_themePreference == QtDarkTheme);
  };
  connect(this, &ZTheme::themePreferenceChanged, themeMenu, syncActions);
  syncActions();

  return themeMenu;
}

QString ZTheme::treeViewIndicatorStyleSheet() const
{
  return QString("QTreeView::indicator:unchecked {image: url(%1);}"
                 "QTreeView::indicator:checked {image: url(%2);}"
                 "QTreeView::indicator:indeterminate {image: url(%3);}")
    .arg(iconFile(EyeCloseIcon), iconFile(EyeOpenIcon), iconFile(EyeHalfIcon));
}

// #define DEBUG_QPalette

QPalette ZTheme::palette() const
{
  QPalette pal = QApplication::palette();

#ifdef DEBUG_QPalette
  const QMetaObject& m = QPalette::staticMetaObject;
  QMetaEnum e = m.enumerator(m.indexOfEnumerator("ColorRole"));
  for (int i = 0, total = static_cast<int>(QPalette::NColorRoles); i < total; ++i) {
    const QString key = e.key(i);
    VLOG(1) << key << " Color A: "
            << pal.color(QPalette::Active, static_cast<QPalette::ColorRole>(e.value(i)))
            << " Color D: "
            << pal.color(QPalette::Disabled, static_cast<QPalette::ColorRole>(e.value(i)))
            << " Color I: "
            << pal.color(QPalette::Inactive, static_cast<QPalette::ColorRole>(e.value(i)));
    VLOG(1) << key << " Brush A: "
            << pal.brush(QPalette::Active, static_cast<QPalette::ColorRole>(e.value(i)))
            << " Brush D: "
            << pal.brush(QPalette::Disabled, static_cast<QPalette::ColorRole>(e.value(i)))
            << " Brush I: "
            << pal.brush(QPalette::Inactive, static_cast<QPalette::ColorRole>(e.value(i)));
  }
#endif

  const static struct
  {
    Color themeColor;
    QPalette::ColorRole paletteColorRole;
    QPalette::ColorGroup paletteColorGroup;
    bool setColorRoleAsBrush;
  } mapping[] = {
    {PaletteWindow,                  QPalette::Window,          QPalette::All,      false},
    {PaletteWindowDisabled,          QPalette::Window,          QPalette::Disabled, false},
    {PaletteWindowText,              QPalette::WindowText,      QPalette::All,      true },
    {PaletteWindowTextDisabled,      QPalette::WindowText,      QPalette::Disabled, true },
    {PaletteBase,                    QPalette::Base,            QPalette::All,      false},
    {PaletteBaseDisabled,            QPalette::Base,            QPalette::Disabled, false},
    {PaletteAlternateBase,           QPalette::AlternateBase,   QPalette::All,      false},
    {PaletteAlternateBaseDisabled,   QPalette::AlternateBase,   QPalette::Disabled, false},
    {PaletteToolTipBase,             QPalette::ToolTipBase,     QPalette::All,      true },
    {PaletteToolTipBaseDisabled,     QPalette::ToolTipBase,     QPalette::Disabled, true },
    {PaletteToolTipText,             QPalette::ToolTipText,     QPalette::All,      false},
    {PaletteToolTipTextDisabled,     QPalette::ToolTipText,     QPalette::Disabled, false},
    {PaletteText,                    QPalette::Text,            QPalette::All,      true },
    {PaletteTextDisabled,            QPalette::Text,            QPalette::Disabled, true },
    {PaletteButton,                  QPalette::Button,          QPalette::All,      false},
    {PaletteButtonDisabled,          QPalette::Button,          QPalette::Disabled, false},
    {PaletteButtonText,              QPalette::ButtonText,      QPalette::All,      true },
    {PaletteButtonTextDisabled,      QPalette::ButtonText,      QPalette::Disabled, true },
    {PaletteBrightText,              QPalette::BrightText,      QPalette::All,      false},
    {PaletteBrightTextDisabled,      QPalette::BrightText,      QPalette::Disabled, false},
    {PaletteHighlight,               QPalette::Highlight,       QPalette::All,      true },
    {PaletteHighlightDisabled,       QPalette::Highlight,       QPalette::Disabled, true },
    {PaletteHighlightedText,         QPalette::HighlightedText, QPalette::All,      true },
    {PaletteHighlightedTextDisabled, QPalette::HighlightedText, QPalette::Disabled, true },
    {PaletteLink,                    QPalette::Link,            QPalette::All,      false},
    {PaletteLinkDisabled,            QPalette::Link,            QPalette::Disabled, false},
    {PaletteLinkVisited,             QPalette::LinkVisited,     QPalette::All,      false},
    {PaletteLinkVisitedDisabled,     QPalette::LinkVisited,     QPalette::Disabled, false},
    {PaletteLight,                   QPalette::Light,           QPalette::All,      false},
    {PaletteLightDisabled,           QPalette::Light,           QPalette::Disabled, false},
    {PaletteMidlight,                QPalette::Midlight,        QPalette::All,      false},
    {PaletteMidlightDisabled,        QPalette::Midlight,        QPalette::Disabled, false},
    {PaletteDark,                    QPalette::Dark,            QPalette::All,      false},
    {PaletteDarkDisabled,            QPalette::Dark,            QPalette::Disabled, false},
    {PaletteMid,                     QPalette::Mid,             QPalette::All,      false},
    {PaletteMidDisabled,             QPalette::Mid,             QPalette::Disabled, false},
    {PaletteShadow,                  QPalette::Shadow,          QPalette::All,      false},
    {PaletteShadowDisabled,          QPalette::Shadow,          QPalette::Disabled, false}
  };

  for (auto entry : mapping) {
    const QColor themeColor = color(entry.themeColor);
    // Use original color if color is not defined in theme.
    if (themeColor.isValid()) {
      if (entry.setColorRoleAsBrush) {
        pal.setBrush(entry.paletteColorGroup, entry.paletteColorRole, themeColor);
#ifdef DEBUG_QPalette
        VLOG(1) << "set brush " << e.valueToKey(entry.paletteColorRole) << " " << entry.paletteColorGroup << " to "
                << themeColor;
#endif
      } else {
        pal.setColor(entry.paletteColorGroup, entry.paletteColorRole, themeColor);
#ifdef DEBUG_QPalette
        VLOG(1) << "set color " << e.valueToKey(entry.paletteColorRole) << " " << entry.paletteColorGroup << " to "
                << themeColor;
#endif
      }
    }
  }

  return pal;
}

} // namespace nim
