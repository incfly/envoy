#include <memory>
#include <vector>

#include "common/buffer/buffer_impl.h"
#include "common/event/dispatcher_impl.h"
#include "common/http/codec_client.h"
#include "common/http/httpx/conn_pool.h"
#include "common/network/utility.h"
#include "common/upstream/upstream_impl.h"

#include "test/common/http/common.h"
#include "test/common/upstream/utility.h"
#include "test/mocks/buffer/mocks.h"
#include "test/mocks/event/mocks.h"
#include "test/mocks/http/mocks.h"
#include "test/mocks/network/mocks.h"
#include "test/mocks/runtime/mocks.h"
#include "test/mocks/upstream/mocks.h"
#include "test/test_common/printers.h"
#include "test/test_common/utility.h"

#include "gmock/gmock.h"
#include "gtest/gtest.h"

using testing::_;
using testing::DoAll;
using testing::InSequence;
using testing::Invoke;
using testing::NiceMock;
using testing::Property;
using testing::Return;
using testing::ReturnRef;
using testing::SaveArg;

namespace Envoy {
namespace Http {
namespace Httpx {
namespace {

/**
 * A test version of ConnPoolImpl that allows for mocking beneath the codec clients.
 */
//class ConnPoolImplForTest : public ConnPoolImpl {
//public:
  //ConnPoolImplForTest(Event::MockDispatcher& dispatcher,
                      //Upstream::ClusterInfoConstSharedPtr cluster,
                      //NiceMock<Event::MockTimer>* upstream_ready_timer)
      //: ConnPoolImpl(dispatcher, Upstream::makeTestHost(cluster, "tcp://127.0.0.1:9000"),
                     //Upstream::ResourcePriority::Default, nullptr),
        //api_(Api::createApiForTest()), mock_dispatcher_(dispatcher),
        //mock_upstream_ready_timer_(upstream_ready_timer) {}

  //~ConnPoolImplForTest() override {
    //EXPECT_EQ(0U, ready_clients_.size());
    //EXPECT_EQ(0U, busy_clients_.size());
    //EXPECT_EQ(0U, pending_requests_.size());
  //}

  //struct TestCodecClient {
    //Http::MockClientConnection* codec_;
    //Network::MockClientConnection* connection_;
    //CodecClient* codec_client_;
    //Event::MockTimer* connect_timer_;
    //Event::DispatcherPtr client_dispatcher_;
  //};

  //CodecClientPtr createCodecClient(Upstream::Host::CreateConnectionData& data) override {
    //// We expect to own the connection, but already have it, so just release it to prevent it from
    //// getting deleted.
    //data.connection_.release();
    //return CodecClientPtr{createCodecClient_()};
  //}

  //MOCK_METHOD0(createCodecClient_, CodecClient*());
  //MOCK_METHOD0(onClientDestroy, void());

  //void expectClientCreate(Protocol protocol = Protocol::Http11) {
    //test_clients_.emplace_back();
    //TestCodecClient& test_client = test_clients_.back();
    //test_client.connection_ = new NiceMock<Network::MockClientConnection>();
    //test_client.codec_ = new NiceMock<Http::MockClientConnection>();
    //test_client.connect_timer_ = new NiceMock<Event::MockTimer>(&mock_dispatcher_);
    //std::shared_ptr<Upstream::MockClusterInfo> cluster{new NiceMock<Upstream::MockClusterInfo>()};
    //test_client.client_dispatcher_ = api_->allocateDispatcher();
    //Network::ClientConnectionPtr connection{test_client.connection_};
    //test_client.codec_client_ = new CodecClientForTest(
        //std::move(connection), test_client.codec_,
        //[this](CodecClient* codec_client) -> void {
          //for (auto i = test_clients_.begin(); i != test_clients_.end(); i++) {
            //if (i->codec_client_ == codec_client) {
              //onClientDestroy();
              //test_clients_.erase(i);
              //return;
            //}
          //}
        //},
        //Upstream::makeTestHost(cluster, "tcp://127.0.0.1:9000"), *test_client.client_dispatcher_);
    //EXPECT_CALL(mock_dispatcher_, createClientConnection_(_, _, _, _))
        //.WillOnce(Return(test_client.connection_));
    //EXPECT_CALL(*this, createCodecClient_()).WillOnce(Return(test_client.codec_client_));
    //EXPECT_CALL(*test_client.connect_timer_, enableTimer(_));
    //ON_CALL(*test_client.codec_, protocol()).WillByDefault(Return(protocol));
  //}

