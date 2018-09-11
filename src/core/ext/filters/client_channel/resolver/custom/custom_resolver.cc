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
#include "src/core/lib/gprpp/lock.h"
#include "src/core/lib/iomgr/closure.h"
#include "src/core/lib/iomgr/combiner.h"
#include "src/core/lib/iomgr/resolve_address.h"
#include "src/core/lib/iomgr/unix_sockets_posix.h"
#include "src/core/lib/slice/slice_internal.h"
#include "src/core/lib/slice/slice_string_helpers.h"

struct grpc_resolver {};
struct grpc_resolver_observer {};
struct grpc_resolver_factory {};
struct grpc_resolver_args {
  const grpc_core::ResolverArgs* args;
};

namespace grpc_core {

namespace {

struct ResolverFactoryVTable {
  void* (*resolve)(void* factory_user_data, grpc_resolver_args* args,
                   grpc_resolver_observer* observer);
  void (*destroy)(void* factory_user_data);
};

struct ResolverVTable {
  void (*request_reresolution)(void* resolver_user_data);
  void (*destroy)(void* resolver_user_data);
};

class CustomResolver : public Resolver {
 public:
  explicit CustomResolver(const ResolverArgs& args, void* user_data,
                          const ResolverVTable& vtable);

  void NextLocked(grpc_channel_args** result,
                  grpc_closure* on_complete) override;

  void RequestReresolutionLocked() override;

 private:
  friend class CustomResolverResponseGenerator;

  virtual ~CustomResolver();

  void MaybeFinishNextLocked();

  void ShutdownLocked() override;

  void* user_data_;
  ResolverVTable vtable_;

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

CustomResolver::CustomResolver(const ResolverArgs& args, void* user_data,
                               const ResolverVTable& vtable)
    : Resolver(args.combiner), user_data_(user_data), vtable_(vtable) {
  channel_args_ = grpc_channel_args_copy(args.args);
}

CustomResolver::~CustomResolver() {
  vtable_.destroy(user_data_);
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

void CustomResolver::RequestReresolutionLocked() {
  vtable_.request_reresolution(user_data_);
}

void CustomResolver::MaybeFinishNextLocked() {
  if (next_completion_ != nullptr && (next_results_ != nullptr)) {
    *target_result_ = grpc_channel_args_union(next_results_, channel_args_);
    grpc_channel_args_destroy(next_results_);
    next_results_ = nullptr;
    GRPC_CLOSURE_SCHED(next_completion_, GRPC_ERROR_NONE);
    next_completion_ = nullptr;
  }
}

void CustomResolver::ShutdownLocked() {
  vtable_.destroy(user_data_);
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

class CustomResolverObserver : public grpc_resolver_observer {
 public:
  CustomResolverObserver(const ResolverArgs& args) {
    channel_args_ = grpc_channel_args_copy(args.args);
  }

  void SetAddresses(grpc_lb_addresses* addresses) {
    LockGuard lock(&lock_);
    grpc_arg new_arg = grpc_lb_addresses_create_channel_arg(addresses);
    auto resolved_channel_args =
        grpc_channel_args_copy_and_add(channel_args_, &new_arg, 1);
  }
  void Stop() {}

 private:
  gpr_mu lock_;
  grpc_channel_args* channel_args_ = nullptr;
  RefCountedPtr<Resolver> resolver_;
};

//
// Factory
//

class CustomResolverFactory : public ResolverFactory,
                              public grpc_resolver_factory {
 public:
  CustomResolverFactory(const char* scheme, void* user_data,
                        const ResolverFactoryVTable& factory_vtable,
                        const ResolverVTable& resolver_vtable)
      : scheme_(gpr_strdup(scheme)),
        user_data_(user_data),
        factory_vtable_(factory_vtable),
        resolver_vtable_(resolver_vtable) {}

  ~CustomResolverFactory() {
    gpr_free(scheme_);
    factory_vtable_.destroy(user_data_);
  }

  OrphanablePtr<Resolver> CreateResolver(
      const ResolverArgs& args) const override {
    auto observer = New<CustomResolverObserver>(args);
    grpc_resolver_args api_args = {&args};
    auto user_data = factory_vtable_.resolve(user_data_, &api_args, observer);
    return OrphanablePtr<Resolver>(
        New<CustomResolver>(args, user_data, resolver_vtable_));
  }

  const char* scheme() const override { return scheme_; }

 private:
  char* scheme_;
  void* user_data_;
  ResolverFactoryVTable factory_vtable_;
  ResolverVTable resolver_vtable_;
};

}  // namespace

}  // namespace grpc_core

void grpc_resolver_custom_init() {}

void grpc_resolver_custom_shutdown() {}

grpc_uri* grpc_resolver_args_get_target(grpc_resolver_args* args) {
  GPR_ASSERT(args != nullptr);
  return args->args->uri;
}

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

void grpc_resolver_factory_register(
    const char* scheme, void* factory_user_data,
    void* (*resolver_factory_resolve)(void* factory_user_data,
                                      grpc_resolver_args* args,
                                      grpc_resolver_observer* observer),
    void (*resolver_factory_destroy)(void* factory_user_data),
    void (*resolver_request_reresolution)(void* resolver_user_data),
    void (*resolver_destroy)(void* resolver_user_data), void* reserved) {
  GPR_ASSERT(reserved == nullptr);
  grpc_core::ResolverFactoryVTable resolver_factory_vtable = {
      resolver_factory_resolve, resolver_factory_destroy};
  grpc_core::ResolverVTable resolver_vtable = {resolver_request_reresolution,
                                               resolver_destroy};
  grpc_core::ResolverRegistry::Builder::RegisterResolverFactory(
      grpc_core::UniquePtr<grpc_core::ResolverFactory>(
          grpc_core::New<grpc_core::CustomResolverFactory>(
              scheme, factory_user_data, resolver_factory_vtable,
              resolver_vtable)));
}

void grpc_resolver_observer_destroy(grpc_resolver_observer* observer) {
  auto custom_observer =
      static_cast<grpc_core::CustomResolverObserver*>(observer);
  grpc_core::Delete(custom_observer);
}

void grpc_resolver_observer_set_addresses(grpc_resolver_observer* observer,
                                          grpc_addresses* addresses) {
  auto custom_observer =
      static_cast<grpc_core::CustomResolverObserver*>(observer);
  custom_observer->SetAddresses(addresses);
}
