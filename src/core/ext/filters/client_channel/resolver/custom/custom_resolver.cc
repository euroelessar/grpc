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

// This is similar to the sockaddr resolver, except that it supports a
// bunch of query args that are useful for dependency injection in tests.

#include <grpc/support/port_platform.h>

#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <grpc/support/alloc.h>
#include <grpc/support/string_util.h>

#include "src/core/ext/filters/client_channel/lb_policy_factory.h"
#include "src/core/ext/filters/client_channel/parse_address.h"
#include "src/core/ext/filters/client_channel/resolver_registry.h"
#include "src/core/lib/channel/channel_args.h"
#include "src/core/lib/gpr/host_port.h"
#include "src/core/lib/gpr/string.h"
#include "src/core/lib/iomgr/closure.h"
#include "src/core/lib/iomgr/combiner.h"
#include "src/core/lib/iomgr/resolve_address.h"
#include "src/core/lib/iomgr/unix_sockets_posix.h"
#include "src/core/lib/slice/slice_internal.h"
#include "src/core/lib/slice/slice_string_helpers.h"

namespace grpc_core {

struct grpc_resolver {};
struct grpc_resolver_factory {};

namespace {

struct grpc_addresses {
  size_t capacity;
};

class CustomResolver : public Resolver {
 public:
  explicit CustomResolver(const ResolverArgs& args);

  void NextLocked(grpc_channel_args** result,
                  grpc_closure* on_complete) override;

  void RequestReresolutionLocked() override;

 private:
  friend class CustomResolverResponseGenerator;

  virtual ~CustomResolver();

  void MaybeFinishNextLocked();

  void ShutdownLocked() override;

  // Passed-in parameters
  grpc_channel_args* channel_args_ = nullptr;
  // If not NULL, the next set of resolution results to be returned to
  // NextLocked()'s closure.
  grpc_channel_args* next_results_ = nullptr;
  // pending next completion, or NULL
  grpc_closure* next_completion_ = nullptr;
  // target result address for next completion
  grpc_channel_args** target_result_ = nullptr;
};

CustomResolver::CustomResolver(const ResolverArgs& args)
    : Resolver(args.combiner) {
  channel_args_ = grpc_channel_args_copy(args.args);
}

CustomResolver::~CustomResolver() {
  grpc_channel_args_destroy(next_results_);
  grpc_channel_args_destroy(channel_args_);
}

void CustomResolver::NextLocked(grpc_channel_args** target_result,
                                grpc_closure* on_complete) {
  GPR_ASSERT(next_completion_ == nullptr);
  next_completion_ = on_complete;
  target_result_ = target_result;
  MaybeFinishNextLocked();
}

void CustomResolver::RequestReresolutionLocked() {}

void CustomResolver::MaybeFinishNextLocked() {
  if (next_completion_ != nullptr && (next_results_ != nullptr)) {
    *target_result_ = grpc_channel_args_union(next_results_, channel_args_);
    grpc_channel_args_destroy(next_results_);
    next_results_ = nullptr;
    GRPC_CLOSURE_SCHED(next_completion_, GRPC_ERROR_NONE);
    next_completion_ = nullptr;
    return_failure_ = false;
  }
}

void CustomResolver::ShutdownLocked() {
  if (next_completion_ != nullptr) {
    *target_result_ = nullptr;
    GRPC_CLOSURE_SCHED(next_completion_, GRPC_ERROR_CREATE_FROM_STATIC_STRING(
                                             "Resolver Shutdown"));
    next_completion_ = nullptr;
  }
}

//
// Observer
//

class CustomObserver : public grpc_resolver {};

//
// Factory
//

class CustomResolverFactory : public ResolverFactory,
                              public grpc_resolver_factory {
 public:
  CustomResolverFactory(const char* scheme) : _scheme(gpr_strdup(scheme)) {}
  ~CustomResolverFactory() { gpr_free(scheme_); }

  OrphanablePtr<Resolver> CreateResolver(
      const ResolverArgs& args) const override {
    return OrphanablePtr<Resolver>(New<CustomResolver>(args));
  }

  const char* scheme() const override { return scheme_; }

 private:
  char* scheme_;
};

}  // namespace

}  // namespace grpc_core

void grpc_resolver_custom_init() {}

void grpc_resolver_custom_shutdown() {}

grpc_addresses* grpc_addresses_create(size_t num_addresses, void* reserved) {
  GPR_ASSERT(reserved == nullptr);
  return grpc_lb_addresses_create(num_addresses, nullptr);
}

void grpc_addresses_destroy(grpc_addresses* addresses) {
  grpc_lb_addresses_destroy(addresses);
}

void grpc_addresses_set_direct_address(grpc_addresses* addresses, size_t index,
                                       const void* address,
                                       size_t address_len) {
  grpc_lb_addresses_set_address(addresses, index, address, address_len, false,
                                nullptr, nullptr);
}

void grpc_addresses_set_balancer_address(grpc_addresses* addresses,
                                         size_t index, const void* address,
                                         size_t address_len,
                                         const char* balancer_name) {
  grpc_lb_addresses_set_address(addresses, index, address, address_len, true,
                                balancer_name, nullptr);
}

grpc_resolver_factory* grpc_resolver_factory_create(const char* scheme,
                                                    void* reserved) {
  GPR_ASSERT(reserved == nullptr);
  grpc_core::ResolverRegistry::Builder::RegisterResolverFactory(
      grpc_core::UniquePtr<grpc_core::ResolverFactory>(
          grpc_core::New<grpc_core::CustomResolverFactory>(scheme)));
  return nullptr;
}

void grpc_resolver_factory_watch_next(grpc_resolver_factory* factory,
                                      grpc_resolver** resolver, grpc_uri** uri,
                                      grpc_completion_queue* cq, void* tag,
                                      void* reserved) {
  GPR_ASSERT(reserved == nullptr);
  auto custom_factory = static_cast<grpc_core::CustomResolverFactory*>(factory);
  custom_factory->WatchNext(resolver, uri, cq, tag);
}

void grpc_resolver_destroy(grpc_resolver* resolver) {
  auto custom_observer = static_cast<grpc_core::CustomObserver*>(resolver);
  custom_observer->Unref();
}

void grpc_resolver_watch_shutdown(grpc_resolver* resolver,
                                  grpc_completion_queue* cq, void* tag,
                                  void* reserved) {
  GPR_ASSERT(reserved == nullptr);
  auto custom_observer = static_cast<grpc_core::CustomObserver*>(resolver);
  custom_observer->WatchShutdown(cq, tag);
}

void grpc_resolver_set_addresses(grpc_resolver* resolver,
                                 grpc_addresses* addresses) {
  auto custom_observer = static_cast<grpc_core::CustomObserver*>(resolver);
  custom_observer->SetAddresses(addresses);
}
