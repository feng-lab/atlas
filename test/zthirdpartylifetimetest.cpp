#include <folly/portability/GMock.h>
#include <folly/portability/GTest.h>

#include <memory>
#include <string>
#include <utility>

#include "../src/3rdparty/build/include/fizz/client/AsyncFizzClient.h"
#include <folly/Conv.h>
#include <folly/coro/BlockingWait.h>
#include "../src/3rdparty/folly/folly/io/async/test/AsyncSocketTest.h"
#include "../src/3rdparty/build/include/proxygen/lib/http/coro/HTTPFixedSource.h"
#include "../src/3rdparty/build/include/proxygen/lib/http/coro/client/HTTPClient.h"
#include "../src/3rdparty/build/include/proxygen/lib/http/coro/client/HTTPCoroConnector.h"
#include "../src/3rdparty/build/include/proxygen/lib/http/coro/client/HTTPCoroSessionPool.h"
#include "../src/3rdparty/proxygen/proxygen/lib/http/coro/server/ScopedHTTPServer.h"
#include "../src/3rdparty/proxygen/proxygen/lib/http/coro/server/samples/fwdproxy/ConnectSource.h"

namespace {

using namespace std::chrono_literals;

class FizzAsyncClientLifetimeTest : public ::testing::Test
{
protected:
  using Client = fizz::client::AsyncFizzClient;

  void SetUp() override
  {
    context_ = std::make_shared<fizz::client::FizzClientContext>();
  }

  template<typename CloseFn>
  void expectCloseCancelsInFlightConnect(const char* closeLabel, CloseFn closeFn)
  {
    SCOPED_TRACE(closeLabel);

    folly::EventBase evb;
    folly::test::TestServer server;
    auto client = Client::UniquePtr(new Client(&evb, context_));
    folly::test::ConnCallback callback;

    client->connect(server.getAddress(), &callback, nullptr, std::string("www.example.com"), folly::none);

    closeFn(*client);
    EXPECT_EQ(callback.state, folly::test::STATE_FAILED);

    client.reset();
    evb.loopOnce(EVLOOP_NONBLOCK);
  }

  std::shared_ptr<fizz::client::FizzClientContext> context_;
};

TEST_F(FizzAsyncClientLifetimeTest, CloseDuringSocketConnectIsSafe)
{
  expectCloseCancelsInFlightConnect("close()", [](Client& client) {
    client.close();
  });
}

TEST_F(FizzAsyncClientLifetimeTest, CloseNowDuringSocketConnectIsSafe)
{
  expectCloseCancelsInFlightConnect("closeNow()", [](Client& client) {
    client.closeNow();
  });
}

TEST_F(FizzAsyncClientLifetimeTest, CloseWithResetDuringSocketConnectIsSafe)
{
  expectCloseCancelsInFlightConnect("closeWithReset()", [](Client& client) {
    client.closeWithReset();
  });
}

class TunnelUpstreamHandler : public proxygen::coro::HTTPHandler
{
public:
  explicit TunnelUpstreamHandler(folly::SocketAddress serverAddress)
    : serverAddress_(std::move(serverAddress))
  {}

  folly::coro::Task<proxygen::coro::HTTPSourceHolder>
  handleRequest(folly::EventBase* evb,
                proxygen::coro::HTTPSessionContextPtr /*ctx*/,
                proxygen::coro::HTTPSourceHolder requestSource) override
  {
    auto transport =
      co_await folly::coro::co_awaitTry(folly::coro::Transport::newConnectedSocket(evb, serverAddress_, 50ms));
    if (transport.hasException()) {
      co_yield folly::coro::co_error(std::move(transport.exception()));
    }

    auto connectSource = std::make_unique<proxygen::coro::ConnectSource>(
      std::make_unique<folly::coro::Transport>(std::move(transport).value()),
      std::move(requestSource));
    folly::coro::co_withExecutor(evb, connectSource->readRequestSendUpstream()).start();
    co_return connectSource.release();
  }

private:
  folly::SocketAddress serverAddress_;
};

class ProxyingHandler : public proxygen::coro::HTTPHandler
{
public:
  folly::coro::Task<proxygen::coro::HTTPSourceHolder>
  handleRequest(folly::EventBase* evb,
                proxygen::coro::HTTPSessionContextPtr ctx,
                proxygen::coro::HTTPSourceHolder requestSource) override
  {
    auto headerEvent = co_await folly::coro::co_awaitTry(requestSource.readHeaderEvent());
    if (headerEvent.hasException()) {
      co_yield folly::coro::co_error(std::move(headerEvent.exception()));
    }

    auto method = headerEvent->headers->getMethod();
    if (method == proxygen::HTTPMethod::CONNECT) {
      if (headerEvent->eom) {
        co_yield folly::coro::co_error(
          proxygen::coro::HTTPError{proxygen::coro::HTTPErrorCode::REFUSED_STREAM, "eom in CONNECT request"});
      }

      auto connectRes = co_await folly::coro::co_awaitTry(
        TunnelUpstreamHandler(serverAddress_).handleRequest(evb, std::move(ctx), std::move(requestSource)));
      if (connectRes.hasException()) {
        co_yield folly::coro::co_error(std::move(connectRes.exception()));
      }
      co_return std::move(connectRes).value();
    }

    co_return proxygen::coro::HTTPFixedSource::makeFixedResponse(200, "ok");
  }

