#include "zservicemanager.h"

#include "zrpcservice.h"
#include "zrpcuidispatcher.h"
#include "zmainwindow.h"
#include "zdoc.h"
#include "zlog.h"
#include <QMetaObject>
#include <QThread>

namespace nim {

ZServiceManager::ZServiceManager(std::string_view appVersion, QObject* parent)
  : QObject(parent)
  , m_appVersion(appVersion)
{
  CHECK(!m_appVersion.empty());
  init();
}

ZServiceManager::~ZServiceManager()
{
  shutdown();
}

void ZServiceManager::init()
{
  CHECK(!m_init);
  m_init = true;

  m_uiThread = QThread::currentThread();

  // Create the UI dispatcher before starting worker threads so it is available
  // immediately for RPC calls, and so construction doesn't rely on the UI
  // event loop being active yet.
  m_rpcUiDispatcher = new ZRpcUiDispatcher(this);

  m_rpcThread = new QThread;
  m_rpcThread->setObjectName("RPCThread");

  m_rpcService = new ZRPCService(m_appVersion, this);
  m_rpcService->setUiDispatcher(m_rpcUiDispatcher);
  m_rpcService->moveToThread(m_rpcThread);

  QObject::connect(m_rpcThread, &QThread::started, this, &ZServiceManager::rpcThreadStarted, Qt::DirectConnection);
  QObject::connect(m_rpcThread, &QThread::finished, this, &ZServiceManager::rpcThreadFinished, Qt::DirectConnection);
  m_rpcThread->start();
}

void ZServiceManager::rpcThreadStarted()
{
  checkCurrentOn(RPC);
  m_rpcService->init();
}

void ZServiceManager::rpcThreadFinished()
{
  checkCurrentOn(RPC);
  m_rpcService->shutdown();
  m_rpcService->moveToThread(m_uiThread);
}

ZRPCService* ZServiceManager::rpcService()
{
  check();

  return this->m_rpcService;
}

ZRpcUiDispatcher* ZServiceManager::rpcUiDispatcher()
{
  check();
  return m_rpcUiDispatcher;
}

void ZServiceManager::setMainWindow(ZMainWindow* mainWindow)
{
  check();
  CHECK(isCurrentOn(UI));
  CHECK(m_rpcUiDispatcher);
  m_rpcUiDispatcher->setMainWindow(mainWindow);
}

void ZServiceManager::shutdown()
{
  CHECK(!m_shutdown);

  // Stop gRPC server running on the RPC thread, then stop the thread itself.
  if (m_rpcService) {
    QMetaObject::invokeMethod(
      m_rpcService,
      [this]() {
        m_rpcService->shutdown();
      },
      Qt::BlockingQueuedConnection);
  }
  m_rpcThread->quit();
  m_rpcThread->wait();
  delete m_rpcThread;
  m_rpcThread = nullptr;

  delete m_rpcService;
  m_rpcService = nullptr;

  m_uiThread = nullptr;

  m_shutdown = true;
}

void ZServiceManager::check()
{
  CHECK(!m_shutdown && m_init);
}

bool ZServiceManager::isCurrentOn(ZServiceManager::ThreadType p)
{
  check();

  QThread* cur = QThread::currentThread();
  if (p == ZServiceManager::UI && cur == m_uiThread) {
    return true;
  }

  if (p == ZServiceManager::RPC && cur == m_rpcThread) {
    return true;
  }

  return false;
}

void ZServiceManager::checkCurrentOn(ThreadType p)
{
  CHECK(isCurrentOn(p));
}

} // namespace nim
