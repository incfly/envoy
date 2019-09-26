#include "extensions/transport_sockets/tls/context_config_impl.h"
#include "extensions/transport_sockets/tls/context_impl.h"
#include "extensions/transport_sockets/tls/ssl_socket.h"

#include "test/integration/http_integration.h"

#include "absl/strings/str_replace.h"
#include "gtest/gtest.h"

namespace Envoy {

// TODO(incfly):
// - upstream setup
//    use the FakeUpstream with transport socket factory typed constructor.
//    override the createUpstreams()
//    src/test/integration/integration.cc:517 is the example of creating the fake upstream
//     with tls configuration, also see how Network::Test::createRawBufferSocket is invoked.
//    just creating the fake upstream right away here...inline code for now.
//    get rid of autonomous_upstream_
//    line 71
// - Client envoy configuration modifying
// bazel test //test/integration:transport_socket_match_integration_test --test_output=streamed
// --test_arg='-l info'
class TransportSockeMatchIntegrationTest : public testing::Test, public HttpIntegrationTest {
public:
  TransportSockeMatchIntegrationTest()
      : HttpIntegrationTest(Http::CodecClient::Type::HTTP1,
                            TestEnvironment::getIpVersionsForTest().front(),
                            ConfigHelper::HTTP_PROXY_CONFIG),
        num_hosts_{1} {
    setUpstreamCount(num_hosts_);
    config_helper_.addConfigModifier([&](envoy::config::bootstrap::v2::Bootstrap& bootstrap) {
      auto* static_resources = bootstrap.mutable_static_resources();
      auto* cluster = static_resources->mutable_clusters(0);
      cluster->clear_hosts();
      auto* load_assignment = cluster->mutable_load_assignment();
      load_assignment->set_cluster_name(cluster->name());
      auto* endpoints = load_assignment->add_endpoints();
      for (uint32_t i = 0; i < num_hosts_; i++) {
        auto* lb_endpoint = endpoints->add_lb_endpoints();
        // ConfigHelper will fill in ports later.
        auto* endpoint = lb_endpoint->mutable_endpoint();
        auto* addr = endpoint->mutable_address()->mutable_socket_address();
        addr->set_address(Network::Test::getLoopbackAddressString(
            TestEnvironment::getIpVersionsForTest().front()));
        addr->set_port_value(0);
        // Assign type metadata based on i.
        // auto* metadata = lb_endpoint->mutable_metadata();
        // Envoy::Config::Metadata::mutableMetadataValue(*metadata, "envoy.lb", type_key_)
        //.set_string_value((i % 2 == 0) ? "a" : "b");
      }
    });
  }

  Network::TransportSocketFactoryPtr createUpstreamSslContext() {
    // copied from grpc_*_hardness.*.h
    envoy::api::v2::auth::DownstreamTlsContext tls_context;
    auto* common_tls_context = tls_context.mutable_common_tls_context();
    common_tls_context->add_alpn_protocols("h2");
    auto* tls_cert = common_tls_context->add_tls_certificates();
    tls_cert->mutable_certificate_chain()->set_filename(
        TestEnvironment::runfilesPath("test/config/integration/certs/upstreamcert.pem"));
    tls_cert->mutable_private_key()->set_filename(
        TestEnvironment::runfilesPath("test/config/integration/certs/upstreamkey.pem"));
    // if (use_client_cert_) {
    tls_context.mutable_require_client_certificate()->set_value(true);
    auto* validation_context = common_tls_context->mutable_validation_context();
    validation_context->mutable_trusted_ca()->set_filename(
        TestEnvironment::runfilesPath("test/config/integration/certs/cacert.pem"));
    //}
    auto cfg = std::make_unique<Extensions::TransportSockets::Tls::ServerContextConfigImpl>(
        tls_context, factory_context_);
    static Stats::Scope* upstream_stats_store = new Stats::IsolatedStoreImpl();
    return std::make_unique<Extensions::TransportSockets::Tls::ServerSslSocketFactory>(
        std::move(cfg), context_manager_, *upstream_stats_store, std::vector<std::string>{});
  }

  void createUpstreams() override {
    for (uint32_t i = 0; i < num_hosts_; i++) {
      // Use below ssl upstream fails as expected...
      // fake_upstreams_.emplace_back(
      // std::make_unique<FakeUpstream>(createUpstreamSslContext(), 0,
      // FakeHttpConnection::Type::HTTP1, version_, timeSystem()));
      fake_upstreams_.emplace_back(
          new FakeUpstream(0, FakeHttpConnection::Type::HTTP1, version_, timeSystem()));
    }
  }
  const uint32_t num_hosts_;
};

TEST_F(TransportSockeMatchIntegrationTest, BasicMatch) {
  initialize();
  codec_client_ = makeHttpConnection(lookupPort("http"));
  auto response =
      sendRequestAndWaitForResponse(default_request_headers_, 0, default_response_headers_, 0);
  EXPECT_EQ("200", response->headers().Status()->value().getStringView());
}

} // namespace Envoy
