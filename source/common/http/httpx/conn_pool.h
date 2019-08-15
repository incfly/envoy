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
 * - Data plane H2C negotiation will be used. TODO(incfly): log an issue for this.
 *
 * TODO(incfly) Work Item
 * - [Done] Finish E2E TLS + H2 config for setup example.
 * - [Done] HTTP1 request routed to httpx conn pool.
 * - [Done] log out the ALPN in a timer callback, shows http/1.1.
 *   [TODO] refator to create codec client in onConnectionEvent callback.
 * - Copy Http2 Client handling stuff.
 * - Handling API change for this feauture in cluster.proto.
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


  // HTTP2 hack methods copied.
  // TODO(incfly): remove hack.
  // Http::ConnectionPool::Instance
  // TODO(incfly): here, see how it's called.
  Http::Protocol protocol_todo() const { return Http::Protocol::Http2; }
  //void addDrainedCallback(DrainedCb cb) override;
  //void drainConnections() override;
  //bool hasActiveConnections() const override;
  //ConnectionPool::Cancellable* newStream(Http::StreamDecoder& response_decoder,
                                         //ConnectionPool::Callbacks& callbacks) override;
  // Upstream::HostDescriptionConstSharedPtr host() const override { return host_; };

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

  // TODO(incfly): some methods are still different than http2's.
  // OnCnnectoinTimeOut e.g
  struct ActiveClient : LinkedObject<ActiveClient>,
                        public Network::ConnectionCallbacks,
                        public Event::DeferredDeletable ,
                        public Http::ConnectionCallbacks,
                        public CodecClientCallbacks {
    ActiveClient(ConnPoolImpl& parent);
    ~ActiveClient() override;

    void onConnectTimeout();

    // Network::ConnectionCallbacks
    void onEvent(Network::ConnectionEvent event) override {
      parent_.onConnectionEvent(*this, event);
    }
    void onAboveWriteBufferHighWatermark() override {}
    void onBelowWriteBufferLowWatermark() override {}



    // HTTP2 hack
    // TODO(incfly): here!
    // parent conn pool implementation is as same as one here. considering
    // put in base class for sharing.
    // void onConnectTimeout() { parent_.onConnectTimeout(*this); }

    // Network::ConnectionCallbacks
    //void onEvent(Network::ConnectionEvent event) override {
      //parent_.onConnectionEvent(*this, event);
    //}
    //void onAboveWriteBufferHighWatermark() override {}
    //void onBelowWriteBufferLowWatermark() override {}

    // CodecClientCallbacks
    void onStreamDestroy() override { parent_.onStreamDestroy(*this); }
    void onStreamReset(Http::StreamResetReason reason) override {
      parent_.onStreamReset(*this, reason);
    }

    // Http::ConnectionCallbacks
    void onGoAway() override { parent_.onGoAway(*this); }


    ConnPoolImpl& parent_;
    CodecClientPtr codec_client_;
    Upstream::HostDescriptionConstSharedPtr real_host_description_;
    StreamWrapperPtr stream_wrapper_;
    Event::TimerPtr connect_timer_;
    Event::TimerPtr alpn_debug_timer_;
    Stats::TimespanPtr conn_length_;
    uint64_t remaining_requests_;

    // TODO(incfly): clean up before merging. 
    // HTTP2 fields
    // ConnPoolImpl& parent_;
    // CodecClientPtr client_;
    // Upstream::HostDescriptionConstSharedPtr real_host_description_;
    uint64_t total_streams_{};
    // Event::TimerPtr connect_timer_;
    bool upstream_ready_{};
    //Stats::TimespanPtr conn_length_;
    bool closed_with_active_rq_{};
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


  // HTTP2 hackk
  //void checkForDrained() override;

  //virtual CodecClientPtr createCodecClient(Upstream::Host::CreateConnectionData& data) PURE;
  virtual uint32_t maxTotalStreams() PURE;
  void movePrimaryClientToDraining();
  //void onConnectionEvent(ActiveClient& client, Network::ConnectionEvent event);
  void onConnectTimeout(ActiveClient& client);
  void onGoAway(ActiveClient& client);
  void onStreamDestroy(ActiveClient& client);
  void onStreamReset(ActiveClient& client, Http::StreamResetReason reason);
  void newClientStream(Http::StreamDecoder& response_decoder, ConnectionPool::Callbacks& callbacks);
  //void onUpstreamReady();

  Stats::TimespanPtr conn_connect_ms_;
  Event::Dispatcher& dispatcher_;
  std::list<ActiveClientPtr> ready_clients_;
  std::list<ActiveClientPtr> busy_clients_;
  std::list<DrainedCb> drained_callbacks_;
  const Network::ConnectionSocket::OptionsSharedPtr socket_options_;
  Event::TimerPtr upstream_ready_timer_;
  bool upstream_ready_enabled_{false};

  // HTTP2 fields.
  // TODO(incfly): remove this hack
  // Stats::TimespanPtr conn_connect_ms_;
  // Event::Dispatcher& dispatcher_;
  ActiveClientPtr primary_client_;
  ActiveClientPtr draining_client_;
  // std::list<DrainedCb> drained_callbacks_;
  // const Network::ConnectionSocket::OptionsSharedPtr socket_options_;
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

  uint32_t maxTotalStreams() override;

  // All streams are 2^31. Client streams are half that, minus stream 0. Just to be on the safe
  // side we do 2^29.
  static const uint64_t MAX_STREAMS = (1 << 29);
};

} // namespace Httpx
} // namespace Http
} // namespace Envoy
