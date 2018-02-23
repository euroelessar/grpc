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

#include "src/core/tsi/ssl_session_cache.h"

#include <grpc/support/log.h>
#include <grpc/support/string_util.h>

namespace grpc_core {

SslSessionPtr ssl_session_clone(const SslSessionPtr& other) {
  SSL_SESSION_up_ref(other.get());
  return SslSessionPtr(other.get());
}

static void cache_key_avl_destroy(void* key, void* unused) {
}

static void* cache_key_avl_copy(void* key, void* unused) {
  return key;
}

static long cache_key_avl_compare(void* key1, void* key2, void* unused) {
  return grpc_slice_cmp(*static_cast<grpc_slice*>(key1),
                        *static_cast<grpc_slice*>(key2));
}

static void cache_value_avl_destroy(void* value, void* unused) {
}

static void* cache_value_avl_copy(void* value, void* unused) {
  return value;
}

// AVL only stores pointers, ownership belonges to linked list.
static const grpc_avl_vtable cache_avl_vtable = {
  cache_key_avl_destroy, cache_key_avl_copy, cache_key_avl_compare,
  cache_value_avl_destroy, cache_value_avl_copy,
};

class SslSessionLRUCache::Node {
public:
  Node(const grpc_slice& key, SslSessionPtr session)
      : key_(key), session_(std::move(session)) {
  }

  ~Node() {
    grpc_slice_unref(key_);
  }

  // Not copyable nor movable.
  Node(const Node&) = delete;
  Node& operator=(const Node&) = delete;

  void* AvlKey() {
    return &key_;
  }

  SslSessionPtr GetSession() const {
    return ssl_session_clone(session_);
  }

  void SetSession(SslSessionPtr session) {
    session_ = std::move(session);
  }

private:
  friend class SslSessionLRUCache;

  grpc_slice key_;
  SslSessionPtr session_;

  Node* next_ = nullptr;
  Node* prev_ = nullptr;
};

SslSessionLRUCache::SslSessionLRUCache(size_t capacity) : capacity_(capacity) {
  GPR_ASSERT(capacity > 0);
  gpr_ref_init(&ref_, 1);
  gpr_mu_init(&lock_);

  entry_by_key_ = grpc_avl_create(&cache_avl_vtable);
}

SslSessionLRUCache::~SslSessionLRUCache() {
  Node* node = use_order_list_head_;
  while (node) {
    Node* next = node->next_;
    Delete(node);
    node = next;
  }

  grpc_avl_unref(entry_by_key_, nullptr);
  gpr_mu_destroy(&lock_);
}

void SslSessionLRUCache::Put(const char* key, SslSessionPtr session) {
  grpc_slice key_slice = grpc_slice_from_copied_string(key);
  mu_guard guard(&lock_);

  Node* node = FindLocked(key_slice);
  if (node != nullptr) {
    grpc_slice_unref(key_slice);
    node->SetSession(std::move(session));
    return;
  }

  node = New<Node>(key_slice, std::move(session));
  PushFront(node);
  entry_by_key_ = grpc_avl_add(entry_by_key_, node->AvlKey(), node, nullptr);

  if (use_order_list_size_ > capacity_) {
    GPR_ASSERT(use_order_list_tail_);
    node = use_order_list_tail_;
    Remove(node);

    // Order matters, key is destroyed after deleting node.
    entry_by_key_ = grpc_avl_remove(entry_by_key_, node->AvlKey(), nullptr);
    Delete(node);
  }
}

SslSessionPtr SslSessionLRUCache::Get(const char* key) {
  // Key is only used for lookups.
  grpc_slice key_slice = grpc_slice_from_static_string(key);
  mu_guard guard(&lock_);

  Node* node = FindLocked(key_slice);
  if (node == nullptr) {
    return nullptr;
  }

  return node->GetSession();
}

size_t SslSessionLRUCache::Size() {
  mu_guard guard(&lock_);
  return use_order_list_size_;
}

SslSessionLRUCache::Node* SslSessionLRUCache::FindLocked(
    const grpc_slice& key) {
  void* value = grpc_avl_get(
      entry_by_key_, const_cast<grpc_slice *>(&key), nullptr);
  if (value == nullptr) {
    return nullptr;
  }

  Node* node = static_cast<Node*>(value);

  // Move to the beginning.
  Remove(node);
  PushFront(node);

  return node;
}

void SslSessionLRUCache::Remove(SslSessionLRUCache::Node* node) {
  if (node->prev_ == nullptr) {
    use_order_list_head_ = node->next_;
  } else {
    node->prev_->next_ = node->next_;
  }

  if (node->next_ == nullptr) {
    use_order_list_tail_ = node->prev_;
  } else {
    node->next_->prev_ = node->prev_;
  }

  GPR_ASSERT(use_order_list_size_ >= 1);
  use_order_list_size_--;
  AssertInvariants();
}

void SslSessionLRUCache::PushFront(SslSessionLRUCache::Node* node) {
  if (use_order_list_head_ == nullptr) {
    use_order_list_head_ = node;
    use_order_list_tail_ = node;
    node->next_ = nullptr;
    node->prev_ = nullptr;
  } else {
    node->next_ = use_order_list_head_;
    node->next_->prev_ = node;
    use_order_list_head_ = node;
    node->prev_ = nullptr;
  }
  use_order_list_size_++;
  AssertInvariants();
}

#ifndef NDEBUG
void SslSessionLRUCache::AssertInvariants() {
  size_t size = 0;
  Node* prev = nullptr;
  Node* current = use_order_list_head_;
  while (current != nullptr) {
    size++;
    GPR_ASSERT(current->prev_ == prev);

    prev = current;
    current = current->next_;
  }

  GPR_ASSERT(prev == use_order_list_tail_);
  GPR_ASSERT(size == use_order_list_size_);
}
#else
void SslSessionLRUCache::AssertInvariants() {
}
#endif

int SslSessionLRUCache::SslExIndex = -1;

void SslSessionLRUCache::InitSslExIndex() {
  SslExIndex = SSL_CTX_get_ex_new_index(
      0, nullptr, nullptr, nullptr,
      [] (void *parent, void *ptr, CRYPTO_EX_DATA *ad,
          int index, long argl, void *argp) {
    static_cast<SslSessionLRUCache*>(ptr)->Unref();
  });
  GPR_ASSERT(SslExIndex != -1);
}

SslSessionLRUCache* SslSessionLRUCache::GetSelf(SSL* ssl) {
  SSL_CTX* ssl_context = SSL_get_SSL_CTX(ssl);
  if (ssl_context == nullptr) {
      return nullptr;
  }

  return static_cast<SslSessionLRUCache*>(
        SSL_CTX_get_ex_data(ssl_context, SslExIndex));
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
  SSL_CTX_set_ex_data(ssl_context, SslExIndex, self);
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
