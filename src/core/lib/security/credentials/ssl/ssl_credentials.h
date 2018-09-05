/*
 *
 * Copyright 2016 gRPC authors.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */
#ifndef GRPC_CORE_LIB_SECURITY_CREDENTIALS_SSL_SSL_CREDENTIALS_H
#define GRPC_CORE_LIB_SECURITY_CREDENTIALS_SSL_SSL_CREDENTIALS_H

#include <grpc/support/port_platform.h>

#include "src/core/lib/security/credentials/credentials.h"

#include "src/core/lib/security/security_connector/ssl/ssl_security_connector.h"
#include "src/core/lib/security/security_connector/ssl_utils.h"

struct grpc_ssl_channel_certificate_config {
  grpc_ssl_pem_key_cert_pair* pem_key_cert_pair;
  char* pem_root_certs;
};

typedef struct {
  grpc_ssl_channel_certificate_config_callback cb;
  void* user_data;
} grpc_ssl_channel_certificate_config_fetcher;

class grpc_ssl_credentials : public grpc_channel_credentials {
 public:
  grpc_ssl_credentials(const char* pem_root_certs,
                       grpc_ssl_pem_key_cert_pair* pem_key_cert_pair,
                       const verify_peer_options* verify_options);
  grpc_ssl_credentials(grpc_ssl_channel_certificate_config_callback cb,
                       void* user_data,
                       const verify_peer_options* verify_options);

  ~grpc_ssl_credentials() override;

  grpc_core::RefCountedPtr<grpc_channel_security_connector>
  create_security_connector(
      grpc_core::RefCountedPtr<grpc_call_credentials> call_creds,
      const char* target, const grpc_channel_args* args,
      grpc_channel_args** new_args) override;

  /* Attempts to fetch the channel certificate config if a callback is
   * available. Current certificate config will continue to be used if the
   * callback returns an error.
   */
  grpc_core::VersionedClientSslConfig TryFetchCertConfig();

 private:
  grpc_ssl_credentials(const verify_peer_options* verify_options);

  gpr_mu lock_;
  grpc_core::VersionedClientSslConfig config_;
  grpc_ssl_channel_certificate_config_fetcher certificate_config_fetcher_;
  verify_peer_options verify_options_;
};

struct grpc_ssl_server_certificate_config {
  grpc_ssl_pem_key_cert_pair* pem_key_cert_pairs = nullptr;
  size_t num_key_cert_pairs = 0;
  char* pem_root_certs = nullptr;
};

struct grpc_ssl_server_certificate_config_fetcher {
  grpc_ssl_server_certificate_config_callback cb = nullptr;
  void* user_data;
};

class grpc_ssl_server_credentials final : public grpc_server_credentials {
 public:
  grpc_ssl_server_credentials(
      const grpc_ssl_server_credentials_options& options);
  ~grpc_ssl_server_credentials() override;

  grpc_core::RefCountedPtr<grpc_server_security_connector>
  create_security_connector() override;

  bool has_cert_config_fetcher() const {
    return certificate_config_fetcher_.cb != nullptr;
  }

  grpc_ssl_certificate_config_reload_status FetchCertConfig(
      grpc_ssl_server_certificate_config** config) {
    GPR_DEBUG_ASSERT(has_cert_config_fetcher());
    return certificate_config_fetcher_.cb(certificate_config_fetcher_.user_data,
                                          config);
  }

  const grpc_ssl_server_config& config() const { return config_; }

 private:
  void build_config(
      const char* pem_root_certs,
      grpc_ssl_pem_key_cert_pair* pem_key_cert_pairs, size_t num_key_cert_pairs,
      grpc_ssl_client_certificate_request_type client_certificate_request);

  grpc_ssl_server_config config_;
  grpc_ssl_server_certificate_config_fetcher certificate_config_fetcher_;
};

tsi_ssl_pem_key_cert_pair* grpc_convert_grpc_to_tsi_cert_pairs(
    const grpc_ssl_pem_key_cert_pair* pem_key_cert_pairs,
    size_t num_key_cert_pairs);

void grpc_tsi_ssl_pem_key_cert_pairs_destroy(tsi_ssl_pem_key_cert_pair* kp,
                                             size_t num_key_cert_pairs);

#endif /* GRPC_CORE_LIB_SECURITY_CREDENTIALS_SSL_SSL_CREDENTIALS_H */
