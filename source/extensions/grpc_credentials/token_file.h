#pragma once

#include "envoy/grpc/google_grpc_creds.h"

#include "extensions/grpc_credentials/well_known_names.h"

namespace Envoy {
namespace Extensions {
namespace GrpcCredentials {

class TokenFileExampleGrpcCredentialsFactory : public Grpc::GoogleGrpcCredentialsFactory {
public:
  virtual std::shared_ptr<grpc::ChannelCredentials>
  getChannelCredentials(const envoy::api::v2::core::GrpcService& grpc_service_config) override;

  std::string name() const override {
    return "envoy.grpc_credentials.token_file_exmaple";
  }
};

class TokenFileAuthenticator : public grpc::MetadataCredentialsPlugin {
public:
  TokenFileAuthenticator(const grpc::string& token_name, const grpc::string& token_path) :
    token_name_(token_name), token_path_(token_path) {}
  
  grpc::Status GetMetadata(grpc::string_ref, grpc::string_ref, const grpc::AuthContext&,
                           std::multimap<grpc::string, grpc::string>* metadata) override;
private:
  grpc::string token_name_;
  grpc::string token_path_;
};

} // namespace GrpcCredentials
} // namespace Extensions
} // namespace Envoy
