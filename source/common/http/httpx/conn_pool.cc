#include "common/http/httpx/conn_pool.h"

#include <cstdint>
#include <list>
#include <memory>

#include "envoy/event/dispatcher.h"
#include "envoy/event/timer.h"
#include "envoy/http/header_map.h"
#include "envoy/upstream/upstream.h"

#include "common/common/utility.h"
#include "common/http/codec_client.h"
#include "common/http/codes.h"
#include "common/http/headers.h"
#include "common/network/utility.h"
#include "common/runtime/runtime_impl.h"
#include "common/upstream/upstream_impl.h"

#include "absl/strings/match.h"

namespace Envoy {
namespace Http {
namespace Httpx {

ConnPoolImpl::ConnPoolImpl(Event::Dispatcher& dispatcher, Upstream::HostConstSharedPtr host,
                           Upstream::ResourcePriority priority,
                           const Network::ConnectionSocket::OptionsSharedPtr& options)
    : ConnPoolImplBase(std::move(host), std::move(priority)), dispatcher_(dispatcher),
      socket_options_(options),
      upstream_ready_timer_(dispatcher_.createTimer([this]() { onUpstreamReady(); })),
      http_protocol_(CodecClient::Type::HTTP1) {}

ConnPoolImpl::~ConnPoolImpl() {
  while (!ready_clients_.empty()) {
    ready_clients_.front()->codec_client_->close();
  }

  while (!busy_clients_.empty()) {
    busy_clients_.front()->codec_client_->close();
  }

  // Make sure all clients are destroyed before we are destroyed.
  dispatcher_.clearDeferredDeleteList();
}

void ConnPoolImpl::drainConnections() {
  while (!ready_clients_.empty()) {
    ready_clients_.front()->codec_client_->close();
  }

  // We drain busy clients by manually setting remaining requests to 1. Thus, when the next
  // response completes the client will be destroyed.
  for (const auto& client : busy_clients_) {
    client->remaining_requests_ = 1;
  }
}

void ConnPoolImpl::addDrainedCallback(DrainedCb cb) {
  drained_callbacks_.push_back(cb);
  checkForDrained();
}

bool ConnPoolImpl::hasActiveConnections() const {
  return !pending_requests_.empty() || !busy_clients_.empty();
}

void ConnPoolImpl::attachRequestToClient(ActiveClient& client, StreamDecoder& response_decoder,
                                         ConnectionPool::Callbacks& callbacks) {
  ASSERT(!client.stream_wrapper_);
  host_->cluster().stats().upstream_rq_total_.inc();
  host_->stats().rq_total_.inc();
  client.stream_wrapper_ = std::make_unique<StreamWrapper>(response_decoder, client);
  callbacks.onPoolReady(*client.stream_wrapper_, client.real_host_description_);
}

void ConnPoolImpl::checkForDrained() {
  if (!drained_callbacks_.empty() && pending_requests_.empty() && busy_clients_.empty()) {
    while (!ready_clients_.empty()) {
      ready_clients_.front()->codec_client_->close();
    }

    for (const DrainedCb& cb : drained_callbacks_) {
      cb();
    }
  }
}

void ConnPoolImpl::createNewConnection() {
  ENVOY_LOG(debug, "creating a new connection");
  ActiveClientPtr client(new ActiveClient(*this));
  client->moveIntoList(std::move(client), busy_clients_);
}

ConnectionPool::Cancellable* ConnPoolImpl::newStream(StreamDecoder& response_decoder,
                                                     ConnectionPool::Callbacks& callbacks) {
  if (http_protocol_ == CodecClient::Type::HTTP1) {
		if (!ready_clients_.empty()) {
			ready_clients_.front()->moveBetweenLists(ready_clients_, busy_clients_);
			ENVOY_CONN_LOG(debug, "using existing connection", *busy_clients_.front()->codec_client_);
			attachRequestToClient(*busy_clients_.front(), response_decoder, callbacks);
			return nullptr;
		}

		if (host_->cluster().resourceManager(priority_).pendingRequests().canCreate()) {
			bool can_create_connection =
					host_->cluster().resourceManager(priority_).connections().canCreate();
			if (!can_create_connection) {
				host_->cluster().stats().upstream_cx_overflow_.inc();
			}

			// If we have no connections at all, make one no matter what so we don't starve.
			if ((ready_clients_.empty() && busy_clients_.empty()) || can_create_connection) {
				createNewConnection();
			}

			return newPendingRequest(response_decoder, callbacks);
		} else {
			ENVOY_LOG(debug, "max pending requests overflow");
			callbacks.onPoolFailure(ConnectionPool::PoolFailureReason::Overflow, absl::string_view(),
															nullptr);
			host_->cluster().stats().upstream_rq_pending_overflow_.inc();
			return nullptr;
		}
  } else {
		ASSERT(drained_callbacks_.empty());

		// First see if we need to handle max streams rollover.
		uint64_t max_streams = host_->cluster().maxRequestsPerConnection();
		if (max_streams == 0) {
			max_streams = maxTotalStreams();
		}

		if (primary_client_ && primary_client_->total_streams_ >= max_streams) {
			movePrimaryClientToDraining();
		}

		if (!primary_client_) {
			primary_client_ = std::make_unique<ActiveClient>(*this);
		}

		// If the primary client is not connected yet, queue up the request.
		if (!primary_client_->upstream_ready_) {
			// If we're not allowed to enqueue more requests, fail fast.
			if (!host_->cluster().resourceManager(priority_).pendingRequests().canCreate()) {
				ENVOY_LOG(debug, "max pending requests overflow");
				callbacks.onPoolFailure(ConnectionPool::PoolFailureReason::Overflow, absl::string_view(),
																nullptr);
				host_->cluster().stats().upstream_rq_pending_overflow_.inc();
				return nullptr;
			}

			return newPendingRequest(response_decoder, callbacks);
		}

		// We already have an active client that's connected to upstream, so attempt to establish a
		// new stream.
		newClientStream(response_decoder, callbacks);
		return nullptr;
  }
}

void ConnPoolImpl::onConnectionEvent(ActiveClient& client, Network::ConnectionEvent event) {
  // TODO(incfly): this will be different, http2 has its own client primary and secondary 
  // handling.
  if (event == Network::ConnectionEvent::RemoteClose ||
      event == Network::ConnectionEvent::LocalClose) {
    // The client died.
    ENVOY_CONN_LOG(debug, "client disconnected, failure reason: {}", *client.codec_client_,
                   client.codec_client_->connectionFailureReason());

    Envoy::Upstream::reportUpstreamCxDestroy(host_, event);
    ActiveClientPtr removed;
    bool check_for_drained = true;
    if (client.stream_wrapper_) {
      if (!client.stream_wrapper_->decode_complete_) {
        Envoy::Upstream::reportUpstreamCxDestroyActiveRequest(host_, event);
      }

      // There is an active request attached to this client. The underlying codec client will
      // already have "reset" the stream to fire the reset callback. All we do here is just
      // destroy the client.
      removed = client.removeFromList(busy_clients_);
    } else if (!client.connect_timer_) {
      // The connect timer is destroyed on connect. The lack of a connect timer means that this
      // client is idle and in the ready pool.
      removed = client.removeFromList(ready_clients_);
      check_for_drained = false;
    } else {
      // The only time this happens is if we actually saw a connect failure.
      host_->cluster().stats().upstream_cx_connect_fail_.inc();
      host_->stats().cx_connect_fail_.inc();

      removed = client.removeFromList(busy_clients_);

      // Raw connect failures should never happen under normal circumstances. If we have an upstream
      // that is behaving badly, requests can get stuck here in the pending state. If we see a
      // connect failure, we purge all pending requests so that calling code can determine what to
      // do with the request.
      ENVOY_CONN_LOG(debug, "purge pending, failure reason: {}", *client.codec_client_,
                     client.codec_client_->connectionFailureReason());
      purgePendingRequests(client.real_host_description_,
                           client.codec_client_->connectionFailureReason());
    }

    dispatcher_.deferredDelete(std::move(removed));

    // If we have pending requests and we just lost a connection we should make a new one.
    if (pending_requests_.size() > (ready_clients_.size() + busy_clients_.size())) {
      createNewConnection();
    }

    if (check_for_drained) {
      checkForDrained();
    }
  }

  if (client.connect_timer_) {
    client.connect_timer_->disableTimer();
    client.connect_timer_.reset();
  }

  // Note that the order in this function is important. Concretely, we must destroy the connect
  // timer before we process a connected idle client, because if this results in an immediate
  // drain/destruction event, we key off of the existence of the connect timer above to determine
  // whether the client is in the ready list (connected) or the busy list (failed to connect).
  if (event == Network::ConnectionEvent::Connected) {
    conn_connect_ms_->complete();
    ENVOY_LOG(info, "incfly debug connection alpn {}", client.codec_client_->ALPNProtocol());
    if (client.codec_client_->ALPNProtocol() == "h2") {
      upgrade();
      // TODO: more thoughts on existing http1 clients handling
      // Need to ensure old clients are properly handled, especially those already bound with http1 request.
      client.upgrade();
    }
    if (http_protocol_ == CodecClient::Type::HTTP2) {
      client.upstream_ready_ = true;
      HTTP2onUpstreamReady();
      return;
    }
    // HTTP1 case.
    processIdleClient(client, false);
  }
}

// HTTPX specifics stuff.
void ConnPoolImpl::upgrade() {
  if (http_protocol_ == CodecClient::Type::HTTP2) {
    return;
  }
  // From this point on we can assume new request can be handled as H2.
  http_protocol_ = CodecClient::Type::HTTP2;
  // TODO(incfly): rest is copied from HTTP2 newStream. Better refactored out.
  // First see if we need to handle max streams rollover.
  uint64_t max_streams = host_->cluster().maxRequestsPerConnection();
  if (max_streams == 0) {
    max_streams = maxTotalStreams();
  }

  if (primary_client_ && primary_client_->total_streams_ >= max_streams) {
    movePrimaryClientToDraining();
  }

  if (!primary_client_) {
    primary_client_ = std::make_unique<ActiveClient>(*this);
  }

  // TODO(incfly): garbage collecting the rest active client created by HTTP1.
}

void ConnPoolImpl::onDownstreamReset(ActiveClient& client) {
  // If we get a downstream reset to an attached client, we just blow it away.
  client.codec_client_->close();
}

void ConnPoolImpl::onResponseComplete(ActiveClient& client) {
  ENVOY_CONN_LOG(debug, "response complete", *client.codec_client_);
  if (!client.stream_wrapper_->encode_complete_) {
    ENVOY_CONN_LOG(debug, "response before request complete", *client.codec_client_);
    onDownstreamReset(client);
  } else if (client.stream_wrapper_->close_connection_ || client.codec_client_->remoteClosed()) {
    ENVOY_CONN_LOG(debug, "saw upstream close connection", *client.codec_client_);
    onDownstreamReset(client);
  } else if (client.remaining_requests_ > 0 && --client.remaining_requests_ == 0) {
    ENVOY_CONN_LOG(debug, "maximum requests per connection", *client.codec_client_);
    host_->cluster().stats().upstream_cx_max_requests_.inc();
    onDownstreamReset(client);
  } else {
    // Upstream connection might be closed right after response is complete. Setting delay=true
    // here to attach pending requests in next dispatcher loop to handle that case.
    // https://github.com/envoyproxy/envoy/issues/2715
    processIdleClient(client, true);
  }
}

void ConnPoolImpl::onUpstreamReady() {
  upstream_ready_enabled_ = false;
  while (!pending_requests_.empty() && !ready_clients_.empty()) {
    ActiveClient& client = *ready_clients_.front();
    ENVOY_CONN_LOG(debug, "attaching to next request", *client.codec_client_);
    // There is work to do so bind a request to the client and move it to the busy list. Pending
    // requests are pushed onto the front, so pull from the back.
    attachRequestToClient(client, pending_requests_.back()->decoder_,
                          pending_requests_.back()->callbacks_);
    pending_requests_.pop_back();
    client.moveBetweenLists(ready_clients_, busy_clients_);
  }
}

void ConnPoolImpl::HTTP2onUpstreamReady() {
  // Establishes new codec streams for each pending request.
  while (!pending_requests_.empty()) {
    newClientStream(pending_requests_.back()->decoder_, pending_requests_.back()->callbacks_);
    pending_requests_.pop_back();
  }
}

void ConnPoolImpl::processIdleClient(ActiveClient& client, bool delay) {
  client.stream_wrapper_.reset();
  if (pending_requests_.empty() || delay) {
    // There is nothing to service or delayed processing is requested, so just move the connection
    // into the ready list.
    ENVOY_CONN_LOG(debug, "moving to ready", *client.codec_client_);
    client.moveBetweenLists(busy_clients_, ready_clients_);
  } else {
    // There is work to do immediately so bind a request to the client and move it to the busy list.
    // Pending requests are pushed onto the front, so pull from the back.
    ENVOY_CONN_LOG(debug, "attaching to next request", *client.codec_client_);
    attachRequestToClient(client, pending_requests_.back()->decoder_,
                          pending_requests_.back()->callbacks_);
    pending_requests_.pop_back();
  }

  if (delay && !pending_requests_.empty() && !upstream_ready_enabled_) {
    upstream_ready_enabled_ = true;
    upstream_ready_timer_->enableTimer(std::chrono::milliseconds(0));
  }

  checkForDrained();
}

ConnPoolImpl::StreamWrapper::StreamWrapper(StreamDecoder& response_decoder, ActiveClient& parent)
    : StreamEncoderWrapper(parent.codec_client_->newStream(*this)),
      StreamDecoderWrapper(response_decoder), parent_(parent) {

  StreamEncoderWrapper::inner_.getStream().addCallbacks(*this);
  parent_.parent_.host_->cluster().stats().upstream_rq_active_.inc();
  parent_.parent_.host_->stats().rq_active_.inc();
}

ConnPoolImpl::StreamWrapper::~StreamWrapper() {
  parent_.parent_.host_->cluster().stats().upstream_rq_active_.dec();
  parent_.parent_.host_->stats().rq_active_.dec();
}

void ConnPoolImpl::StreamWrapper::onEncodeComplete() { encode_complete_ = true; }

void ConnPoolImpl::StreamWrapper::decodeHeaders(HeaderMapPtr&& headers, bool end_stream) {
  // If Connection: close OR
  //    Http/1.0 and not Connection: keep-alive OR
  //    Proxy-Connection: close
  if ((headers->Connection() &&
       (absl::EqualsIgnoreCase(headers->Connection()->value().getStringView(),
                               Headers::get().ConnectionValues.Close))) ||
      (parent_.codec_client_->protocol() == Protocol::Http10 &&
       (!headers->Connection() ||
        !absl::EqualsIgnoreCase(headers->Connection()->value().getStringView(),
                                Headers::get().ConnectionValues.KeepAlive))) ||
      (headers->ProxyConnection() &&
       (absl::EqualsIgnoreCase(headers->ProxyConnection()->value().getStringView(),
                               Headers::get().ConnectionValues.Close)))) {
    parent_.parent_.host_->cluster().stats().upstream_cx_close_notify_.inc();
    close_connection_ = true;
  }

  StreamDecoderWrapper::decodeHeaders(std::move(headers), end_stream);
}

void ConnPoolImpl::StreamWrapper::onDecodeComplete() {
  decode_complete_ = encode_complete_;
  parent_.parent_.onResponseComplete(parent_);
}

ConnPoolImpl::ActiveClient::ActiveClient(ConnPoolImpl& parent)
    : parent_(parent),
      connect_timer_(parent_.dispatcher_.createTimer([this]() -> void { onConnectTimeout(); })),
      remaining_requests_(parent_.host_->cluster().maxRequestsPerConnection()),
      http_protocol_(parent.http_protocol()) {

  parent_.conn_connect_ms_ = std::make_unique<Stats::Timespan>(
      parent_.host_->cluster().stats().upstream_cx_connect_ms_, parent_.dispatcher_.timeSource());
  Upstream::Host::CreateConnectionData data =
      parent_.host_->createConnection(parent_.dispatcher_, parent_.socket_options_, nullptr);
  real_host_description_ = data.host_description_;
  codec_client_ = parent_.createCodecClient(data);
  codec_client_->addConnectionCallbacks(*this);
  parent_.host_->cluster().stats().upstream_cx_total_.inc();
  parent_.host_->cluster().stats().upstream_cx_active_.inc();
  // TODO(incfly): how to handle the stats case, maybe http1 maybe http2, can't decided at this phase.
  if (http_protocol_ == CodecClient::Type::HTTP1) {
    parent_.host_->cluster().stats().upstream_cx_http1_total_.inc();
  } else {
    parent_.host_->cluster().stats().upstream_cx_http2_total_.inc();
  }
  parent_.host_->stats().cx_total_.inc();
  parent_.host_->stats().cx_active_.inc();
  conn_length_ = std::make_unique<Stats::Timespan>(
      parent_.host_->cluster().stats().upstream_cx_length_ms_, parent_.dispatcher_.timeSource());
  connect_timer_->enableTimer(parent_.host_->cluster().connectTimeout());
  parent_.host_->cluster().resourceManager(parent_.priority_).connections().inc();

  codec_client_->setConnectionStats(
      {parent_.host_->cluster().stats().upstream_cx_rx_bytes_total_,
       parent_.host_->cluster().stats().upstream_cx_rx_bytes_buffered_,
       parent_.host_->cluster().stats().upstream_cx_tx_bytes_total_,
       parent_.host_->cluster().stats().upstream_cx_tx_bytes_buffered_,
       &parent_.host_->cluster().stats().bind_errors_, nullptr});
}

ConnPoolImpl::ActiveClient::~ActiveClient() {
  parent_.host_->cluster().stats().upstream_cx_active_.dec();
  parent_.host_->stats().cx_active_.dec();
  conn_length_->complete();
  parent_.host_->cluster().resourceManager(parent_.priority_).connections().dec();
}

void ConnPoolImpl::ActiveClient::onConnectTimeout() {
  // We just close the client at this point. This will result in both a timeout and a connect
  // failure and will fold into all the normal connect failure logic.
  ENVOY_CONN_LOG(debug, "connect timeout", *codec_client_);
  parent_.host_->cluster().stats().upstream_cx_connect_timeout_.inc();
  codec_client_->close();
}

CodecClientPtr ProdConnPoolImpl::createCodecClient(Upstream::Host::CreateConnectionData& data) {
  // ENVOY_CONN_LOG(info, "jianfeih debug creating the codec with protocol {}", http_protocol_ == CodecClient::Type::HTTP1);
  CodecClientPtr codec{new CodecClientProd(http_protocol_, std::move(data.connection_),
                                           data.host_description_, dispatcher_)};
  return codec;
}

/*
HTTP2 Hack
*/

void ConnPoolImpl::newClientStream(Http::StreamDecoder& response_decoder,
                                   ConnectionPool::Callbacks& callbacks) {
  if (!host_->cluster().resourceManager(priority_).requests().canCreate()) {
    ENVOY_LOG(debug, "max requests overflow");
    callbacks.onPoolFailure(ConnectionPool::PoolFailureReason::Overflow, absl::string_view(),
                            nullptr);
    host_->cluster().stats().upstream_rq_pending_overflow_.inc();
  } else {
    ENVOY_CONN_LOG(info, "incfly debug creating stream", *primary_client_->codec_client_);
    ENVOY_CONN_LOG(debug, "creating stream", *primary_client_->codec_client_);
    primary_client_->total_streams_++;
    host_->stats().rq_total_.inc();
    host_->stats().rq_active_.inc();
    host_->cluster().stats().upstream_rq_total_.inc();
    host_->cluster().stats().upstream_rq_active_.inc();
    host_->cluster().resourceManager(priority_).requests().inc();
    callbacks.onPoolReady(primary_client_->codec_client_->newStream(response_decoder),
                          primary_client_->real_host_description_);
  }
}

void ConnPoolImpl::onGoAway(ActiveClient& client) {
  ENVOY_CONN_LOG(debug, "remote goaway", *client.codec_client_);
  host_->cluster().stats().upstream_cx_close_notify_.inc();
  if (&client == primary_client_.get()) {
    movePrimaryClientToDraining();
  }
}



void ConnPoolImpl::onStreamDestroy(ActiveClient& client) {
  ENVOY_CONN_LOG(debug, "destroying stream: {} remaining", *client.codec_client_,
                 client.codec_client_->numActiveRequests());
  host_->stats().rq_active_.dec();
  host_->cluster().stats().upstream_rq_active_.dec();
  host_->cluster().resourceManager(priority_).requests().dec();
  if (&client == draining_client_.get() && client.codec_client_->numActiveRequests() == 0) {
    // Close out the draining client if we no long have active requests.
    client.codec_client_->close();
  }

  // If we are destroying this stream because of a disconnect, do not check for drain here. We will
  // wait until the connection has been fully drained of streams and then check in the connection
  // event callback.
  if (!client.closed_with_active_rq_) {
    checkForDrained();
  }
}

void ConnPoolImpl::onStreamReset(ActiveClient& client, Http::StreamResetReason reason) {
  if (reason == StreamResetReason::ConnectionTermination ||
      reason == StreamResetReason::ConnectionFailure) {
    host_->cluster().stats().upstream_rq_pending_failure_eject_.inc();
    client.closed_with_active_rq_ = true;
  } else if (reason == StreamResetReason::LocalReset) {
    host_->cluster().stats().upstream_rq_tx_reset_.inc();
  } else if (reason == StreamResetReason::RemoteReset) {
    host_->cluster().stats().upstream_rq_rx_reset_.inc();
  }
}


void ConnPoolImpl::movePrimaryClientToDraining() {
  ENVOY_CONN_LOG(debug, "moving primary to draining", *primary_client_->codec_client_);
  if (draining_client_) {
    // This should pretty much never happen, but is possible if we start draining and then get
    // a goaway for example. In this case just kill the current draining connection. It's not
    // worth keeping a list.
    draining_client_->codec_client_->close();
  }

  ASSERT(!draining_client_);
  if (primary_client_->codec_client_->numActiveRequests() == 0) {
    // If we are making a new connection and the primary does not have any active requests just
    // close it now.
    primary_client_->codec_client_->close();
  } else {
    draining_client_ = std::move(primary_client_);
  }

  ASSERT(!primary_client_);
}

uint32_t ProdConnPoolImpl::maxTotalStreams() { return MAX_STREAMS; }

} // namespace Http1
} // namespace Http
} // namespace Envoy
