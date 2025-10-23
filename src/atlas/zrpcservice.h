#pragma once

#include <QObject>
#include <grpcpp/server.h>

namespace nim {

class ZRPCService : public QObject
{
  Q_OBJECT

public:
  explicit ZRPCService(QObject* parent = nullptr);

  void init();

  void shutdown();

Q_SIGNALS:

public:

private:
  void onRPCThreadStarted();

private:
  grpc::Server* m_grpcServer = nullptr;
};

} // namespace nim
