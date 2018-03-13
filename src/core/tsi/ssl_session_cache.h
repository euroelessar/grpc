/*
 *
 * Copyright 2018 gRPC authors.
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

#include <grpc/support/port_platform.h>

#include <grpc/slice.h>
#include <grpc/support/sync.h>

extern "C" {
#include <openssl/ssl.h>
}

#include "src/core/lib/avl/avl.h"
#include "src/core/lib/gprpp/memory.h"
#include "src/core/tsi/ssl_session.h"

namespace grpc_core {

class SslSessionCacheTest;

class SslSessionLRUCache {
 public:
  explicit SslSessionLRUCache(size_t capacity);
  ~SslSessionLRUCache();

  // Not copyable nor movable.
  SslSessionLRUCache(const SslSessionLRUCache&) = delete;
  SslSessionLRUCache& operator=(const SslSessionLRUCache&) = delete;

  void Ref() { gpr_ref(&ref_); }
  void Unref() {
    if (gpr_unref(&ref_)) {
      Delete(this);
    }
  }

  size_t Size();

  void InitContext(SSL_CTX* ssl_context);
  static void InitSslExIndex();
  static void ResumeSession(SSL* ssl);

 private:
  class Node;
  friend class grpc_core::SslSessionCacheTest;

  Node* FindLocked(const grpc_slice& key);
  void PutLocked(const char* key, SslSessionPtr session);
  SslSessionPtr GetLocked(const char* key);
  void Remove(Node* node);
  void PushFront(Node* node);
  void AssertInvariants();

  static int SslExIndex;
  static SslSessionLRUCache* GetSelf(SSL* ssl);
  static int SetNewCallback(SSL* ssl, SSL_SESSION* session);

  gpr_refcount ref_;
  gpr_mu lock_;
  size_t capacity_;

  Node* use_order_list_head_ = nullptr;
  Node* use_order_list_tail_ = nullptr;
  size_t use_order_list_size_ = 0;
  // Slice is owned by list.
  grpc_avl entry_by_key_;
};

}  // namespace grpc_core

#endif /* GRPC_CORE_TSI_SSL_SESSION_CACHE_H */
