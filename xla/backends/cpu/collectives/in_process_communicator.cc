/* Copyright 2023 The OpenXLA Authors.

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

#include "xla/backends/cpu/collectives/in_process_communicator.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "absl/algorithm/container.h"
#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/strings/str_join.h"
#include "absl/types/span.h"
#include "xla/backends/cpu/collectives/cpu_collectives.h"
#include "xla/core/collectives/rank_id.h"
#include "xla/primitive_util.h"
#include "xla/refcounting_hash_map.h"
#include "xla/service/collective_ops_utils.h"
#include "xla/service/global_device_id.h"
#include "xla/service/rendezvous.h"
#include "xla/stream_executor/device_memory.h"
#include "xla/tsl/platform/statusor.h"
#include "xla/util.h"
#include "xla/xla_data.pb.h"

namespace xla::cpu {
namespace {

template <typename Participant>
static bool ByRank(const Participant* a, const Participant* b) {
  return a->rank < b->rank;
}

void FormatGlobalId(std::string* out, const GlobalDeviceId& device) {
  absl::StrAppend(out, device.value());
}

//===----------------------------------------------------------------------===//
// AllGather
//===----------------------------------------------------------------------===//

struct AllGatherParticipant {
  size_t rank;
  se::DeviceMemoryBase src;
  se::DeviceMemoryBase dest;
};

static absl::Status AllGatherOp(
    size_t num_bytes, absl::Span<const AllGatherParticipant*> participants) {
  absl::c_sort(participants, ByRank<AllGatherParticipant>);

  size_t num_participants = participants.size();

  for (size_t i = 0; i < num_participants; ++i) {
    for (size_t j = 0; j < num_participants; ++j) {
      std::byte* dest = static_cast<std::byte*>(participants[i]->dest.opaque());
      size_t offset = j * num_bytes;
      std::memcpy(dest + offset, participants[j]->src.opaque(), num_bytes);
    }
  }

  return absl::OkStatus();
}

//===----------------------------------------------------------------------===//
// AllToAll
//===----------------------------------------------------------------------===//

struct AllToAllParticipant {
  size_t rank;

  std::vector<se::DeviceMemoryBase> src;
  std::vector<se::DeviceMemoryBase> dest;
};

static absl::Status AllToAllOp(
    size_t num_bytes, absl::Span<const AllToAllParticipant*> participants) {
  absl::c_sort(participants, ByRank<AllToAllParticipant>);

  size_t num_participants = participants.size();

  for (size_t i = 0; i < num_participants; ++i) {
    for (size_t j = 0; j < num_participants; ++j) {
      std::memcpy(participants[j]->dest[i].opaque(),
                  participants[i]->src[j].opaque(), num_bytes);
    }
  }

  return absl::OkStatus();
}

//===----------------------------------------------------------------------===//
// CollectivePermute
//===----------------------------------------------------------------------===//

struct CollectivePermuteParticipant {
  size_t rank;
  std::optional<RankId> src_rank;

  se::DeviceMemoryBase src;
  se::DeviceMemoryBase dest;
};

static absl::Status CollectivePermuteOp(
    size_t num_bytes,
    absl::Span<const CollectivePermuteParticipant*> participants) {
  absl::c_sort(participants, ByRank<CollectivePermuteParticipant>);

  for (const CollectivePermuteParticipant* participant : participants) {
    void* dest = participant->dest.opaque();

    if (participant->src_rank) {
      size_t src_rank = participant->src_rank->value();
      std::memcpy(dest, participants.at(src_rank)->src.opaque(), num_bytes);
    } else {
      std::memset(dest, 0, num_bytes);
    }
  }
  return absl::OkStatus();
}

//===----------------------------------------------------------------------===//

struct AllReduceParticipantData : ParticipantData {
  explicit AllReduceParticipantData(const RendezvousKey& rendezvous_key_p,
                                    int rank)
      : ParticipantData(rendezvous_key_p, rank) {}

  int64_t element_count;
  const void* source_data;
  void* destination_data;
  PrimitiveType primitive_type;

  ReductionKind reduction_kind;

  std::string ToString() const override {
    return absl::StrFormat(
        "AllReduceParticipantData{rank=%d, element_count=%d, type=%s, "
        "rendezvous_key=%s}",
        local_rank, element_count, PrimitiveType_Name(primitive_type),
        rendezvous_key.ToString());
  }
};

template <typename T>
T GetInitialValue(ReductionKind reduction_kind) {
  switch (reduction_kind) {
    case ReductionKind::SUM:
      return static_cast<T>(0);
    case ReductionKind::PRODUCT:
      return static_cast<T>(1);
    case ReductionKind::MIN:
      return std::numeric_limits<T>::has_infinity
                 ? std::numeric_limits<T>::infinity()
                 : std::numeric_limits<T>::max();
    case ReductionKind::MAX:
      return std::numeric_limits<T>::has_infinity
                 ? -std::numeric_limits<T>::infinity()
                 : std::numeric_limits<T>::lowest();
  }
}

// We cannot use static_assert(false), because the C++ standard (prior to
// CWG2518) does not allow the statement discarded by a constexpr if to
// be ill-formed for every possible specialization.
// See https://en.cppreference.com/w/cpp/language/if#Constexpr_if
template <ReductionKind>
constexpr bool always_false_v = false;

template <ReductionKind reduction_kind, typename T>
void ReduceHelper(absl::Span<T> acc, absl::Span<T const* const> inputs) {
  // TODO(penporn): make sure this gets vectorized.
  if constexpr (reduction_kind == ReductionKind::SUM) {
    for (size_t j = 0; j < inputs.size(); ++j) {
      for (size_t i = 0; i < acc.size(); ++i) {
        acc[i] += inputs[j][i];
      }
    }
  } else if constexpr (reduction_kind == ReductionKind::PRODUCT) {
    for (size_t j = 0; j < inputs.size(); ++j) {
      for (size_t i = 0; i < acc.size(); ++i) {
        acc[i] *= inputs[j][i];
      }
    }
  } else if constexpr (reduction_kind == ReductionKind::MIN) {
    for (size_t j = 0; j < inputs.size(); ++j) {
      for (size_t i = 0; i < acc.size(); ++i) {
        acc[i] = std::min(acc[i], inputs[j][i]);
      }
    }
  } else if constexpr (reduction_kind == ReductionKind::MAX) {
    for (size_t j = 0; j < inputs.size(); ++j) {
      for (size_t i = 0; i < acc.size(); ++i) {
        acc[i] = std::max(acc[i], inputs[j][i]);
      }
    }
  } else {
    static_assert(always_false_v<reduction_kind>, "Unsupported reduction kind");
  }
}

template <PrimitiveType PT>
absl::Status ReduceScatter(ReductionKind reduction_kind,
                           absl::Span<const void* const> inputs, void* output,
                           int64_t num_elems) {
  using T = primitive_util::NativeTypeOf<PT>;
  T initial_value = GetInitialValue<T>(reduction_kind);

  absl::Span<T> out_chunk =
      absl::MakeSpan(reinterpret_cast<T*>(output), num_elems);
  for (int64_t i = 0; i < num_elems; ++i) {
    out_chunk[i] = initial_value;
  }

  absl::Span<T const* const> input_chunks(
      reinterpret_cast<T const* const*>(inputs.data()), inputs.size());
  switch (reduction_kind) {
    case ReductionKind::SUM:
      ReduceHelper<ReductionKind::SUM, T>(out_chunk, input_chunks);
      break;
    case ReductionKind::PRODUCT:
      ReduceHelper<ReductionKind::PRODUCT, T>(out_chunk, input_chunks);
      break;
    case ReductionKind::MIN:
      if constexpr (!is_complex_v<T>) {
        ReduceHelper<ReductionKind::MIN, T>(out_chunk, input_chunks);
      } else {
        return absl::InvalidArgumentError(
            "Min reductions not supported for complex types");
      }
      break;
    case ReductionKind::MAX:
      if constexpr (!is_complex_v<T>) {
        ReduceHelper<ReductionKind::MAX, T>(out_chunk, input_chunks);
      } else {
        return absl::InvalidArgumentError(
            "Max reductions not supported for complex types");
      }
      break;
  }

  return absl::OkStatus();
}

class CpuAllReduceRendezvous
    : public Rendezvous<AllReduceParticipantData, std::nullptr_t> {
 public:
  explicit CpuAllReduceRendezvous(const RendezvousKey& k)
      : Rendezvous<AllReduceParticipantData, std::nullptr_t>(k) {}

 protected:
  absl::StatusOr<std::nullptr_t> RunCollectiveOp(
      const AllReduceParticipantData& me) override {
    VLOG(3) << me.ToString();
    int64_t world_size = participants_.size();
    // Divide the buffer up into equal(ish) chunks. Rank r computes the r-th
    // chunk of the output.
    int64_t chunk_elems = CeilOfRatio(me.element_count, world_size);

    int64_t start_elem = me.local_rank * chunk_elems;
    int64_t end_elem = std::min(start_elem + chunk_elems, me.element_count);
    chunk_elems = std::max(int64_t{0}, end_elem - start_elem);
    if (chunk_elems == 0) {
      return nullptr;
    }

    auto bytes_per_elem = primitive_util::ByteWidth(me.primitive_type);
    int64_t chunk_offset = start_elem * bytes_per_elem;
    int64_t chunk_bytes = chunk_elems * bytes_per_elem;
    void* reduce_output =
        reinterpret_cast<char*>(me.destination_data) + chunk_offset;

    std::vector<const void*> inputs;
    inputs.reserve(world_size);
    for (const auto& p : participants_) {
      inputs.push_back(reinterpret_cast<const char*>(p->source_data) +
                       chunk_offset);
    }

    if (primitive_util::IsArrayType(me.primitive_type)) {
      TF_RETURN_IF_ERROR(primitive_util::ArrayTypeSwitch<absl::Status>(
          [&](const auto constant_type) {
            return ReduceScatter<constant_type>(me.reduction_kind, inputs,
                                                reduce_output, chunk_elems);
          },
          me.primitive_type));
    } else {
      return absl::UnimplementedError(absl::StrCat(
          "Unexpected datatype: ",
          primitive_util::LowercasePrimitiveTypeName(me.primitive_type)));
    }

    // All-gather the reduced chunks.
    for (const auto& p : participants_) {
      if (p->local_rank != me.local_rank) {
        std::memcpy(reinterpret_cast<char*>(p->destination_data) + chunk_offset,
                    reduce_output, chunk_bytes);
      }
    }
    return nullptr;
  }
};

struct ReduceScatterParticipantData : ParticipantData {
  ReduceScatterParticipantData(const RendezvousKey& rendezvous_key_p, int rank)
      : ParticipantData(rendezvous_key_p, rank) {}

  ReductionKind reduction_kind;
  PrimitiveType element_type;
  const void* source_buffer;
  void* destination_buffer;
  size_t chunk_elems;

  std::string ToString() const override {
    return absl::StrFormat(
        "ReduceScatterParticipantData{rank=%d, "
        "devices=[%s], source_buffer=%p, "
        "destination_buffer=%p, chunk_elems=%d}",
        local_rank,
        absl::StrJoin(rendezvous_key.global_devices, ", ", FormatGlobalId),
        source_buffer, destination_buffer, chunk_elems);
  }
};

class CpuReduceScatterRendezvous
    : public Rendezvous<ReduceScatterParticipantData, std::nullptr_t> {
 public:
  explicit CpuReduceScatterRendezvous(const RendezvousKey& k)
      : Rendezvous<ReduceScatterParticipantData, std::nullptr_t>(k) {}

 protected:
  absl::StatusOr<std::nullptr_t> RunCollectiveOp(
      const ReduceScatterParticipantData& me) override {
    auto bytes_per_elem = primitive_util::ByteWidth(me.element_type);
    int64_t chunk_offset = me.local_rank * me.chunk_elems * bytes_per_elem;

    std::vector<const void*> inputs;
    inputs.reserve(participants_.size());
    for (const auto& p : participants_) {
      inputs.push_back(reinterpret_cast<const char*>(p->source_buffer) +
                       chunk_offset);
    }

    if (primitive_util::IsArrayType(me.element_type)) {
      TF_RETURN_IF_ERROR(primitive_util::ArrayTypeSwitch<absl::Status>(
          [&](const auto constant_type) {
            return ReduceScatter<constant_type>(me.reduction_kind, inputs,
                                                me.destination_buffer,
                                                me.chunk_elems);
          },
          me.element_type));
    } else {
      return absl::UnimplementedError(absl::StrCat(
          "Unexpected datatype: ",
          primitive_util::LowercasePrimitiveTypeName(me.element_type)));
    }
    return nullptr;
  }
};

}  // namespace

struct InProcessCommunicator::State {
  RefcountingHashMap<RendezvousKey, CpuAllReduceRendezvous>
      all_reduce_rendezvous_map;
  RefcountingHashMap<RendezvousKey, CpuReduceScatterRendezvous>
      reduce_scatter_rendezvous_map;
};

InProcessCommunicator::InProcessCommunicator(std::shared_ptr<State> state,
                                             size_t rank, size_t num_ranks)
    : state_(std::move(state)), rank_(rank), num_ranks_(num_ranks) {}

InProcessCommunicator::~InProcessCommunicator() = default;

std::shared_ptr<InProcessCommunicator::State>
InProcessCommunicator::CreateState() {
  return std::make_shared<State>();
}

absl::Status InProcessCommunicator::AllReduce(se::DeviceMemoryBase send_buffer,
                                              se::DeviceMemoryBase recv_buffer,
                                              PrimitiveType dtype, size_t count,
                                              ReductionKind reduction_kind,
                                              const Executor& executor) {
  TF_ASSIGN_OR_RETURN(auto cpu_executor, CpuCollectives::TryCast(&executor));
  const RendezvousKey& key = cpu_executor->rendezvous_key();

  AllReduceParticipantData participant(key, rank_);
  participant.element_count = count;
  participant.primitive_type = dtype;
  participant.source_data = send_buffer.opaque();
  participant.destination_data = recv_buffer.opaque();
  participant.reduction_kind = reduction_kind;

  auto make_cpu_rendezvous = [](const RendezvousKey& k) {
    return std::make_unique<CpuAllReduceRendezvous>(k);
  };

  return CpuAllReduceRendezvous::SubmitParticipant(
             [&] {
               return state_->all_reduce_rendezvous_map.GetOrCreateIfAbsent(
                   key, make_cpu_rendezvous);
             },
             participant)
      .status();
}

absl::Status InProcessCommunicator::CollectivePermute(
    se::DeviceMemoryBase send_buffer, se::DeviceMemoryBase recv_buffer,
    PrimitiveType dtype, size_t count, std::optional<RankId> source_rank,
    absl::Span<const RankId> target_ranks, const Executor& executor) {
  TF_ASSIGN_OR_RETURN(auto cpu_executor, CpuCollectives::TryCast(&executor));
  const RendezvousKey& key = cpu_executor->rendezvous_key();

  std::string name = absl::StrCat("collective permute ", key.ToString());
  CollectivePermuteParticipant partiticipant{rank_, source_rank, send_buffer,
                                             recv_buffer};

  size_t num_bytes = count * primitive_util::ByteWidth(dtype);
  return RendezvousSingle<absl::Status>(
      name, key, partiticipant, key.num_local_participants,
      std::bind(CollectivePermuteOp, num_bytes, std::placeholders::_1));
}

absl::Status InProcessCommunicator::AllToAll(
    absl::Span<const se::DeviceMemoryBase> send_buffers,
    absl::Span<const se::DeviceMemoryBase> recv_buffers, PrimitiveType dtype,
    size_t count, const Executor& executor) {
  TF_ASSIGN_OR_RETURN(auto cpu_executor, CpuCollectives::TryCast(&executor));
  const RendezvousKey& key = cpu_executor->rendezvous_key();

  std::string name = absl::StrCat("all to all ", key.ToString());
  AllToAllParticipant partiticipant{rank_,
                                    {send_buffers.begin(), send_buffers.end()},
                                    {recv_buffers.begin(), recv_buffers.end()}};

  size_t num_bytes = count * primitive_util::ByteWidth(dtype);
  return RendezvousSingle<absl::Status>(
      name, key, partiticipant, key.num_local_participants,
      std::bind(AllToAllOp, num_bytes, std::placeholders::_1));
}

absl::Status InProcessCommunicator::AllGather(se::DeviceMemoryBase send_buffer,
                                              se::DeviceMemoryBase recv_buffer,
                                              PrimitiveType dtype, size_t count,
                                              const Executor& executor) {
  TF_ASSIGN_OR_RETURN(auto cpu_executor, CpuCollectives::TryCast(&executor));
  const RendezvousKey& key = cpu_executor->rendezvous_key();

  std::string name = absl::StrCat("all gather ", key.ToString());
  AllGatherParticipant partiticipant{rank_, send_buffer, recv_buffer};

  size_t num_bytes = count * primitive_util::ByteWidth(dtype);
  return RendezvousSingle<absl::Status>(
      name, key, partiticipant, key.num_local_participants,
      std::bind(AllGatherOp, num_bytes, std::placeholders::_1));
}

absl::Status InProcessCommunicator::ReduceScatter(
    se::DeviceMemoryBase send_buffer, se::DeviceMemoryBase recv_buffer,
    PrimitiveType dtype, size_t count, ReductionKind reduction_kind,
    const Executor& executor) {
  TF_ASSIGN_OR_RETURN(auto cpu_executor, CpuCollectives::TryCast(&executor));
  const RendezvousKey& key = cpu_executor->rendezvous_key();

  ReduceScatterParticipantData participant(key, rank_);
  participant.element_type = dtype;
  participant.reduction_kind = reduction_kind;
  participant.chunk_elems = count;
  participant.source_buffer = send_buffer.opaque();
  participant.destination_buffer = recv_buffer.opaque();
  auto make_cpu_rendezvous = [](const RendezvousKey& k) {
    return std::make_unique<CpuReduceScatterRendezvous>(k);
  };
  return CpuReduceScatterRendezvous::SubmitParticipant(
             [&] {
               return state_->reduce_scatter_rendezvous_map.GetOrCreateIfAbsent(
                   key, make_cpu_rendezvous);
             },
             participant)
      .status();
}

}  // namespace xla::cpu
