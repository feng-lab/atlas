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

signals:

public slots:

private slots:
  void onRPCThreadStarted();

private:
  QThread* m_rpcThread = nullptr;
  grpc::Server* m_grpcServer = nullptr;
};

} // namespace nim


