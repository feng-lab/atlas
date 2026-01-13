#pragma once

#include <QObject>
#include <grpcpp/server.h>
#include <QPointer>
#include <memory>
#include <thread>
#include "zdoc.h"
#include "z3drenderingengine.h"

namespace nim {

class ZRPCService : public QObject
{
  Q_OBJECT

public:
  explicit ZRPCService(QObject* parent = nullptr);

  ~ZRPCService() override; // defined in .cpp to keep grpc::Service complete

  void init();

  void shutdown();

  // Context setters (UI thread). Safe to call before/after init.
  void setDoc(ZDoc* doc)
  {
    m_doc = doc;
  }

  void setEngine(Z3DRenderingEngine* engine)
  {
    m_engine = engine;
  }

  ZDoc* doc() const { return m_doc; }
  Z3DRenderingEngine* engine() const { return m_engine; }

Q_SIGNALS:

public:

private:
  void onRPCThreadStarted();

private:
  grpc::Server* m_grpcServer = nullptr; // non-owning, points into m_serverOwned
  std::unique_ptr<grpc::Server> m_serverOwned;
  std::thread m_waitThread;
  // Own service instances to guarantee lifetime >= server lifetime
  std::unique_ptr<grpc::Service> m_sceneService;
  QPointer<ZDoc> m_doc;
  QPointer<Z3DRenderingEngine> m_engine;
};

} // namespace nim
