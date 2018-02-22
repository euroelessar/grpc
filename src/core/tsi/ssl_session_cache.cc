/*
 *
 * Copyright 2017 gRPC authors.
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

#include "src/core/tsi/ssl_session_cache.h"

#include <grpc/support/log.h>

namespace grpc_core {

SslSessionPtr ssl_session_clone(const SslSessionPtr& other) {
  SSL_SESSION_up_ref(other.get());
  return SslSessionPtr(other.get());
}

SslSessionLRUCache::SslSessionLRUCache(size_t capacity) : capacity_(capacity) {
  GPR_ASSERT(capacity > 0);
  gpr_ref_init(&ref_, 1);
  gpr_mu_init(&lock_);
}

SslSessionLRUCache::~SslSessionLRUCache() {
  gpr_mu_destroy(&lock_);
}

void SslSessionLRUCache::Put(const char* key, SslSessionPtr session) {
  grpc_slice key_slice = grpc_slice_from_copied_string(key);
  mu_guard guard(&lock_);

  SslSessionList::iterator it = FindLocked(key_slice);
  if (it != use_order_list_.end()) {
    grpc_slice_unref(key_slice);
    it->SetSession(std::move(session));
    return;
  }

  use_order_list_.emplace_front(key_slice, std::move(session));
  auto emplace_result = entry_by_key_.emplace(
      key_slice, use_order_list_.begin());
  GPR_ASSERT(emplace_result.second);

  if (use_order_list_.size() > capacity_) {
    it = std::prev(use_order_list_.end());
    GPR_ASSERT(it != use_order_list_.end());

    // Order matters, key is destroyed after removing element from the list.
    size_t removed_count = entry_by_key_.erase(it->Key());
    GPR_ASSERT(removed_count == 1);
    use_order_list_.erase(it);
  }
}

SslSessionPtr SslSessionLRUCache::Get(const char* key) {
  // Key is only used for lookups.
  grpc_slice key_slice = grpc_slice_from_static_string(key);
  mu_guard guard(&lock_);

  SslSessionList::iterator it = FindLocked(key_slice);
  if (it == use_order_list_.end()) {
    return nullptr;
  }

  return it->GetSession();
}

size_t SslSessionLRUCache::Size() {
  mu_guard guard(&lock_);

  GPR_ASSERT(use_order_list_.size() == entry_by_key_.size());
  return use_order_list_.size();
}

SslSessionLRUCache::SslSessionList::iterator SslSessionLRUCache::FindLocked(
    const grpc_slice& key) {
  auto it = entry_by_key_.find(key);
  if (it == entry_by_key_.end()) {
    return use_order_list_.end();
  }

  // Move to the beginning.
  if (it->second != use_order_list_.begin()) {
    use_order_list_.splice(use_order_list_.begin(), use_order_list_, it->second);
  }
  GPR_ASSERT(grpc_slice_is_equivalent(it->first, it->second->Key()));

  return it->second;
}

int SslSessionLRUCache::GetSslExIndex() {
  static int id = SSL_CTX_get_ex_new_index(
      0, nullptr, nullptr, nullptr,
      [] (void *parent, void *ptr, CRYPTO_EX_DATA *ad,
          int index, long argl, void *argp) {
    static_cast<SslSessionLRUCache*>(ptr)->Unref();
  });
  return id;
}

void SslSessionLRUCache::InitSslExIndex() {
  int id = GetSslExIndex();
  GPR_ASSERT(id != -1);
}

SslSessionLRUCache* SslSessionLRUCache::GetSelf(SSL* ssl) {
  SSL_CTX* ssl_context = SSL_get_SSL_CTX(ssl);
  if (ssl_context == nullptr) {
      return nullptr;
  }

  return static_cast<SslSessionLRUCache*>(
        SSL_CTX_get_ex_data(ssl_context, GetSslExIndex()));
}

int SslSessionLRUCache::SetNewCallback(SSL* ssl, SSL_SESSION* session) {
  SslSessionLRUCache* self = GetSelf(ssl);
  if (self == nullptr) {
    return 0;
  }

  const char *server_name = SSL_get_servername(ssl, TLSEXT_NAMETYPE_host_name);
  if (server_name == nullptr) {
    return 0;
  }

  self->Put(server_name, SslSessionPtr(session));
  // Return 1 to indicate transfered ownership over the given session.
  return 1;
}

void SslSessionLRUCache::InitContext(tsi_ssl_session_cache* cache, SSL_CTX* ssl_context) {
  auto self = static_cast<SslSessionLRUCache*>(cache);
  // SSL_CTX will call Unref on destruction.
  self->Ref();
  SSL_CTX_set_ex_data(ssl_context, GetSslExIndex(), self);
  SSL_CTX_sess_set_new_cb(ssl_context, SetNewCallback);
}

void SslSessionLRUCache::ResumeSession(SSL* ssl) {
  SslSessionLRUCache* self = GetSelf(ssl);
  if (self == nullptr) {
    return;
  }

  const char *server_name = SSL_get_servername(ssl, TLSEXT_NAMETYPE_host_name);
  if (server_name == nullptr) {
      return;
  }

  SslSessionPtr session = self->Get(server_name);
  if (session) {
    // SSL_set_session internally increments reference counter.
    SSL_set_session(ssl, session.get());
  }
}

} // namespace grpc_core

tsi_ssl_session_cache* tsi_ssl_session_cache_create_lru(size_t capacity) {
  return grpc_core::New<grpc_core::SslSessionLRUCache>(capacity);
}

static grpc_core::SslSessionLRUCache* tsi_ssl_session_cache_get_self(
    tsi_ssl_session_cache* cache) {
  return static_cast<grpc_core::SslSessionLRUCache*>(cache);
}

void tsi_ssl_session_cache_ref(tsi_ssl_session_cache* cache) {
  tsi_ssl_session_cache_get_self(cache)->Ref();
}

void tsi_ssl_session_cache_unref(tsi_ssl_session_cache* cache) {
  tsi_ssl_session_cache_get_self(cache)->Unref();
}
