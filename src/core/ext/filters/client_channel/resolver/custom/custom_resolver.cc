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

#include "src/core/ext/filters/client_channel/resolver/custom/custom_resolver.h"

namespace grpc_core {

struct grpc_resolver {};
struct grpc_resolver_factory {};

// This cannot be in an anonymous namespace, because it is a friend of
// CustomResolverResponseGenerator.
class CustomResolver : public Resolver, public grpc_resolver {
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

  // passed-in parameters
  grpc_channel_args* channel_args_ = nullptr;
  // If not NULL, the next set of resolution results to be returned to
  // NextLocked()'s closure.
  grpc_channel_args* next_results_ = nullptr;
  // Results to use for the pretended re-resolution in
  // RequestReresolutionLocked().
  grpc_channel_args* reresolution_results_ = nullptr;
  // TODO(juanlishen): This can go away once pick_first is changed to not throw
  // away its subchannels, since that will eliminate its dependence on
  // channel_saw_error_locked() causing an immediate resolver return.
  // A copy of the most-recently used resolution results.
  grpc_channel_args* last_used_results_ = nullptr;
  // pending next completion, or NULL
  grpc_closure* next_completion_ = nullptr;
  // target result address for next completion
  grpc_channel_args** target_result_ = nullptr;
  // if true, return failure
  bool return_failure_ = false;
};

CustomResolver::CustomResolver(const ResolverArgs& args)
    : Resolver(args.combiner) {
  channel_args_ = grpc_channel_args_copy(args.args);
  CustomResolverResponseGenerator* response_generator =
      CustomResolverResponseGenerator::GetFromArgs(args.args);
  if (response_generator != nullptr) response_generator->resolver_ = this;
}

CustomResolver::~CustomResolver() {
  grpc_channel_args_destroy(next_results_);
  grpc_channel_args_destroy(reresolution_results_);
  grpc_channel_args_destroy(last_used_results_);
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
  // A resolution must have been returned before an error is seen.
  GPR_ASSERT(last_used_results_ != nullptr);
  grpc_channel_args_destroy(next_results_);
  if (reresolution_results_ != nullptr) {
    next_results_ = grpc_channel_args_copy(reresolution_results_);
  } else {
    // If reresolution_results is unavailable, re-resolve with the most-recently
    // used results to avoid a no-op re-resolution.
    next_results_ = grpc_channel_args_copy(last_used_results_);
  }
  MaybeFinishNextLocked();
}

void CustomResolver::MaybeFinishNextLocked() {
  if (next_completion_ != nullptr &&
      (next_results_ != nullptr || return_failure_)) {
    *target_result_ =
        return_failure_ ? nullptr
                        : grpc_channel_args_union(next_results_, channel_args_);
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
// CustomResolverResponseGenerator
//

struct SetResponseClosureArg {
  grpc_closure set_response_closure;
  CustomResolverResponseGenerator* generator;
  grpc_channel_args* response;
};

void CustomResolverResponseGenerator::SetResponseLocked(void* arg,
                                                        grpc_error* error) {
  SetResponseClosureArg* closure_arg = static_cast<SetResponseClosureArg*>(arg);
  CustomResolver* resolver = closure_arg->generator->resolver_;
  grpc_channel_args_destroy(resolver->next_results_);
  resolver->next_results_ = closure_arg->response;
  grpc_channel_args_destroy(resolver->last_used_results_);
  resolver->last_used_results_ = grpc_channel_args_copy(closure_arg->response);
  resolver->MaybeFinishNextLocked();
  Delete(closure_arg);
}

void CustomResolverResponseGenerator::SetResponse(grpc_channel_args* response) {
  GPR_ASSERT(response != nullptr);
  GPR_ASSERT(resolver_ != nullptr);
  SetResponseClosureArg* closure_arg = New<SetResponseClosureArg>();
  closure_arg->generator = this;
  closure_arg->response = grpc_channel_args_copy(response);
  GRPC_CLOSURE_SCHED(
      GRPC_CLOSURE_INIT(&closure_arg->set_response_closure, SetResponseLocked,
                        closure_arg,
                        grpc_combiner_scheduler(resolver_->combiner())),
      GRPC_ERROR_NONE);
}

void CustomResolverResponseGenerator::SetReresolutionResponseLocked(
    void* arg, grpc_error* error) {
  SetResponseClosureArg* closure_arg = static_cast<SetResponseClosureArg*>(arg);
  CustomResolver* resolver = closure_arg->generator->resolver_;
  grpc_channel_args_destroy(resolver->reresolution_results_);
  resolver->reresolution_results_ = closure_arg->response;
  Delete(closure_arg);
}

void CustomResolverResponseGenerator::SetReresolutionResponse(
    grpc_channel_args* response) {
  GPR_ASSERT(resolver_ != nullptr);
  SetResponseClosureArg* closure_arg = New<SetResponseClosureArg>();
  closure_arg->generator = this;
  closure_arg->response =
      response != nullptr ? grpc_channel_args_copy(response) : nullptr;
  GRPC_CLOSURE_SCHED(
      GRPC_CLOSURE_INIT(&closure_arg->set_response_closure,
                        SetReresolutionResponseLocked, closure_arg,
                        grpc_combiner_scheduler(resolver_->combiner())),
      GRPC_ERROR_NONE);
}

void CustomResolverResponseGenerator::SetFailureLocked(void* arg,
                                                       grpc_error* error) {
  SetResponseClosureArg* closure_arg = static_cast<SetResponseClosureArg*>(arg);
  CustomResolver* resolver = closure_arg->generator->resolver_;
  resolver->return_failure_ = true;
  resolver->MaybeFinishNextLocked();
  Delete(closure_arg);
}

void CustomResolverResponseGenerator::SetFailure() {
  GPR_ASSERT(resolver_ != nullptr);
  SetResponseClosureArg* closure_arg = New<SetResponseClosureArg>();
  closure_arg->generator = this;
  GRPC_CLOSURE_SCHED(
      GRPC_CLOSURE_INIT(&closure_arg->set_response_closure, SetFailureLocked,
                        closure_arg,
                        grpc_combiner_scheduler(resolver_->combiner())),
      GRPC_ERROR_NONE);
}

namespace {

//
// Factory
//

namespace {

class CustomResolverFactory : public ResolverFactory {
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

}  // namespace

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

grpc_resolver* grpc_resolver_create(grpc_resolver_factory* factory,
                                    void* reserved) {
  GPR_ASSERT(reserved == nullptr);
}

void grpc_resolver_destroy(grpc_resolver* resolver) {}

void grpc_resolver_watch_initialization(grpc_resolver* resolver, grpc_uri** uri,
                                        grpc_completion_queue* cq, void* tag,
                                        void* reserved) {
  GPR_ASSERT(reserved == nullptr);
}

void grpc_resolver_watch_shutdown(grpc_resolver* resolver,
                                  grpc_completion_queue* cq, void* tag,
                                  void* reserved) {
  GPR_ASSERT(reserved == nullptr);
}

void grpc_resolver_set_addresses(grpc_resolver* resolver,
                                 grpc_addresses* addresses) {}
