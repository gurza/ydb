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

#include <grpc/support/port_platform.h>

#include <string.h>

#include <util/generic/string.h>
#include <util/string/cast.h>

#include "y_absl/strings/str_cat.h"

#include <grpc/support/alloc.h>
#include <grpc/support/log.h>
#include <grpc/support/string_util.h>

#include "src/core/lib/channel/channel_stack.h"
#include "src/core/lib/channel/promise_based_filter.h"
#include "src/core/lib/gpr/string.h"
#include "src/core/lib/iomgr/error.h"
#include "src/core/lib/profiling/timers.h"
#include "src/core/lib/promise/promise.h"
#include "src/core/lib/promise/try_seq.h"
#include "src/core/lib/security/context/security_context.h"
#include "src/core/lib/security/credentials/credentials.h"
#include "src/core/lib/security/security_connector/security_connector.h"
#include "src/core/lib/security/transport/auth_filters.h"
#include "src/core/lib/slice/slice_internal.h"
#include "src/core/lib/slice/slice_string_helpers.h"
#include "src/core/lib/surface/call.h"
#include "src/core/lib/transport/transport.h"

#define MAX_CREDENTIALS_METADATA_COUNT 4

void grpc_auth_metadata_context_copy(grpc_auth_metadata_context* from,
                                     grpc_auth_metadata_context* to) {
  grpc_auth_metadata_context_reset(to);
  to->channel_auth_context = from->channel_auth_context;
  if (to->channel_auth_context != nullptr) {
    const_cast<grpc_auth_context*>(to->channel_auth_context)
        ->Ref(DEBUG_LOCATION, "grpc_auth_metadata_context_copy")
        .release();
  }
  to->service_url = gpr_strdup(from->service_url);
  to->method_name = gpr_strdup(from->method_name);
}

void grpc_auth_metadata_context_reset(
    grpc_auth_metadata_context* auth_md_context) {
  if (auth_md_context->service_url != nullptr) {
    gpr_free(const_cast<char*>(auth_md_context->service_url));
    auth_md_context->service_url = nullptr;
  }
  if (auth_md_context->method_name != nullptr) {
    gpr_free(const_cast<char*>(auth_md_context->method_name));
    auth_md_context->method_name = nullptr;
  }
  if (auth_md_context->channel_auth_context != nullptr) {
    const_cast<grpc_auth_context*>(auth_md_context->channel_auth_context)
        ->Unref(DEBUG_LOCATION, "grpc_auth_metadata_context");
    auth_md_context->channel_auth_context = nullptr;
  }
}

static grpc_security_level convert_security_level_string_to_enum(
    const char* security_level) {
  if (strcmp(security_level, "TSI_INTEGRITY_ONLY") == 0) {
    return GRPC_INTEGRITY_ONLY;
  } else if (strcmp(security_level, "TSI_PRIVACY_AND_INTEGRITY") == 0) {
    return GRPC_PRIVACY_AND_INTEGRITY;
  }
  return GRPC_SECURITY_NONE;
}

bool grpc_check_security_level(grpc_security_level channel_level,
                               grpc_security_level call_cred_level) {
  return static_cast<int>(channel_level) >= static_cast<int>(call_cred_level);
}

