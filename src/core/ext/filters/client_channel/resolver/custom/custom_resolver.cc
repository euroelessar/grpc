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

static void* response_generator_arg_copy(void* p) {
  CustomResolverResponseGenerator* generator =
      static_cast<CustomResolverResponseGenerator*>(p);
  // TODO(roth): We currently deal with this ref manually.  Once the
  // new channel args code is converted to C++, find a way to track this ref
  // in a cleaner way.
  RefCountedPtr<CustomResolverResponseGenerator> copy = generator->Ref();
  copy.release();
  return p;
}

static void response_generator_arg_destroy(void* p) {
  CustomResolverResponseGenerator* generator =
      static_cast<CustomResolverResponseGenerator*>(p);
  generator->Unref();
}

static int response_generator_cmp(void* a, void* b) { return GPR_ICMP(a, b); }

static const grpc_arg_pointer_vtable response_generator_arg_vtable = {
    response_generator_arg_copy, response_generator_arg_destroy,
    response_generator_cmp};

}  // namespace

grpc_arg CustomResolverResponseGenerator::MakeChannelArg(
    CustomResolverResponseGenerator* generator) {
  grpc_arg arg;
  arg.type = GRPC_ARG_POINTER;
  arg.key = (char*)GRPC_ARG_custom_RESOLVER_RESPONSE_GENERATOR;
  arg.value.pointer.p = generator;
  arg.value.pointer.vtable = &response_generator_arg_vtable;
  return arg;
}

CustomResolverResponseGenerator* CustomResolverResponseGenerator::GetFromArgs(
    const grpc_channel_args* args) {
  const grpc_arg* arg =
      grpc_channel_args_find(args, GRPC_ARG_custom_RESOLVER_RESPONSE_GENERATOR);
  if (arg == nullptr || arg->type != GRPC_ARG_POINTER) return nullptr;
  return static_cast<CustomResolverResponseGenerator*>(arg->value.pointer.p);
}

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

struct QueryArgument {
  QueryArgument(const char* name, const char* value) {}

  char* name;
  char* value;
};

class CustomResolverTarget : public grpc_resolver_target {
 public:
  CustomResolverTarget(grpc_uri* uri) {
    scheme_ = gpr_strdup(uri->scheme);
    authority_ = gpr_strdup(uri->authority);
    path_ = gpr_strdup(uri->path);
    for (size_t i = 0; i < uri->num_query_parts; ++i) {
      char* name = gpr_strdup(uri->query_parts[i]);
      char* value = nullptr;
      if (uri->query_parts_values[i]) {
        value = gpr_strdup(uri->query_parts_values[i]);
      }
      query_arguments_.emplace_back(name, value);
    }
  }

  ~CustomResolverTarget() {
    gpr_free(scheme_);
    gpr_free(authority_);
    gpr_free(path_);
    for (size_t i = 0; i < query_arguments_.size(); ++i) {
      gpr_free(query_arguments_[i].first);
      if (query_arguments_[i].second) {
        gpr_free(query_arguments_[i].second);
      }
    }
  }

  const char* GetScheme() const { return scheme_; }
  const char* GetAuthority() const { return authority_; }
  const char* GetPath() const { return path_; }
  size_t GetQueryArgumentsNum() const { return query_arguments_.size(); }
  const char* GetQueryArgumentName(size_t index) const {
    GPR_ASSERT(index < query_arguments_.size());
    return query_arguments_[index].first;
  }
  const char* GetQueryArgumentValue(size_t index) const {
    GPR_ASSERT(index < query_arguments_.size());
    return query_arguments_[index].second;
  }

 private:
  char* scheme_;
  char* authority_;
  char* path_;
  InlinedVector<std::pair<char*, char*>, 5> query_arguments_;
};

}  // namespace

}  // namespace grpc_core

void grpc_resolver_custom_init() {
  grpc_core::ResolverRegistry::Builder::RegisterResolverFactory(
      grpc_core::UniquePtr<grpc_core::ResolverFactory>(
          grpc_core::New<grpc_core::CustomResolverFactory>()));
}

void grpc_resolver_custom_shutdown() {}

const char* grpc_resolver_target_get_scheme(grpc_resolver_target* target) {
  return static_cast<grpc_core::CustomResolverTarget*>(target)->GetScheme();
}

const char* grpc_resolver_target_get_authority(grpc_resolver_target* target) {
  return static_cast<grpc_core::CustomResolverTarget*>(target)->GetAuthority();
}

const char* grpc_resolver_target_get_path(grpc_resolver_target* target) {
  return static_cast<grpc_core::CustomResolverTarget*>(target)->GetPath();
}

size_t grpc_resolver_get_target_query_arguments_num(
    grpc_resolver_target* target) {
  return static_cast<grpc_core::CustomResolverTarget*>(target)
      ->GetQueryArgumentsNum();
}

const char* grpc_resolver_target_get_query_argument_name(
    grpc_resolver_target* target, size_t index) {
  return static_cast<grpc_core::CustomResolverTarget*>(target)
      ->GetQueryArgumentName(index);
}

const char* grpc_resolver_target_get_query_argument_value(
    grpc_resolver_target* target, size_t index) {
  return static_cast<grpc_core::CustomResolverTarget*>(target)
      ->GetQueryArgumentValue(index);
}

grpc_addresses* grpc_new_addresses(size_t num_addresses, void* reserved) {
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

grpc_resolver_factory* grpc_new_resolver_factory(const char* scheme,
                                                 void* reserved) {
  GPR_ASSERT(reserved == nullptr);
  grpc_core::ResolverRegistry::Builder::RegisterResolverFactory(
      grpc_core::UniquePtr<grpc_core::ResolverFactory>(
          grpc_core::New<grpc_core::CustomResolverFactory>(scheme)));
  return nullptr;
}

grpc_resolver* grpc_resolver_factory_new_resolver(
    grpc_resolver_factory* factory, void* reserved) {
  GPR_ASSERT(reserved == nullptr);
}

void grpc_resolver_destroy(grpc_resolver* resolver) {}

void grpc_resolver_watch_initialization(grpc_resolver* resolver,
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
