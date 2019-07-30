#pragma once

#include <cstdint>
#include <list>
#include <memory>

#include "envoy/event/deferred_deletable.h"
#include "envoy/event/timer.h"
#include "envoy/http/conn_pool.h"
#include "envoy/network/connection.h"
#include "envoy/stats/timespan.h"
#include "envoy/upstream/upstream.h"

#include "common/common/linked_object.h"
#include "common/http/codec_client.h"
#include "common/http/codec_wrappers.h"
#include "common/http/conn_pool_base.h"

#include "absl/types/optional.h"

namespace Envoy {
namespace Http {
namespace Httpx {

/**
 * A connection pool implementation for HTTP1 and HTTP2 auto upgrade.
 * Overview
 * When the cluster configuration is default or not specified HTTP2 explicitly,
 * and some flag-guarded configuration enables this httpx feature, the request
 * will be bounded to this pool.
 *
 * The auto upgrade happens within two place,
 * - When ActiveClient request createCodeClient, with additional data, TLS ALPN
 *   informatin can help determine what CodecClient will be used.
 * - Data plane H2C negotiation will be used. TODO: someone can help.
 *
 * TODO(incfly) Work Item
 * - [Done] Finish E2E TLS + H2 config for setup example.
 * - Basic HTTP1 request handled, all use cases. (just copy paste should be good).
 * - Log out the data.nextProtocol == h2 case, printf verify works.
 * - Copy Http2 Client handling stuff.
 * - Flag guard? runtime feature?
 */
class ConnPoolImpl : public ConnectionPool::Instance, public ConnPoolImplBase {
public:
  ConnPoolImpl(Event::Dispatcher& dispatcher, Upstream::HostConstSharedPtr host,
               Upstream::ResourcePriority priority,
               const Network::ConnectionSocket::OptionsSharedPtr& options);

  ~ConnPoolImpl() override;

  // ConnectionPool::Instance
  Http::Protocol protocol() const override { return Http::Protocol::Http11; }
  void addDrainedCallback(DrainedCb cb) override;
  void drainConnections() override;
  bool hasActiveConnections() const override;
  ConnectionPool::Cancellable* newStream(StreamDecoder& response_decoder,
                                         ConnectionPool::Callbacks& callbacks) override;
  Upstream::HostDescriptionConstSharedPtr host() const override { return host_; };

  // ConnPoolImplBase
  void checkForDrained() override;

protected:
  struct ActiveClient;

  struct StreamWrapper : public StreamEncoderWrapper,
                         public StreamDecoderWrapper,
                         public StreamCallbacks {
    StreamWrapper(StreamDecoder& response_decoder, ActiveClient& parent);
    ~StreamWrapper() override;

    // StreamEncoderWrapper
    void onEncodeComplete() override;

    // StreamDecoderWrapper
    void decodeHeaders(HeaderMapPtr&& headers, bool end_stream) override;
    void onPreDecodeComplete() override {}
    void onDecodeComplete() override;

    // Http::StreamCallbacks
    void onResetStream(StreamResetReason, absl::string_view) override {
      parent_.parent_.onDownstreamReset(parent_);
    }
    void onAboveWriteBufferHighWatermark() override {}
    void onBelowWriteBufferLowWatermark() override {}

    ActiveClient& parent_;
    bool encode_complete_{};
    bool close_connection_{};
    bool decode_complete_{};
  };

  using StreamWrapperPtr = std::unique_ptr<StreamWrapper>;

  struct ActiveClient : LinkedObject<ActiveClient>,
                        public Network::ConnectionCallbacks,
                        public Event::DeferredDeletable {
    ActiveClient(ConnPoolImpl& parent);
    ~ActiveClient() override;

    void onConnectTimeout();

    // Network::ConnectionCallbacks
    void onEvent(Network::ConnectionEvent event) override {
      parent_.onConnectionEvent(*this, event);
    }
    void onAboveWriteBufferHighWatermark() override {}
    void onBelowWriteBufferLowWatermark() override {}

    ConnPoolImpl& parent_;
    CodecClientPtr codec_client_;
    Upstream::HostDescriptionConstSharedPtr real_host_description_;
    StreamWrapperPtr stream_wrapper_;
    Event::TimerPtr connect_timer_;
    Stats::TimespanPtr conn_length_;
    uint64_t remaining_requests_;
  };

  using ActiveClientPtr = std::unique_ptr<ActiveClient>;

  void attachRequestToClient(ActiveClient& client, StreamDecoder& response_decoder,
                             ConnectionPool::Callbacks& callbacks);
  virtual CodecClientPtr createCodecClient(Upstream::Host::CreateConnectionData& data) PURE;
  void createNewConnection();
  void onConnectionEvent(ActiveClient& client, Network::ConnectionEvent event);
  void onDownstreamReset(ActiveClient& client);
  void onResponseComplete(ActiveClient& client);
  void onUpstreamReady();
  void processIdleClient(ActiveClient& client, bool delay);

  Stats::TimespanPtr conn_connect_ms_;
  Event::Dispatcher& dispatcher_;
  std::list<ActiveClientPtr> ready_clients_;
  std::list<ActiveClientPtr> busy_clients_;
  std::list<DrainedCb> drained_callbacks_;
  const Network::ConnectionSocket::OptionsSharedPtr socket_options_;
  Event::TimerPtr upstream_ready_timer_;
  bool upstream_ready_enabled_{false};
};

/**
 * Production implementation of the ConnPoolImpl.
 */
class ProdConnPoolImpl : public ConnPoolImpl {
public:
  ProdConnPoolImpl(Event::Dispatcher& dispatcher, Upstream::HostConstSharedPtr host,
                   Upstream::ResourcePriority priority,
                   const Network::ConnectionSocket::OptionsSharedPtr& options)
      : ConnPoolImpl(dispatcher, host, priority, options) {}

  // ConnPoolImpl
  CodecClientPtr createCodecClient(Upstream::Host::CreateConnectionData& data) override;
};

} // namespace Httpx
} // namespace Http
} // namespace Envoy
