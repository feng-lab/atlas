#include "zservicemanager.h"

#include "zrpcservice.h"
#include "zlog.h"
#include <QThread>
#include <QThreadPool>

namespace nim {

ZServiceManager* g_sm = nullptr;

ZServiceManager::ZServiceManager(QObject* parent)
  : QObject(parent)
{
  g_sm = this;
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

  const int threadCount = QThreadPool::globalInstance()->maxThreadCount();
  QThreadPool::globalInstance()->setMaxThreadCount(qMax(4, 2 * threadCount));

  m_uiThread = QThread::currentThread();
  m_ioThread = new QThread;
  m_dbThread = new QThread;
  m_pushThread = new QThread;
  m_rpcThread = new QThread;
  m_logicThread = new QThread;

  m_rpcService = new ZRPCService;
  m_rpcService->moveToThread(m_rpcThread);

  QObject::connect(m_ioThread, &QThread::started, this, &ZServiceManager::ioThreadStarted, Qt::DirectConnection);
  QObject::connect(m_dbThread, &QThread::started, this, &ZServiceManager::dbThreadStarted, Qt::DirectConnection);
  QObject::connect(m_pushThread, &QThread::started, this, &ZServiceManager::pushThreadStarted, Qt::DirectConnection);
  QObject::connect(m_rpcThread, &QThread::started, this, &ZServiceManager::rpcThreadStarted, Qt::DirectConnection);
  QObject::connect(m_logicThread, &QThread::started, this, &ZServiceManager::logicThreadStarted, Qt::DirectConnection);
  QObject::connect(m_ioThread, &QThread::finished, this, &ZServiceManager::ioThreadFinished, Qt::DirectConnection);
  QObject::connect(m_dbThread, &QThread::finished, this, &ZServiceManager::dbThreadFinished, Qt::DirectConnection);
  QObject::connect(m_pushThread, &QThread::finished, this, &ZServiceManager::pushThreadFinished, Qt::DirectConnection);
  QObject::connect(m_rpcThread, &QThread::finished, this, &ZServiceManager::rpcThreadFinished, Qt::DirectConnection);
  QObject::connect(m_logicThread,
                   &QThread::finished,
                   this,
                   &ZServiceManager::logicThreadFinished,
                   Qt::DirectConnection);
  m_ioThread->start();
  m_dbThread->start();
  m_pushThread->start();
  m_rpcThread->start();
  m_logicThread->start();
}

void ZServiceManager::ioThreadStarted()
{
  checkCurrentOn(IO);
}

void ZServiceManager::ioThreadFinished()
{
  checkCurrentOn(IO);
}

void ZServiceManager::dbThreadStarted()
{
  checkCurrentOn(DB);
}

void ZServiceManager::dbThreadFinished()
{
  checkCurrentOn(DB);
}

void ZServiceManager::pushThreadStarted()
{
  checkCurrentOn(PUSH);
}

void ZServiceManager::pushThreadFinished()
{
  checkCurrentOn(PUSH);
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

void ZServiceManager::logicThreadStarted()
{
  checkCurrentOn(LOGIC);
}

void ZServiceManager::logicThreadFinished()
{
  checkCurrentOn(LOGIC);
}

ZRPCService* ZServiceManager::rpcService()
{
  check();

  return this->m_rpcService;
}

void ZServiceManager::shutdown()
{
  CHECK(!m_shutdown);

  QThreadPool::globalInstance()->waitForDone();

  m_logicThread->quit();
  m_logicThread->wait();
  delete m_logicThread;
  m_logicThread = nullptr;

  m_rpcThread->quit();
  m_rpcThread->wait();
  delete m_rpcThread;
  m_rpcThread = nullptr;

  m_pushThread->quit();
  m_pushThread->wait();
  delete m_pushThread;
  m_pushThread = nullptr;

  m_dbThread->quit();
  m_dbThread->wait();
  delete m_dbThread;
  m_dbThread = nullptr;

  m_ioThread->quit();
  m_ioThread->wait();
  delete m_ioThread;
  m_ioThread = nullptr;

  delete m_rpcService;
  m_rpcService = nullptr;

  m_uiThread = nullptr;

  m_shutdown = true;
}

void ZServiceManager::check()
{
  CHECK(!m_shutdown && m_init);
}

QThread* ZServiceManager::getThread(ThreadType p)
{
  check();

  if (p == ZServiceManager::UI) {
    return this->m_uiThread;
  }
  if (p == ZServiceManager::IO) {
    return this->m_ioThread;
  }
  if (p == ZServiceManager::DB) {
    return this->m_dbThread;
  }
  if (p == ZServiceManager::PUSH) {
    return this->m_pushThread;
  }
  if (p == ZServiceManager::RPC) {
    return this->m_rpcThread;
  }
  if (p == ZServiceManager::LOGIC) {
    return this->m_logicThread;
  }

  CHECK(false);
  return nullptr;
}

bool ZServiceManager::isCurrentOn(ZServiceManager::ThreadType p)
{
  check();

  QThread* cur = QThread::currentThread();
  if (p == ZServiceManager::UI && cur == m_uiThread) {
    return true;
  }

  if (p == ZServiceManager::DB && cur == m_dbThread) {
    return true;
  }

  if (p == ZServiceManager::IO && cur == m_ioThread) {
    return true;
  }

  if (p == ZServiceManager::PUSH && cur == m_pushThread) {
    return true;
  }

  if (p == ZServiceManager::RPC && cur == m_rpcThread) {
    return true;
  }

  if (p == ZServiceManager::LOGIC && cur == m_logicThread) {
    return true;
  }

  if (p == ZServiceManager::EXTERNAL) {
    if (cur != m_uiThread && cur != m_dbThread && cur != m_ioThread && cur != m_pushThread && cur != m_rpcThread &&
        cur != m_logicThread) {
      return true;
    }
  }

  return false;
}

void ZServiceManager::checkCurrentOn(ThreadType p)
{
  CHECK(isCurrentOn(p));
}

} // namespace nim
