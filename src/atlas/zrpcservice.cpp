#include "zrpcservice.h"

#include "helloworld.grpc.pb.h"
#include "zservicemanager.h"
#include "zlog.h"
#include <QThread>
#include <QtCore/QDebug>
#include <grpcpp/grpcpp.h>

namespace nim {

using grpc::Server;
using grpc::ServerBuilder;
using grpc::ServerContext;
using grpc::Status;
using helloworld::HelloRequest;
using helloworld::HelloReply;
using helloworld::Greeter;

// Logic and data behind the server's behavior.
class GreeterServiceImpl final : public Greeter::Service {
  Status SayHello(ServerContext* context, const HelloRequest* request, HelloReply* reply) override {
    // Overwrite the call's compression algorithm to DEFLATE.
    context->set_compression_algorithm(GRPC_COMPRESS_DEFLATE);
    std::string prefix("Hello ");
    reply->set_message(prefix + request->name());
    return Status::OK;
  }
};

ZRPCService::ZRPCService(QObject* parent)
  : QObject(parent)
{
}

void ZRPCService::init()
{
  g_sm->checkCurrentOn(ZServiceManager::RPC);

  m_rpcThread = new QThread();
  QObject::connect(m_rpcThread, &QThread::started, this, &ZRPCService::onRPCThreadStarted, Qt::DirectConnection);
  m_rpcThread->start();
}

void ZRPCService::shutdown()
{
  g_sm->checkCurrentOn(ZServiceManager::RPC);

  m_grpcServer->Shutdown();
  m_grpcServer = nullptr;

  m_rpcThread->quit();
  m_rpcThread->wait();
  delete m_rpcThread;
  m_rpcThread = nullptr;
}

void ZRPCService::onRPCThreadStarted()
{
  if (g_sm->isCurrentOn(ZServiceManager::RPC)) {
    qFatal("g_sm->CurrentOn(ZServiceManager::RPC)");
  }

  std::string server_address("0.0.0.0:50051");
  GreeterServiceImpl service;

  grpc::ServerBuilder builder;
  // Set the default compression algorithm for the server.
  builder.SetDefaultCompressionAlgorithm(GRPC_COMPRESS_GZIP);
  // Listen on the given address without any authentication mechanism.
  builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
  // Register "service" as the instance through which we'll communicate with
  // clients. In this case it corresponds to an *synchronous* service.
  builder.RegisterService(&service);
  // Finally assemble the server.
  std::unique_ptr<Server> server(builder.BuildAndStart());
  LOG(INFO) << "Server listening on " << server_address;
  m_grpcServer = server.get();
  // Wait for the server to shutdown. Note that some other thread must be
  // responsible for shutting down the server for this call to ever return.
  server->Wait();
  LOG(INFO) << "Server shutdown";
}

} // namespace nim

