#pragma once

#include <QObject>
#include <memory>
#include <string_view>
#include <thread>

namespace grpc {

class Server;
class Service;

} // namespace grpc

namespace nim {

class ZRpcUiDispatcher;

class ZRPCService : public QObject
{
  Q_OBJECT

public:
  explicit ZRPCService(std::string_view appVersion, QObject* parent = nullptr);

  ~ZRPCService() override; // defined in .cpp to keep grpc::Service complete

  // Install the UI dispatcher (lives on the UI thread). Must outlive the gRPC server.
  void setUiDispatcher(ZRpcUiDispatcher* dispatcher);

  void init();

  void shutdown();

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

  // Non-owning; owned by ZServiceManager on the UI thread.
  ZRpcUiDispatcher* m_uiDispatcher = nullptr;
  // Non-owning reference to the app's version string. Must outlive the RPC service.
  std::string_view m_appVersion;
};

} // namespace nim
