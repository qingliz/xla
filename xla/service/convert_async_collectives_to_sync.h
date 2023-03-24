/* Copyright 2023 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#ifndef XLA_SERVICE_CONVERT_ASYNC_COLLECTIVES_TO_SYNC_H_
#define XLA_SERVICE_CONVERT_ASYNC_COLLECTIVES_TO_SYNC_H_

#include "xla/service/hlo_pass_interface.h"

namespace xla {

// Convert asynchronous collectives to synchronous (after HLO scheduling) if
// there are no compute operations overlapping with them.

class ConvertAsyncCollectivesToSync : public HloModulePass {
 public:
  explicit ConvertAsyncCollectivesToSync(HloPredicate is_nop = {})
      : is_nop_(is_nop) {}
  absl::string_view name() const override {
    return "convert-async-collectives-to-sync";
  }

  using HloPassInterface::Run;
  StatusOr<bool> Run(
      HloModule* module,
      const absl::flat_hash_set<absl::string_view>& execution_threads) override;

 private:
  StatusOr<bool> RunOnComputation(HloComputation* computation);
  HloPredicate is_nop_;
};
}  // namespace xla

#endif  // XLA_SERVICE_CONVERT_ASYNC_COLLECTIVES_TO_SYNC_H_
