#pragma once

class QWidget;

namespace nim {

class ZMainWindow;

class ZAppRestartController
{
public:
  [[nodiscard]] static bool requestRestart(ZMainWindow& mainWindow, QWidget* promptParent = nullptr);

  [[nodiscard]] static bool isRestartShutdownInProgress();
};

} // namespace nim