  //void expectEnableUpstreamReady() {
    //EXPECT_FALSE(upstream_ready_enabled_);
    //EXPECT_CALL(*mock_upstream_ready_timer_, enableTimer(_)).Times(1).RetiresOnSaturation();
  //}

  //void expectAndRunUpstreamReady() {
    //EXPECT_TRUE(upstream_ready_enabled_);
    //mock_upstream_ready_timer_->callback_();
    //EXPECT_FALSE(upstream_ready_enabled_);
  //}

  //Api::ApiPtr api_;
  //Event::MockDispatcher& mock_dispatcher_;
  //NiceMock<Event::MockTimer>* mock_upstream_ready_timer_;
  //std::vector<TestCodecClient> test_clients_;
//};

/**
 * Test fixture for all connection pool tests.
 */
class HttpxConnPoolImplTest : public testing::Test {
};
//public:
  //Http1ConnPoolImplTest()
      //: upstream_ready_timer_(new NiceMock<Event::MockTimer>(&dispatcher_)),
        //conn_pool_(dispatcher_, cluster_, upstream_ready_timer_) {}

  //~Http1ConnPoolImplTest() override {
    //EXPECT_TRUE(TestUtility::gaugesZeroed(cluster_->stats_store_.gauges()));
  //}

  //NiceMock<Event::MockDispatcher> dispatcher_;
  //std::shared_ptr<Upstream::MockClusterInfo> cluster_{new NiceMock<Upstream::MockClusterInfo>()};
  //NiceMock<Event::MockTimer>* upstream_ready_timer_;
  //ConnPoolImplForTest conn_pool_;
  //NiceMock<Runtime::MockLoader> runtime_;
//};

/**
 * Helper for dealing with an active test request.
 */
//struct ActiveTestRequest {
  //enum class Type { Pending, CreateConnection, Immediate };

  //ActiveTestRequest(Http1ConnPoolImplTest& parent, size_t client_index, Type type)
      //: parent_(parent), client_index_(client_index) {
    //uint64_t current_rq_total = parent_.cluster_->stats_.upstream_rq_total_.value();
    //if (type == Type::CreateConnection) {
      //parent.conn_pool_.expectClientCreate();
    //}

    //if (type == Type::Immediate) {
      //expectNewStream();
    //}

    //handle_ = parent.conn_pool_.newStream(outer_decoder_, callbacks_);

    //if (type == Type::Immediate) {
      //EXPECT_EQ(nullptr, handle_);
    //} else {
      //EXPECT_NE(nullptr, handle_);
    //}

    //if (type == Type::CreateConnection) {
      //EXPECT_CALL(*parent_.conn_pool_.test_clients_[client_index_].connect_timer_, disableTimer());
      //expectNewStream();
      //parent.conn_pool_.test_clients_[client_index_].connection_->raiseEvent(
          //Network::ConnectionEvent::Connected);
    //}
    //if (type != Type::Pending) {
      //EXPECT_EQ(current_rq_total + 1, parent_.cluster_->stats_.upstream_rq_total_.value());
    //}
  //}

  //void completeResponse(bool with_body) {
    //// Test additional metric writes also.
    //Http::HeaderMapPtr response_headers(
        //new TestHeaderMapImpl{{":status", "200"}, {"x-envoy-upstream-canary", "true"}});

    //inner_decoder_->decodeHeaders(std::move(response_headers), !with_body);
    //if (with_body) {
      //Buffer::OwnedImpl data;
      //inner_decoder_->decodeData(data, true);
    //}
  //}

  //void expectNewStream() {
    //EXPECT_CALL(*parent_.conn_pool_.test_clients_[client_index_].codec_, newStream(_))
        //.WillOnce(DoAll(SaveArgAddress(&inner_decoder_), ReturnRef(request_encoder_)));
    //EXPECT_CALL(callbacks_.pool_ready_, ready());
  //}

  //void startRequest() { callbacks_.outer_encoder_->encodeHeaders(TestHeaderMapImpl{}, true); }

  //Http1ConnPoolImplTest& parent_;
  //size_t client_index_;
  //NiceMock<Http::MockStreamDecoder> outer_decoder_;
  //Http::ConnectionPool::Cancellable* handle_{};
  //NiceMock<Http::MockStreamEncoder> request_encoder_;
  //Http::StreamDecoder* inner_decoder_{};
  //ConnPoolCallbacks callbacks_;
//};

TEST_F(HttpxConnPoolImplTest, Host) {
}

} // namespace
} // namespace Http1
} // namespace Http
}
