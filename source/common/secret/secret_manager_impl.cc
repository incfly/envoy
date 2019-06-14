#include "common/secret/secret_manager_impl.h"

#include "envoy/common/exception.h"
#include "envoy/admin/v2alpha/config_dump.pb.h"

#include "common/common/assert.h"
#include "common/common/logger.h"
#include "common/secret/sds_api.h"
#include "common/secret/secret_provider_impl.h"
#include "common/ssl/certificate_validation_context_config_impl.h"
#include "common/ssl/tls_certificate_config_impl.h"

namespace Envoy {
namespace Secret {

SecretManagerImpl::SecretManagerImpl(Server::Admin& admin) :
  config_tracker_entry_(
      admin.getConfigTracker().add("secrets", [this] {
        return dumpSecretConfigs();
        })) {
}
void SecretManagerImpl::addStaticSecret(const envoy::api::v2::auth::Secret& secret) {
  switch (secret.type_case()) {
  case envoy::api::v2::auth::Secret::TypeCase::kTlsCertificate: {
    auto secret_provider =
        std::make_shared<TlsCertificateConfigProviderImpl>(secret.tls_certificate());
    if (!static_tls_certificate_providers_.insert(std::make_pair(secret.name(), secret_provider))
             .second) {
      throw EnvoyException(
          fmt::format("Duplicate static TlsCertificate secret name {}", secret.name()));
    }
    break;
  }
  case envoy::api::v2::auth::Secret::TypeCase::kValidationContext: {
    auto secret_provider = std::make_shared<CertificateValidationContextConfigProviderImpl>(
        secret.validation_context());
    if (!static_certificate_validation_context_providers_
             .insert(std::make_pair(secret.name(), secret_provider))
             .second) {
      throw EnvoyException(fmt::format(
          "Duplicate static CertificateValidationContext secret name {}", secret.name()));
    }
    break;
  }
  default:
    throw EnvoyException("Secret type not implemented");
  }
}

TlsCertificateConfigProviderSharedPtr
SecretManagerImpl::findStaticTlsCertificateProvider(const std::string& name) const {
  auto secret = static_tls_certificate_providers_.find(name);
  return (secret != static_tls_certificate_providers_.end()) ? secret->second : nullptr;
}

CertificateValidationContextConfigProviderSharedPtr
SecretManagerImpl::findStaticCertificateValidationContextProvider(const std::string& name) const {
  auto secret = static_certificate_validation_context_providers_.find(name);
  return (secret != static_certificate_validation_context_providers_.end()) ? secret->second
                                                                            : nullptr;
}

TlsCertificateConfigProviderSharedPtr SecretManagerImpl::createInlineTlsCertificateProvider(
    const envoy::api::v2::auth::TlsCertificate& tls_certificate) {
  return std::make_shared<TlsCertificateConfigProviderImpl>(tls_certificate);
}

CertificateValidationContextConfigProviderSharedPtr
SecretManagerImpl::createInlineCertificateValidationContextProvider(
    const envoy::api::v2::auth::CertificateValidationContext& certificate_validation_context) {
  return std::make_shared<CertificateValidationContextConfigProviderImpl>(
      certificate_validation_context);
}

TlsCertificateConfigProviderSharedPtr SecretManagerImpl::findOrCreateTlsCertificateProvider(
    const envoy::api::v2::core::ConfigSource& sds_config_source, const std::string& config_name,
    Server::Configuration::TransportSocketFactoryContext& secret_provider_context) {
  return certificate_providers_.findOrCreate(sds_config_source, config_name,
                                             secret_provider_context);
}

CertificateValidationContextConfigProviderSharedPtr
SecretManagerImpl::findOrCreateCertificateValidationContextProvider(
    const envoy::api::v2::core::ConfigSource& sds_config_source, const std::string& config_name,
    Server::Configuration::TransportSocketFactoryContext& secret_provider_context) {
  return validation_context_providers_.findOrCreate(sds_config_source, config_name,
                                                    secret_provider_context);
}

// TODO: question, what's the handling of static inlined stuff? they're just internal constructs
// not needed to be exposed, maybe?
ProtobufTypes::MessagePtr SecretManagerImpl::dumpSecretConfigs() {
  auto config_dump = std::make_unique<envoy::admin::v2alpha::SecretsConfigDump>();
  // config_dump->set_version_info("jianfeih-test-version");
  auto secrets = certificate_providers_.allSecrets();
  for (const auto& cert_secrets : secrets) {
    auto secret_data = cert_secrets->secretData();
    auto tls_cert = cert_secrets->secret();
    auto dynamic_secret = config_dump->mutable_dynamic_secrets()->Add();
    auto secret = dynamic_secret->mutable_secret();

    ProtobufWkt::Timestamp last_updated_ts;
    TimestampUtil::systemClockToTimestamp(secret_data.last_updated_, last_updated_ts);
    dynamic_secret->set_version_info(secret_data.version_info_);
    *dynamic_secret->mutable_last_updated() = last_updated_ts;
    secret->set_name(secret_data.resource_name);
    // ENVOY_LOG(info, "jianfeih debug the secret is {}", cert_secrets);
    // TODO: handling this should be log the secret data but not the cert to signify
    // stuck at sds updates level.
    if (!tls_cert) {
      // ENVOY_LOG(info, "jianfeih debug the cert is empty");
      continue;
    }
    //ENVOY_LOG(info, "jianfeih debug the cert is empty {} {} {}",
        //secret_data.resource_name, last_updated_ts, secret_data.version_info_);
    auto tls_certificate = secret->mutable_tls_certificate();
    tls_certificate->MergeFrom(*tls_cert);
    tls_certificate->clear_private_key();
    tls_certificate->clear_password();
  }

  // Handling validation Context provided via SDS.
  auto context_secrets = validation_context_providers_.allSecrets();
  for (const auto& validation_context_secret : context_secrets) {
    auto secret_data = validation_context_secret->secretData();
    auto validation_context = validation_context_secret->secret();
    auto dynamic_secret = config_dump->mutable_dynamic_secrets()->Add();
    auto secret = dynamic_secret->mutable_secret();
    ProtobufWkt::Timestamp last_updated_ts;
    TimestampUtil::systemClockToTimestamp(secret_data.last_updated_, last_updated_ts);
    dynamic_secret->set_version_info(secret_data.version_info_);
    *dynamic_secret->mutable_last_updated() = last_updated_ts;
    secret->set_name(secret_data.resource_name);
    if (!validation_context) {
      continue;
    }
    auto dump_context = secret->mutable_validation_context();
    dump_context->MergeFrom(*validation_context);
  }
  return config_dump;
}


} // namespace Secret
} // namespace Envoy