namespace grpc_core {

ClientAuthFilter::ClientAuthFilter(
    RefCountedPtr<grpc_channel_security_connector> security_connector,
    RefCountedPtr<grpc_auth_context> auth_context)
    : args_{std::move(security_connector), std::move(auth_context)} {}

ArenaPromise<y_absl::StatusOr<ClientInitialMetadata>>
ClientAuthFilter::GetCallCredsMetadata(ClientInitialMetadata initial_metadata) {
  auto* ctx = static_cast<grpc_client_security_context*>(
      GetContext<grpc_call_context_element>()[GRPC_CONTEXT_SECURITY].value);
  grpc_call_credentials* channel_call_creds =
      args_.security_connector->mutable_request_metadata_creds();
  const bool call_creds_has_md = (ctx != nullptr) && (ctx->creds != nullptr);

  if (channel_call_creds == nullptr && !call_creds_has_md) {
    /* Skip sending metadata altogether. */
    return Immediate(
        y_absl::StatusOr<ClientInitialMetadata>(std::move(initial_metadata)));
  }

  RefCountedPtr<grpc_call_credentials> creds;
  if (channel_call_creds != nullptr && call_creds_has_md) {
    creds = RefCountedPtr<grpc_call_credentials>(
        grpc_composite_call_credentials_create(channel_call_creds,
                                               ctx->creds.get(), nullptr));
    if (creds == nullptr) {
      return Immediate(y_absl::UnauthenticatedError(
          "Incompatible credentials set on channel and call."));
    }
  } else if (call_creds_has_md) {
    creds = ctx->creds->Ref();
  } else {
    creds = channel_call_creds->Ref();
  }

  /* Check security level of call credential and channel, and do not send
   * metadata if the check fails. */
  grpc_auth_property_iterator it = grpc_auth_context_find_properties_by_name(
      args_.auth_context.get(), GRPC_TRANSPORT_SECURITY_LEVEL_PROPERTY_NAME);
  const grpc_auth_property* prop = grpc_auth_property_iterator_next(&it);
  if (prop == nullptr) {
    return Immediate(
        y_absl::UnauthenticatedError("Established channel does not have an auth "
                                   "property representing a security level."));
  }
  const grpc_security_level call_cred_security_level =
      creds->min_security_level();
  const bool is_security_level_ok = grpc_check_security_level(
      convert_security_level_string_to_enum(prop->value),
      call_cred_security_level);
  if (!is_security_level_ok) {
    return Immediate(y_absl::UnauthenticatedError(
        "Established channel does not have a sufficient security level to "
        "transfer call credential."));
  }

  return creds->GetRequestMetadata(std::move(initial_metadata), &args_);
}

ArenaPromise<TrailingMetadata> ClientAuthFilter::MakeCallPromise(
    ClientInitialMetadata initial_metadata,
    NextPromiseFactory next_promise_factory) {
  auto* legacy_ctx = GetContext<grpc_call_context_element>();
  if (legacy_ctx[GRPC_CONTEXT_SECURITY].value == nullptr) {
    legacy_ctx[GRPC_CONTEXT_SECURITY].value =
        grpc_client_security_context_create(GetContext<Arena>(),
                                            /*creds=*/nullptr);
    legacy_ctx[GRPC_CONTEXT_SECURITY].destroy =
        grpc_client_security_context_destroy;
  }
  static_cast<grpc_client_security_context*>(
      legacy_ctx[GRPC_CONTEXT_SECURITY].value)
      ->auth_context = args_.auth_context;

  auto* host = initial_metadata->get_pointer(HttpAuthorityMetadata());
  if (host == nullptr) {
    return next_promise_factory(std::move(initial_metadata));
  }
  return TrySeq(args_.security_connector->CheckCallHost(
                    host->as_string_view(), args_.auth_context.get()),
                GetCallCredsMetadata(std::move(initial_metadata)),
                next_promise_factory);
}

y_absl::StatusOr<ClientAuthFilter> ClientAuthFilter::Create(
    const grpc_channel_args* args, ChannelFilter::Args) {
  grpc_security_connector* sc = grpc_security_connector_find_in_args(args);
  if (sc == nullptr) {
    return y_absl::InvalidArgumentError(
        "Security connector missing from client auth filter args");
  }
  grpc_auth_context* auth_context = grpc_find_auth_context_in_args(args);
  if (auth_context == nullptr) {
    return y_absl::InvalidArgumentError(
        "Auth context missing from client auth filter args");
  }

  return ClientAuthFilter(
      static_cast<grpc_channel_security_connector*>(sc)->Ref(),
      auth_context->Ref());
}

}  // namespace grpc_core

const grpc_channel_filter grpc_client_auth_filter =
    grpc_core::MakePromiseBasedFilter<grpc_core::ClientAuthFilter,
                                      grpc_core::FilterEndpoint::kClient>(
        "client-auth-filter");
