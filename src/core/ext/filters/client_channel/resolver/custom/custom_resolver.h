//
// Copyright 2016 gRPC authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//

#ifndef GRPC_CORE_EXT_FILTERS_CLIENT_CHANNEL_RESOLVER_CUSTOM_CUSTOM_RESOLVER_H
#define GRPC_CORE_EXT_FILTERS_CLIENT_CHANNEL_RESOLVER_CUSTOM_CUSTOM_RESOLVER_H

#include <grpc/support/port_platform.h>

#include "src/core/ext/filters/client_channel/lb_policy_factory.h"
#include "src/core/ext/filters/client_channel/uri_parser.h"
#include "src/core/lib/channel/channel_args.h"
#include "src/core/lib/gprpp/ref_counted.h"

namespace grpc_core {

class CustomResolver;

/// A mechanism for generating responses for the fake resolver.
/// An instance of this class is passed to the fake resolver via a channel
/// argument (see \a MakeChannelArg()) and used to inject and trigger custom
/// resolutions.
// TODO(roth): I would ideally like this to be InternallyRefCounted
// instead of RefCounted, but external refs are currently needed to
// encode this in channel args.  Once channel_args are converted to C++,
// see if we can find a way to fix this.
class CustomResolverResponseGenerator
    : public RefCounted<CustomResolverResponseGenerator> {
 public:
  CustomResolverResponseGenerator() {}

  // Instructs the fake resolver associated with the response generator
  // instance to trigger a new resolution with the specified response.
  void SetResponse(grpc_channel_args* next_response);

  // Sets the re-resolution response, which is returned by the fake resolver
  // when re-resolution is requested (via \a RequestReresolutionLocked()).
  // The new re-resolution response replaces any previous re-resolution
  // response that may have been set by a previous call.
  // If the re-resolution response is set to NULL, then the fake
  // resolver will return the last value set via \a SetResponse().
  void SetReresolutionResponse(grpc_channel_args* response);

  // Tells the resolver to return a transient failure (signalled by
  // returning a null result with no error).
  void SetFailure();

  // Returns a channel arg containing \a generator.
  static grpc_arg MakeChannelArg(CustomResolverResponseGenerator* generator);

  // Returns the response generator in \a args, or null if not found.
  static CustomResolverResponseGenerator* GetFromArgs(
      const grpc_channel_args* args);

 private:
  friend class CustomResolver;

  static void SetResponseLocked(void* arg, grpc_error* error);
  static void SetReresolutionResponseLocked(void* arg, grpc_error* error);
  static void SetFailureLocked(void* arg, grpc_error* error);

  CustomResolver* resolver_ = nullptr;  // Do not own.
};

}  // namespace grpc_core

#endif /* GRPC_CORE_EXT_FILTERS_CLIENT_CHANNEL_RESOLVER_CUSTOM_CUSTOM_RESOLVER_H \
        */