  folly::SocketAddress serverAddress_;
};

class ProxygenConnectLifetimeTest : public ::testing::Test
{
protected:
  void SetUp() override
  {
    proxygen::coro::HTTPServer::Config serverConfig;
    serverConfig.socketConfig.bindAddress.setFromIpPort("127.0.0.1", 0);

    server_ = proxygen::coro::ScopedHTTPServer::start(std::move(serverConfig), handler_);
    ASSERT_NE(server_, nullptr);
    ASSERT_TRUE(server_->address().has_value());
    handler_->serverAddress_ = *server_->address();
  }

  proxygen::coro::HTTPCoroConnector::ConnectionParams getConnParams() const
  {
    return proxygen::coro::HTTPClient::getConnParams(proxygen::coro::HTTPClient::SecureTransportImpl::NONE);
  }

  folly::SocketAddress serverAddress() const
  {
    return handler_->serverAddress_;
  }

  folly::EventBase evb_;
  std::shared_ptr<ProxyingHandler> handler_{std::make_shared<ProxyingHandler>()};
  std::unique_ptr<proxygen::coro::ScopedHTTPServer> server_;
};

TEST_F(ProxygenConnectLifetimeTest, PooledTunnelSessionSurvivesPoolDestruction)
{
  folly::coro::blockingWait(
    [&]() -> folly::coro::Task<void> {
      auto proxyPool =
        std::make_shared<proxygen::coro::HTTPCoroSessionPool>(&evb_,
                                                              serverAddress(),
                                                              proxygen::coro::HTTPCoroSessionPool::defaultPoolParams(),
                                                              getConnParams());
      auto proxySessionRes = co_await folly::coro::co_awaitTry(proxyPool->getSessionWithReservation());
      if (proxySessionRes.hasException()) {
        ADD_FAILURE() << "Failed to get proxy session: " << proxySessionRes.exception().what();
        co_return;
      }

      auto proxySession = std::move(proxySessionRes).value();
      auto tunneledSessionRes = co_await folly::coro::co_awaitTry(proxygen::coro::HTTPCoroConnector::proxyConnect(
        proxySession.session,
        std::move(proxySession.reservation),
        folly::to<std::string>(serverAddress().getAddressStr(), ":", serverAddress().getPort()),
        /*connectUnique=*/false,
        50ms,
        getConnParams(),
        proxygen::coro::HTTPClient::getSessionParams()));
      if (tunneledSessionRes.hasException()) {
        ADD_FAILURE() << "Failed to create tunneled session: " << tunneledSessionRes.exception().what();
        co_return;
      }

      auto* tunneledSession = *tunneledSessionRes;
      auto tunneledSessionCtx = tunneledSession->acquireKeepAlive();
      auto reservation = tunneledSession->reserveRequest();
      if (reservation.hasException()) {
        ADD_FAILURE() << "Failed to reserve tunneled request: " << reservation.exception().what();
        co_return;
      }

      proxyPool.reset();

      auto tunneledUrl = proxygen::URL(folly::to<std::string>("http://localhost:", serverAddress().getPort(), "/"));
      auto response = co_await folly::coro::co_awaitTry(
        proxygen::coro::HTTPClient::get(tunneledSession, std::move(*reservation), tunneledUrl));
      if (response.hasException()) {
        ADD_FAILURE() << "Request via pooled CONNECT tunnel failed after pool "
                         "destruction: "
                      << response.exception().what();
        co_return;
      }

      EXPECT_EQ(response->headers->getStatusCode(), 200);

      tunneledSession->initiateDrain();
      co_await folly::coro::co_reschedule_on_current_executor;
    }(),
    &evb_);
}

} // namespace
