/*
 *
 * Copyright 2015 gRPC authors.
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

#ifndef GRPC_CORE_LIB_SECURITY_SECURITY_CONNECTOR_SSL_UTILS_H
#define GRPC_CORE_LIB_SECURITY_SECURITY_CONNECTOR_SSL_UTILS_H

#include <grpc/support/port_platform.h>

#include <stdbool.h>

#include <grpc/grpc_security.h>
#include <grpc/slice_buffer.h>

#include "src/core/lib/gprpp/ref_counted.h"
#include "src/core/lib/gprpp/ref_counted_ptr.h"
#include "src/core/tsi/ssl_transport_security.h"
#include "src/core/tsi/transport_security_interface.h"

/* --- Util. --- */

/* --- URL schemes. --- */
#define GRPC_SSL_URL_SCHEME "https"

/* Return HTTP2-compliant cipher suites that gRPC accepts by default. */
const char* grpc_get_ssl_cipher_suites(void);

/* Map from grpc_ssl_client_certificate_request_type to
 * tsi_client_certificate_request_type. */
tsi_client_certificate_request_type
grpc_get_tsi_client_certificate_request_type(
    grpc_ssl_client_certificate_request_type grpc_request_type);

/* Return an array of strings containing alpn protocols. */
const char** grpc_fill_alpn_protocol_strings(size_t* num_alpn_protocols);

/* Exposed for testing only. */
grpc_core::RefCountedPtr<grpc_auth_context> grpc_ssl_peer_to_auth_context(
    const tsi_peer* peer);
tsi_peer grpc_shallow_peer_from_ssl_auth_context(
    const grpc_auth_context* auth_context);
void grpc_shallow_peer_destruct(tsi_peer* peer);
int grpc_ssl_host_matches_name(const tsi_peer* peer, const char* peer_name);

namespace grpc_core {

class ClientSslConfig : public RefCounted<ClientSslConfig> {
 public:
  static RefCountedPtr<ClientSslConfig> Create(
      const char* pem_root_certs,
      grpc_ssl_pem_key_cert_pair* pem_key_cert_pair);

  const char* GetPemRootCerts() const { return pem_root_certs_; }

  tsi_ssl_pem_key_cert_pair* GetPemKeyCertPair() const {
    return pem_key_cert_pair_;
  }

 private:
  // So New() can call our private ctor.
  template <typename T2, typename... Args>
  friend T2* New(Args&&... args);

  // So Delete() can call our private dtor.
  template <typename T2>
  friend void Delete(T2*);

  ClientSslConfig(const char* pem_root_certs,
                  grpc_ssl_pem_key_cert_pair* pem_key_cert_pair);
  ~ClientSslConfig();

  char* pem_root_certs_ = nullptr;
  tsi_ssl_pem_key_cert_pair* pem_key_cert_pair_ = nullptr;
};

struct VersionedClientSslConfig {
  int version;
  RefCountedPtr<ClientSslConfig> config;
};

/* --- Default SSL Root Store. --- */

// The class implements default SSL root store.
class DefaultSslRootStore {
 public:
  // Gets the default SSL root store. Returns nullptr if not found.
  static const tsi_ssl_root_certs_store* GetRootStore();

  // Gets the default PEM root certificate.
  static const char* GetPemRootCerts();

 protected:
  // Returns default PEM root certificates in nullptr terminated grpc_slice.
  // This function is protected instead of private, so that it can be tested.
  static grpc_slice ComputePemRootCerts();

 private:
  // Construct me not!
  DefaultSslRootStore();

  // Initialization of default SSL root store.
  static void InitRootStore();

  // One-time initialization of default SSL root store.
  static void InitRootStoreOnce();

  // SSL root store in tsi_ssl_root_certs_store object.
  static tsi_ssl_root_certs_store* default_root_store_;

  // Default PEM root certificates.
  static grpc_slice default_pem_root_certs_;
};

}  // namespace grpc_core

#endif /* GRPC_CORE_LIB_SECURITY_SECURITY_CONNECTOR_SSL_UTILS_H \
        */
