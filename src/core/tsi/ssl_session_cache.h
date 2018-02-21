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

class SslSession {
public:
  SslSession(const grpc_slice &key, SSL_SESSION* session)
      : key_(key), session_(session) {
    SSL_SESSION_up_ref(session_);
  }

  ~SslSession() {
    grpc_slice_unref(key_);
    SSL_SESSION_free(session_);
  }

  // Not copyable nor movable.
  SslSession(const SslSession&) = delete;
  SslSession& operator=(const SslSession&) = delete;

  const grpc_slice& Key() const {
    return key_;
  }

  SSL_SESSION* GetSession() const {
    SSL_SESSION_up_ref(session_);
    return session_;
  }

  void SetSession(SSL_SESSION* session) {
    SSL_SESSION_free(session_);
    session_ = session;
    SSL_SESSION_up_ref(session_);
  }

private:
  grpc_slice key_;
  SSL_SESSION* session_;
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

  void InitContext(SSL_CTX* ssl_context);
  static void InitSslExIndex();
  static void ResumeSession(SSL* ssl);

private:
  typedef std::list<SslSession, Allocator<SslSession>> SslSessionList;

  SslSessionList::iterator FindLocked(const grpc_slice& key);

  void Put(const char* key, SSL_SESSION* session);
  SSL_SESSION* Get(const char* key);

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
