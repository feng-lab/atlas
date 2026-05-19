#pragma once

#include <QObject>
#include <string_view>

class QThread;

namespace nim {

class ZRPCService;
class ZRpcUiDispatcher;
class ZMainWindow;

class ZServiceManager : public QObject
{
  Q_OBJECT

public:
  explicit ZServiceManager(std::string_view appVersion, QObject* parent = nullptr);

  ~ZServiceManager() override;

public:
  enum ThreadType
  {
    UI, // ui
    RPC, // network<--
  };

  bool isCurrentOn(ThreadType p);

  void checkCurrentOn(ThreadType p);

  ZRPCService* rpcService();

  // UI-thread dispatcher for RPCs. The object is owned by the service manager
  // and lives on the UI thread for the app lifetime.
  ZRpcUiDispatcher* rpcUiDispatcher();

  // Register the app's main window so RPC UI dispatch can avoid scanning and
  // safely coordinate document/view lifecycle.
  void setMainWindow(ZMainWindow* mainWindow);

private:
  void rpcThreadStarted();

  void rpcThreadFinished();

private:
  void check();

  void init();

  void shutdown();

private:
  QThread* m_uiThread = nullptr;
  QThread* m_rpcThread = nullptr;

  ZRPCService* m_rpcService = nullptr;
  ZRpcUiDispatcher* m_rpcUiDispatcher = nullptr;

  // Non-owning reference to the app's version string. Must outlive the service manager.
  // We pass the build-stamped `GIT_VERSION` string literal from main.cpp, so lifetime is the entire process.
  std::string_view m_appVersion;

  bool m_shutdown = false;
  bool m_init = false;
};

} // namespace nim
