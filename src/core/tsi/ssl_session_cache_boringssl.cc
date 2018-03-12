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

#ifdef OPENSSL_IS_BORINGSSL


// BoringSSL and OpenSSL have different behavior regarding TLS ticket
// resumption.
//
// BoringSSL allows SSL_SESSION to outlive SSL and SSL_CTX objects which are
// re-created by gRPC on every cert rotation/subchannel creation.
// SSL_SESSION is also immutable in BoringSSL and it's safe to share
// the same session between different threads and connections.
//
// OpenSSL invalidates SSL_SESSION on SSL destruction making it pointless
// to cache sessions. The workaround is to serialize (relatively expensive)
// session into binary blob and re-create it from blob on every handshake.
void SslSessionMaybeDeleter::operator()(SSL_SESSION* session) {
  SSL_SESSION_free(session);
}

class SslSessionLRUCache::Node {
 public:
  Node(const grpc_slice& key, SslSessionPtr session) : key_(key) {
    SetSession(std::move(session));
  }

  ~Node() {
    grpc_slice_unref(key_);
  }

  // Not copyable nor movable.
  Node(const Node&) = delete;
  Node& operator=(const Node&) = delete;

  void* AvlKey() { return &key_; }

#ifdef OPENSSL_IS_BORINGSSL
  SslSessionGetResult GetSession() const {
    return SslSessionGetResult(session_.get());
  }

  void SetSession(SslSessionPtr session) { session_ = std::move(session); }
#else
  SslSessionGetResult GetSession() const {
    const unsigned char* data = GRPC_SLICE_START_PTR(session_);
    size_t length = GRPC_SLICE_LENGTH(session_);
    SSL_SESSION* session = d2i_SSL_SESSION(nullptr, &data, length);
    if (session == nullptr) {
      return SslSessionGetResult();
    }
    return SslSessionGetResult(session);
  }

  void SetSession(SslSessionPtr session) {
    int size = i2d_SSL_SESSION(session.get(), nullptr);
    GPR_ASSERT(size > 0);
    grpc_slice slice = grpc_slice_malloc(size_t(size));
    unsigned char* start = GRPC_SLICE_START_PTR(slice);
    int second_size = i2d_SSL_SESSION(session.get(), &start);
    GPR_ASSERT(size == second_size);
    grpc_slice_unref(session_);
    session_ = slice;
  }
#endif

 private:
  friend class SslSessionLRUCache;

  grpc_slice key_;
#ifdef OPENSSL_IS_BORINGSSL
  SslSessionPtr session_;
#else
  grpc_slice session_;
#endif

  Node* next_ = nullptr;
  Node* prev_ = nullptr;
};

#endif /* OPENSSL_IS_BORINGSSL */
