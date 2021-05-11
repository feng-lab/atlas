#pragma once

#include <QObject>

class QThread;

namespace nim {

class ZRPCService;

class ZServiceManager : public QObject
{
Q_OBJECT
public:
  explicit ZServiceManager(QObject* parent = nullptr);

  ~ZServiceManager() override;

public:
  enum ThreadType
  {
    EXTERNAL, // threadpool
    UI, // ui
    DB, // database
    IO, // file
    PUSH, // network-->
    RPC, // network<--
    LOGIC // logic,eg.ctpmgr
  };

  QThread* getThread(ThreadType p);

  bool isCurrentOn(ThreadType p);

  void checkCurrentOn(ThreadType p);

  ZRPCService* rpcService();

private:

  void dbThreadStarted();

  void ioThreadStarted();

  void pushThreadStarted();

  void rpcThreadStarted();

  void logicThreadStarted();

  void dbThreadFinished();

  void ioThreadFinished();

  void pushThreadFinished();

  void rpcThreadFinished();

  void logicThreadFinished();

private:
  void check();

  void init();

  void shutdown();

private:
  QThread* m_uiThread = nullptr;
  QThread* m_dbThread = nullptr;
  QThread* m_ioThread = nullptr;
  QThread* m_pushThread = nullptr;
  QThread* m_rpcThread = nullptr;
  QThread* m_logicThread = nullptr;

  ZRPCService* m_rpcService = nullptr;

  bool m_shutdown = false;
  bool m_init = false;
};

extern ZServiceManager* g_sm;

} // namespace nim


