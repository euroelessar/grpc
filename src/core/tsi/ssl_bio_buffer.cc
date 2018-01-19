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

#include "ssl_bio_buffer.h"

#include <grpc/support/log.h>

#include <algorithm>
#include <cstring>

namespace grpc_core {

BIOByteBufferReader::BIOByteBufferReader()
    : buffer_(nullptr),
      slice_index_(0),
      data_offset_(0) {
  bio_ = BIO_new(&kBIOMethod);
  bio_->ptr = this;
}

BIOByteBufferReader::~BIOByteBufferReader() {
  // BIO is reference-counted and may outlive socket.
  bio_->ptr = nullptr;
  BIO_free(bio_);
}

void BIOByteBufferReader::SetByteBuffer(grpc_slice_buffer* buffer) {
  buffer_ = buffer;
  slice_index_ = 0;
  data_offset_ = 0;
}

BIOByteBufferReader* BIOByteBufferReader::GetSelf(BIO* bio) {
  GPR_ASSERT(&kBIOMethod == bio->method);
  auto self = reinterpret_cast<BIOByteBufferReader*>(bio->ptr);
  if (self)
    GPR_ASSERT(bio == self->bio());
  return self;
}

int BIOByteBufferReader::BIORead(char* out, int signed_len) {
  if (signed_len < 0) {
    return -1;
  }

  size_t out_len = signed_len;
  size_t written = 0;

  while (slice_index_ < buffer_->count) {
    grpc_slice &slice = buffer_->slices[slice_index_];
    uint8_t* slice_data = GRPC_SLICE_START_PTR(slice);
    size_t slice_len = GRPC_SLICE_LENGTH(slice);

    size_t len_to_write = std::min(slice_len - data_offset_, out_len - written);
    std::memcpy(out + written, slice_data + data_offset_, len_to_write);
    written += len_to_write;
    data_offset_ += len_to_write;

    if (out_len > written) {
      break;
    }

    slice_index_++;
    data_offset_ = 0;
  }

  return written;
}

int BIOByteBufferReader::BIOReadWrapper(BIO* bio, char* out, int len) {
  BIO_clear_retry_flags(bio);

  BIOByteBufferReader* self = GetSelf(bio);
  if (!self) {
    OPENSSL_PUT_ERROR(BIO, ERR_R_SHOULD_NOT_HAVE_BEEN_CALLED);
    return -1;
  }

  return self->BIORead(out, len);
}

long BIOByteBufferReader::BIOCtrlWrapper(BIO* bio, int cmd, long larg, void* parg) {
  return 0;
}

static const BIO_METHOD BIOByteBufferReader::kBIOMethod = {
    0,       // type (unused)
    nullptr, // name (unused)
    nullptr, // write
    BIOByteBufferReader::BIOReadWrapper,
    nullptr, // puts
    nullptr, // gets,
    BIOByteBufferReader::BIOCtrlWrapper,
    nullptr, // destroy
    nullptr, // callback_ctrl
};

}  // namespace grpc_core
