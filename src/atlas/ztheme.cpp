#include "ztheme.h"

#include "zjson.h"

#ifdef Q_OS_MACOS
#include <private/qcore_mac_p.h>
#endif

#include <QMetaEnum>
#include <QFile>
#include <QApplication>

namespace nim {

ZTheme& ZTheme::instance()
{
  static ZTheme theme;
  return theme;
}

ZTheme::ZTheme()
{
  const QMetaObject &m = *metaObject();
  m_colors.resize(m.enumerator(m.indexOfEnumerator("Color")).keyCount());
  m_icons.resize(m.enumerator(m.indexOfEnumerator("Icon")).keyCount());
  m_iconFiles.resize(m_icons.size());

#ifdef Q_OS_MACOS
  m_currentTheme = qt_mac_applicationIsInDarkMode() ? "dark" : "light";
#else
  m_currentTheme = "light";
#endif
  loadTheme(QString(":Resources/themes/%1.atlastheme").arg(m_currentTheme));
}

void ZTheme::loadTheme(const QString& fn)
{
  QFile file(fn);
  if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
    throw ZIOException(tr("Can not open file"));
  }

  QByteArray saveData = file.readAll();

  QJsonParseError jsonError;
  QJsonDocument loadDoc(QJsonDocument::fromJson(saveData, &jsonError));
  if (loadDoc.isNull() || loadDoc.isEmpty() || !loadDoc.isObject()) {
    throw ZIOException(QString("Incorrect file format <%1>").arg(jsonError.errorString()));
  }

  QJsonObject loadObj = loadDoc.object();
  if (!loadObj.contains("AtlasTheme") || !loadObj["AtlasTheme"].isObject()) {
    throw ZIOException(tr("File is not AtlasTheme format"));
  }

  QJsonObject themeObj = loadObj["AtlasTheme"].toObject();

  const QMetaObject &m = *metaObject();

  for (QJsonObject::const_iterator it = themeObj.begin(); it != themeObj.end(); ++it) {
    if (it.key() == "Palette") {
      auto po = it.value().toObject();
      for (auto pit = po.begin(); pit != po.end(); ++pit) {
        m_palette[pit.key()] = readNamedColor(pit.value().toString()).first;
      }
    }
  }

  for (QJsonObject::const_iterator it = themeObj.begin(); it != themeObj.end(); ++it) {
    if (it.key() == "Colors") {
      auto po = it.value().toObject();
      QMetaEnum e = m.enumerator(m.indexOfEnumerator("Color"));
      for (int i = 0, total = e.keyCount(); i < total; ++i) {
        const QString key = QLatin1String(e.key(i));
        if (!po.contains(key)) {
          continue;
        }
        m_colors[i] = readNamedColor(po[key].toString());
      }
    } else if (it.key() == "Icons") {
      auto po = it.value().toObject();
      QMetaEnum e = m.enumerator(m.indexOfEnumerator("Icon"));
      for (int i = 0, total = e.keyCount(); i < total; ++i) {
        const QString key = QLatin1String(e.key(i));
        m_iconFiles[i] = po[key].toString();
        m_icons[i] = QIcon(m_iconFiles[i]);
      }
    }
  }

  QApplication::setPalette(palette());
}

std::pair<QColor, QString> ZTheme::readNamedColor(const QString &color) const
{
  if (m_palette.find(color) != m_palette.end())
    return std::make_pair(m_palette.at(color), color);

  const QColor col('#' + color);
  if (!col.isValid()) {
    throw ZIOException(QString("theme color %1 invalid").arg(color));
  }
  return std::make_pair(col, QString());
}

