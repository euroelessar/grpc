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

#ifndef GRPC_CORE_TSI_SSL_SESSION_CACHE_H
#define GRPC_CORE_TSI_SSL_SESSION_CACHE_H

#include <list>
#include <unordered_map>

#include <grpc/support/sync.h>
#include <grpc/slice.h>

extern "C" {
#include <openssl/ssl.h>
}

#include "src/core/lib/gprpp/memory.h"

struct tsi_ssl_session_cache {};

namespace grpc_core {

struct SliceHash {
  uint32_t operator()(const grpc_slice& slice) const noexcept {
    return grpc_slice_hash(slice);
  }
};

struct SliceEqualTo {
  bool operator()(const grpc_slice& a, const grpc_slice& b) const {
    return grpc_slice_cmp(a, b) == 0;
  }
};

struct SslSessionDeleter {
  void operator()(SSL_SESSION* session) {
    SSL_SESSION_free(session);
  }
};

typedef std::unique_ptr<SSL_SESSION, SslSessionDeleter> SslSessionPtr;

SslSessionPtr ssl_session_clone(const SslSessionPtr &other);

class SslSession {
public:
  SslSession(const grpc_slice& key, SslSessionPtr session)
      : key_(key), session_(std::move(session)) {
  }

  ~SslSession() {
    grpc_slice_unref(key_);
  }

  // Not copyable nor movable.
  SslSession(const SslSession&) = delete;
  SslSession& operator=(const SslSession&) = delete;

  const grpc_slice& Key() const {
    return key_;
  }

  SslSessionPtr GetSession() const {
    return ssl_session_clone(session_);
  }

  void SetSession(SslSessionPtr session) {
    session_ = std::move(session);
  }

private:
  grpc_slice key_;
  SslSessionPtr session_;
};

class SslSessionLRUCache : public tsi_ssl_session_cache {
public:
  SslSessionLRUCache(size_t capacity);
  ~SslSessionLRUCache();

  void Ref() { gpr_ref(&ref_); }
  void Unref() {
    if (gpr_unref(&ref_)) {
      Delete(this);
    }
  }

  void Put(const char* key, SslSessionPtr session);
  SslSessionPtr Get(const char* key);
  size_t Size();

  static void InitContext(tsi_ssl_session_cache* cache, SSL_CTX* ssl_context);
  static void InitSslExIndex();
  static void ResumeSession(SSL* ssl);

private:
  typedef std::list<SslSession, Allocator<SslSession>> SslSessionList;

  SslSessionList::iterator FindLocked(const grpc_slice& key);

  static SslSessionLRUCache* GetSelf(SSL* ssl);
  static int GetSslExIndex();
  static int SetNewCallback(SSL* ssl, SSL_SESSION* session);

  gpr_refcount ref_;
  gpr_mu lock_;
  size_t capacity_;

  std::list<SslSession, Allocator<SslSession>> use_order_list_;
  // Slice is owned by list.
  std::unordered_map<
      grpc_slice,
      SslSessionList::iterator,
      SliceHash,
      SliceEqualTo,
      Allocator<std::pair<const grpc_slice, SslSessionList::iterator>>>
      entry_by_key_;
};

} // namespace grpc_core

#endif /* GRPC_CORE_TSI_SSL_SESSION_CACHE_H */
