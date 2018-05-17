#include "extensions/grpc_credentials/token_file.h"

#include "envoy/api/v2/core/grpc_service.pb.h"
#include "envoy/api/v2/core/credentials.pb.h"
#include "envoy/grpc/google_grpc_creds.h"
#include "envoy/registry/registry.h"

#include "common/grpc/google_grpc_creds_impl.h"
#include "common/filesystem/filesystem_impl.h"

namespace Envoy {
namespace Extensions {
namespace GrpcCredentials {

std::shared_ptr<grpc::ChannelCredentials>
TokenFileExampleGrpcCredentialsFactory::getChannelCredentials(
    const envoy::api::v2::core::GrpcService& grpc_service_config) {
  const auto& google_grpc = grpc_service_config.google_grpc();
  std::shared_ptr<grpc::ChannelCredentials> creds =
      grpc::SslCredentials(grpc::SslCredentialsOptions());
  if (google_grpc.has_channel_credentials() &&
      google_grpc.channel_credentials().has_ssl_credentials()) {
    creds = grpc::SslCredentials(
        Grpc::buildSslOptionsFromConfig(google_grpc.channel_credentials().ssl_credentials()));
  }
  std::shared_ptr<grpc::CallCredentials> call_creds = nullptr;
  for (const auto& credential : google_grpc.call_credentials()) {
    switch (credential.credential_specifier_case()) {
    case envoy::api::v2::core::GrpcService::GoogleGrpc::CallCredentials::kFromPlugin: {
      const auto& plugin = credential.from_plugin();
      const std::string name = plugin.name();
      if (name == "envoy.v2.core.token_file_credentials") {
        const auto& config = plugin.config().fields();
        std::string token_name;
        std::string token_path;
        if (config.find("credential_name") != config.end()) {
          token_name = config.at("credential_name").string_value();
        }
        if (config.find("credential_path") != config.end()) {
          token_path = config.at("credential_path").string_value();
        }
        std::shared_ptr<grpc::CallCredentials> new_call_creds = grpc::MetadataCredentialsFromPlugin(
           std::make_unique<TokenFileAuthenticator>(token_name, token_path));
        if (call_creds == nullptr) {
          call_creds = new_call_creds;
        } else {
          call_creds = grpc::CompositeCallCredentials(call_creds, new_call_creds);
        }
      }
      break;
    }
    default:
      // unused credential types
      continue;
    }
  }
  if (call_creds != nullptr) {
    return grpc::CompositeChannelCredentials(creds, call_creds);
  }
  return creds;
}

grpc::Status
TokenFileAuthenticator::GetMetadata(grpc::string_ref, grpc::string_ref, const grpc::AuthContext&,
                                    std::multimap<grpc::string, grpc::string>* metadata) {
  grpc::string token_value = ::Envoy::Filesystem::fileReadToEnd(token_path_);
  metadata->insert(std::make_pair(token_name_, token_value));
  return grpc::Status::OK;
}
static Registry::RegisterFactory<TokenFileExampleGrpcCredentialsFactory, Grpc::GoogleGrpcCredentialsFactory>
  token_file_google_grpc_credentials_registered_;

} // namespace GrpcCredentials
} // namespace Extensions
} // namespace Envoy