QPalette ZTheme::palette() const
{
  QPalette pal = QApplication::palette();

  const static struct {
    Color themeColor;
    QPalette::ColorRole paletteColorRole;
    QPalette::ColorGroup paletteColorGroup;
    bool setColorRoleAsBrush;
  } mapping[] = {
    {PaletteWindow,                    QPalette::Window,           QPalette::All,      false},
    {PaletteWindowDisabled,            QPalette::Window,           QPalette::Disabled, false},
    {PaletteWindowText,                QPalette::WindowText,       QPalette::All,      true},
    {PaletteWindowTextDisabled,        QPalette::WindowText,       QPalette::Disabled, true},
    {PaletteBase,                      QPalette::Base,             QPalette::All,      false},
    {PaletteBaseDisabled,              QPalette::Base,             QPalette::Disabled, false},
    {PaletteAlternateBase,             QPalette::AlternateBase,    QPalette::All,      false},
    {PaletteAlternateBaseDisabled,     QPalette::AlternateBase,    QPalette::Disabled, false},
    {PaletteToolTipBase,               QPalette::ToolTipBase,      QPalette::All,      true},
    {PaletteToolTipBaseDisabled,       QPalette::ToolTipBase,      QPalette::Disabled, true},
    {PaletteToolTipText,               QPalette::ToolTipText,      QPalette::All,      false},
    {PaletteToolTipTextDisabled,       QPalette::ToolTipText,      QPalette::Disabled, false},
    {PaletteText,                      QPalette::Text,             QPalette::All,      true},
    {PaletteTextDisabled,              QPalette::Text,             QPalette::Disabled, true},
    {PaletteButton,                    QPalette::Button,           QPalette::All,      false},
    {PaletteButtonDisabled,            QPalette::Button,           QPalette::Disabled, false},
    {PaletteButtonText,                QPalette::ButtonText,       QPalette::All,      true},
    {PaletteButtonTextDisabled,        QPalette::ButtonText,       QPalette::Disabled, true},
    {PaletteBrightText,                QPalette::BrightText,       QPalette::All,      false},
    {PaletteBrightTextDisabled,        QPalette::BrightText,       QPalette::Disabled, false},
    {PaletteHighlight,                 QPalette::Highlight,        QPalette::All,      true},
    {PaletteHighlightDisabled,         QPalette::Highlight,        QPalette::Disabled, true},
    {PaletteHighlightedText,           QPalette::HighlightedText,  QPalette::All,      true},
    {PaletteHighlightedTextDisabled,   QPalette::HighlightedText,  QPalette::Disabled, true},
    {PaletteLink,                      QPalette::Link,             QPalette::All,      false},
    {PaletteLinkDisabled,              QPalette::Link,             QPalette::Disabled, false},
    {PaletteLinkVisited,               QPalette::LinkVisited,      QPalette::All,      false},
    {PaletteLinkVisitedDisabled,       QPalette::LinkVisited,      QPalette::Disabled, false},
    {PaletteLight,                     QPalette::Light,            QPalette::All,      false},
    {PaletteLightDisabled,             QPalette::Light,            QPalette::Disabled, false},
    {PaletteMidlight,                  QPalette::Midlight,         QPalette::All,      false},
    {PaletteMidlightDisabled,          QPalette::Midlight,         QPalette::Disabled, false},
    {PaletteDark,                      QPalette::Dark,             QPalette::All,      false},
    {PaletteDarkDisabled,              QPalette::Dark,             QPalette::Disabled, false},
    {PaletteMid,                       QPalette::Mid,              QPalette::All,      false},
    {PaletteMidDisabled,               QPalette::Mid,              QPalette::Disabled, false},
    {PaletteShadow,                    QPalette::Shadow,           QPalette::All,      false},
    {PaletteShadowDisabled,            QPalette::Shadow,           QPalette::Disabled, false}
  };

  for (auto entry: mapping) {
    const QColor themeColor = color(entry.themeColor);
    // Use original color if color is not defined in theme.
    if (themeColor.isValid()) {
      if (entry.setColorRoleAsBrush)
        pal.setBrush(entry.paletteColorGroup, entry.paletteColorRole, themeColor);
      else
        pal.setColor(entry.paletteColorGroup, entry.paletteColorRole, themeColor);
    }
  }

  return pal;
}

} // namespace nim
