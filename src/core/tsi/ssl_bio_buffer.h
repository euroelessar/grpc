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

#ifndef GRPC_CORE_TSI_SSL_BIO_BUFFER_H
#define GRPC_CORE_TSI_SSL_BIO_BUFFER_H

#include <grpc/byte_buffer.h>

#include <openssl/bio.h>

namespace grpc_core {

class BIOByteBufferReader {
public:
    BIOByteBufferReader();
    ~BIOByteBufferReader();

    void SetByteBuffer(grpc_slice_buffer *buffer);

    BIO *bio() { return bio_; }

private:
    static BIOByteBufferReader* GetSelf(BIO* bio);
    int BIORead(char* out, int len);

    static int BIOReadWrapper(BIO* bio, char* out, int len);
    static long BIOCtrlWrapper(BIO* bio, int cmd, long larg, void* parg);

    static const BIO_METHOD kBIOMethod;

    grpc_slice_buffer *buffer_;
    size_t slice_index_;
    size_t data_offset_;
    BIO *bio_;
};

}  // namespace grpc_core

#endif /* GRPC_CORE_TSI_SSL_BIO_BUFFER_H */
