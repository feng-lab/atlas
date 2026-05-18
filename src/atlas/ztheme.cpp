#include "ztheme.h"

#include "zjson.h"
#include "zlog.h"

#ifdef Q_OS_MACOS
#include <private/qcore_mac_p.h>
#endif

#include <QMetaEnum>
#include <QFile>
#include <QApplication>
#include <QSettings>
#include <QStyleFactory>

namespace nim {

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

  updateTheme();
}

void ZTheme::updateTheme()
{
#if defined(Q_OS_MACOS)
  m_currentTheme = qt_mac_applicationIsInDarkMode() ? "dark" : "light";
#elif defined(Q_OS_WIN)
  constexpr char regkey[] = "HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize";
  bool ok;
  const auto setting = QSettings(regkey, QSettings::NativeFormat).value("AppsUseLightTheme").toInt(&ok);
  m_currentTheme = (ok && setting == 0) ? "dark" : "light";
  QApplication::setStyle(QStyleFactory::create("Fusion"));
#else
  m_currentTheme = "dark";
  QApplication::setStyle(QStyleFactory::create("Fusion"));
#endif
  LOG(INFO) << "Current Theme: " << m_currentTheme;
  const QString themePath = QString(":Resources/themes/%1.atlastheme").arg(m_currentTheme);
  if (!QFile::exists(themePath)) {
    LOG(WARNING) << "Theme file not found: " << themePath << ". Falling back to Qt defaults.";
    return;
  }
  loadTheme(themePath);
}

bool ZTheme::event(QEvent* event)
{
  if (event->type() == QEvent::ApplicationPaletteChange) {
    updateTheme();
  }
  return QObject::event(event);
}

void ZTheme::loadTheme(const QString& fn)
{
  const auto& loadObj = loadJsonObject(fn);
  if (!loadObj.contains("AtlasTheme") || !loadObj.at("AtlasTheme").is_object()) {
    throw ZException("File is not AtlasTheme format");
  }

  const auto& themeObj = loadObj.at("AtlasTheme").as_object();

  const QMetaObject& m = *metaObject();

  for (const auto& [key, value] : themeObj) {
    if (key == "Palette") {
      auto& po = value.as_object();
      for (const auto& [pkey, pvalue] : po) {
        QString qpkey = QString::fromUtf8(pkey.data(), pkey.size());
        m_palette[qpkey] = readNamedColor(asQString(pvalue)).first;
      }
    }
  }

  for (const auto& [key, value] : themeObj) {
    if (key == "Colors") {
      auto& po = value.as_object();
      QMetaEnum e = m.enumerator(m.indexOfEnumerator("Color"));
      for (int i = 0, total = e.keyCount(); i < total; ++i) {
        auto ekey = json::string_view(e.key(i));
        if (!po.contains(ekey)) {
          continue;
        }
        m_colors[i] = readNamedColor(asQString(po.at(ekey)));
      }
    } else if (key == "Icons") {
      auto& po = value.as_object();
      QMetaEnum e = m.enumerator(m.indexOfEnumerator("Icon"));
      for (int i = 0, total = e.keyCount(); i < total; ++i) {
        auto ekey = json::string_view(e.key(i));
        m_iconFiles[i] = asQString(po.at(ekey));
        QIcon(m_iconFiles[i]).swap(m_icons[i]);
      }
    }
  }

  QApplication::setPalette(palette());
}

std::pair<QColor, QString> ZTheme::readNamedColor(const QString& color) const
{
  if (m_palette.contains(color)) {
    return std::make_pair(m_palette.at(color), color);
  }

  const QColor col('#' + color);
  if (!col.isValid()) {
    throw ZException(fmt::format("theme color {} invalid", color));
  }
  return std::make_pair(col, QString());
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
