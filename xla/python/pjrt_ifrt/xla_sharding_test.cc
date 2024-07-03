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

#include "xla/python/pjrt_ifrt/xla_sharding.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <utility>

#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include "absl/types/span.h"
#include "xla/hlo/ir/hlo_sharding.h"
#include "xla/hlo/ir/tile_assignment.h"
#include "xla/python/ifrt/index.h"
#include "xla/python/ifrt/index_domain.h"
#include "xla/python/ifrt/memory.h"
#include "xla/python/ifrt/shape.h"
#include "xla/python/ifrt/sharding.h"
#include "xla/python/ifrt/sharding_test_util.h"
#include "xla/xla_data.pb.h"
#include "tsl/platform/errors.h"
#include "tsl/platform/status_matchers.h"
#include "tsl/platform/statusor.h"

namespace xla {
namespace ifrt {
namespace {

using ::testing::ElementsAre;
using ::testing::ElementsAreArray;
using ::testing::HasSubstr;
using ::testing::SizeIs;
using ::tsl::testing::IsOkAndHolds;
using ::tsl::testing::StatusIs;

class HloShardingTest : public test_util::ShardingTest {};

TEST_P(HloShardingTest, IsFullyReplicated) {
  auto device_list = GetDevices({0, 1, 2, 3, 4, 5});
  {
    // Fully replicated.
    auto xla_hlo_sharding = xla::HloSharding::Replicate();
    std::shared_ptr<const HloSharding> sharding =
        HloSharding::Create(device_list, MemoryKind(), xla_hlo_sharding);
    EXPECT_TRUE(sharding->IsFullyReplicated());
  }
  {
    // Not fully replicated.
    auto xla_hlo_sharding = xla::HloSharding::IotaTile({1, 6});
    std::shared_ptr<const HloSharding> sharding =
        HloSharding::Create(device_list, MemoryKind(), xla_hlo_sharding);
    EXPECT_FALSE(sharding->IsFullyReplicated());
  }
  {
    // Not fully replicated.
    auto xla_hlo_sharding = xla::HloSharding::IotaTile({2, 3});
    std::shared_ptr<const HloSharding> sharding =
        HloSharding::Create(device_list, MemoryKind(), xla_hlo_sharding);
    EXPECT_FALSE(sharding->IsFullyReplicated());
  }
}

TEST_P(HloShardingTest, GetShardShape) {
  auto device_list = GetDevices({0, 1, 2, 3, 4, 5});
  auto xla_hlo_sharding = xla::HloSharding::IotaTile({2, 3});
  std::shared_ptr<const HloSharding> sharding =
      HloSharding::Create(device_list, MemoryKind(), xla_hlo_sharding);
  EXPECT_THAT(sharding->GetShardShape(Shape({6, 6})),
              IsOkAndHolds(Shape({3, 2})));
  EXPECT_THAT(sharding->GetShardShape(Shape({6, 6, 6})),
              StatusIs(tsl::error::INVALID_ARGUMENT,
                       HasSubstr("Numbers of dimensions don't match. From "
                                 "Shape 3 vs from HloSharding 2")));
}

TEST_P(HloShardingTest, HasSamePartitioning) {
  auto device_list0 = GetDevices({0, 1, 2, 3, 4, 5});
  auto xla_hlo_sharding0 = xla::HloSharding::IotaTile({2, 3});
  std::shared_ptr<const HloSharding> sharding0 =
      HloSharding::Create(device_list0, MemoryKind(), xla_hlo_sharding0);

  EXPECT_TRUE(sharding0->HasSamePartitioning(*sharding0));
  {
    auto device_list1 = GetDevices({3, 4, 5, 0, 1, 2});
    auto xla_hlo_sharding1 = xla::HloSharding::IotaTile({2, 3});
    std::shared_ptr<const HloSharding> sharding1 =
        HloSharding::Create(device_list1, MemoryKind(), xla_hlo_sharding1);
    EXPECT_TRUE(sharding0->HasSamePartitioning(*sharding1));
  }
  // Different number of shards.
  {
    auto device_list1 = GetDevices({3, 4, 5});
    auto xla_hlo_sharding1 = xla::HloSharding::IotaTile({3, 1});
    std::shared_ptr<const HloSharding> sharding1 =
        HloSharding::Create(device_list1, MemoryKind(), xla_hlo_sharding1);
    EXPECT_FALSE(sharding0->HasSamePartitioning(*sharding1));
  }
  // Different HloSharding.
  {
    auto device_list1 = GetDevices({3, 4, 5, 0, 1, 2});
    auto xla_hlo_sharding1 = xla::HloSharding::IotaTile({3, 2});
    std::shared_ptr<const HloSharding> sharding1 =
        HloSharding::Create(device_list1, MemoryKind(), xla_hlo_sharding1);
    EXPECT_FALSE(sharding0->HasSamePartitioning(*sharding1));
  }
}

TEST_P(HloShardingTest, WithDeviceAssignment) {
  auto device_list0 = GetDevices({0, 1, 2, 3, 4, 5});
  auto xla_hlo_sharding0 = xla::HloSharding::IotaTile({2, 3});
  std::shared_ptr<const HloSharding> sharding0 =
      HloSharding::Create(device_list0, MemoryKind(), xla_hlo_sharding0);
  {
    auto device_list1 = GetDevices({3, 4, 5, 0, 1, 2});
    auto xla_hlo_sharding1 = xla::HloSharding::IotaTile({2, 3});
    std::shared_ptr<const HloSharding> sharding1 =
        HloSharding::Create(device_list1, MemoryKind(), xla_hlo_sharding1);
    TF_ASSERT_OK_AND_ASSIGN(
        auto new_sharding,
        sharding0->WithDeviceAssignment(device_list1,
                                        /*memory_kind=*/std::nullopt));
    EXPECT_EQ(*new_sharding, *sharding1);
  }
  {
    auto device_list1 = GetDevices({0, 1, 2});
    EXPECT_THAT(
        sharding0->WithDeviceAssignment(device_list1,
                                        /*memory_kind=*/std::nullopt),
        StatusIs(tsl::error::INVALID_ARGUMENT,
                 HasSubstr("HloSharding should have the same number of "
                           "devices as the current sharding, but was asked to "
                           "have 3 devices")));
  }
}

TEST_P(HloShardingTest, IndexDomainsWithReplication) {
  auto device_list = GetDevices({0, 1});
  // Fully replicated.
  auto xla_hlo_sharding = xla::HloSharding::Replicate();
  std::shared_ptr<const HloSharding> sharding =
      HloSharding::Create(device_list, MemoryKind(), xla_hlo_sharding);

  Shape shape({10, 20});
  TF_ASSERT_OK_AND_ASSIGN(auto index_domains, sharding->IndexDomains(shape));

  EXPECT_THAT(index_domains,
              ElementsAre(IndexDomain(shape), IndexDomain(shape)));
  EXPECT_THAT(
      index_domains,
      ElementsAreArray(TEST_HloShardingIndexDomainsSlowPath(*sharding, shape)));
}

TEST_P(HloShardingTest, DisassembleWithReplication) {
  auto device_list = GetDevices({0, 1});
  // Fully replicated.
  auto xla_hlo_sharding = xla::HloSharding::Replicate();
  std::shared_ptr<const HloSharding> sharding =
      HloSharding::Create(device_list, MemoryKind(), xla_hlo_sharding);

  Shape shape({10, 20});
  TF_ASSERT_OK_AND_ASSIGN(auto disassembled, sharding->Disassemble(shape));

  ASSERT_THAT(disassembled, SizeIs(2));
  for (int i = 0; i < 2; ++i) {
    const auto& [shape, sharding] = disassembled[i];
    EXPECT_EQ(shape, Shape({10, 20}));
    EXPECT_EQ(*sharding,
              *SingleDeviceSharding::Create(device_list[i], MemoryKind()));
  }
}

TEST_P(HloShardingTest, IndexDomainsWithTile) {
  auto device_list = GetDevices({0, 1});
  // 2-way sharded along axis 0, 1-way sharded along axis 1.
  auto xla_hlo_sharding = xla::HloSharding::Tile(xla::TileAssignment({2, 1}));
  std::shared_ptr<const HloSharding> sharding =
      HloSharding::Create(device_list, MemoryKind(), xla_hlo_sharding);

  Shape shape({10, 20});
  TF_ASSERT_OK_AND_ASSIGN(auto index_domains, sharding->IndexDomains(shape));

  EXPECT_THAT(index_domains,
              ElementsAre(IndexDomain(Index({0, 0}), Shape({5, 20})),
                          IndexDomain(Index({5, 0}), Shape({5, 20}))));
  EXPECT_THAT(
      index_domains,
      ElementsAreArray(TEST_HloShardingIndexDomainsSlowPath(*sharding, shape)));
}

TEST_P(HloShardingTest, DisassembleWithTile) {
  auto device_list = GetDevices({0, 1});
  // 2-way sharded along axis 0, 1-way sharded along axis 1.
  auto xla_hlo_sharding = xla::HloSharding::Tile(xla::TileAssignment({2, 1}));
  std::shared_ptr<const HloSharding> sharding =
      HloSharding::Create(device_list, MemoryKind(), xla_hlo_sharding);

  Shape shape({10, 20});
  TF_ASSERT_OK_AND_ASSIGN(auto disassembled, sharding->Disassemble(shape));

  ASSERT_THAT(disassembled, SizeIs(2));
  for (int i = 0; i < 2; ++i) {
    const auto& [shape, sharding] = disassembled[i];
    EXPECT_EQ(shape, Shape({5, 20}));
    EXPECT_EQ(*sharding,
              *SingleDeviceSharding::Create(device_list[i], MemoryKind()));
  }
}

TEST_P(HloShardingTest, IndexDomainsWithUnevenTile) {
  auto device_list = GetDevices({0, 1});
  // 2-way sharded along axis 0, 1-way sharded along axis 1.
  auto xla_hlo_sharding = xla::HloSharding::Tile(xla::TileAssignment({2, 1}));
  std::shared_ptr<const HloSharding> sharding =
      HloSharding::Create(device_list, MemoryKind(), xla_hlo_sharding);

  Shape shape({11, 20});
  TF_ASSERT_OK_AND_ASSIGN(auto index_domains, sharding->IndexDomains(shape));

  EXPECT_THAT(index_domains,
              ElementsAre(IndexDomain(Index({0, 0}), Shape({6, 20})),
                          IndexDomain(Index({6, 0}), Shape({5, 20}))));
  EXPECT_THAT(
      index_domains,
      ElementsAreArray(TEST_HloShardingIndexDomainsSlowPath(*sharding, shape)));
}

TEST_P(HloShardingTest, DisassembleWithUnevenTile) {
  auto device_list = GetDevices({0, 1});
  // 2-way sharded along axis 0, 1-way sharded along axis 1.
  auto xla_hlo_sharding = xla::HloSharding::Tile(xla::TileAssignment({2, 1}));
  std::shared_ptr<const HloSharding> sharding =
      HloSharding::Create(device_list, MemoryKind(), xla_hlo_sharding);

  Shape shape({11, 20});
  TF_ASSERT_OK_AND_ASSIGN(auto disassembled, sharding->Disassemble(shape));

  ASSERT_THAT(disassembled, SizeIs(2));
  for (int i = 0; i < 2; ++i) {
    const auto& [shape, sharding] = disassembled[i];
    if (i == 0) {
      EXPECT_EQ(shape, Shape({6, 20}));
    } else {
      EXPECT_EQ(shape, Shape({5, 20}));
    }
    EXPECT_EQ(*sharding,
              *SingleDeviceSharding::Create(device_list[i], MemoryKind()));
  }
}

TEST_P(HloShardingTest, IndexDomainsWithPartialTile) {
  auto device_list = GetDevices({0, 1, 2, 3, 4, 5});
  // 2-way sharded along axis 0, 1-way sharded along axis 1, each shard
  // replicated by 3 times.
  auto xla_hlo_sharding =
      xla::HloSharding::PartialTile(xla::TileAssignment({2, 1, 3}));
  std::shared_ptr<const HloSharding> sharding =
      HloSharding::Create(device_list, MemoryKind(), xla_hlo_sharding);

  Shape shape({10, 20});
  TF_ASSERT_OK_AND_ASSIGN(auto index_domains, sharding->IndexDomains(shape));

  EXPECT_THAT(index_domains,
              ElementsAre(IndexDomain(Index({0, 0}), Shape({5, 20})),
                          IndexDomain(Index({0, 0}), Shape({5, 20})),
                          IndexDomain(Index({0, 0}), Shape({5, 20})),
                          IndexDomain(Index({5, 0}), Shape({5, 20})),
                          IndexDomain(Index({5, 0}), Shape({5, 20})),
                          IndexDomain(Index({5, 0}), Shape({5, 20}))));
  EXPECT_THAT(
      index_domains,
      ElementsAreArray(TEST_HloShardingIndexDomainsSlowPath(*sharding, shape)));
}

TEST_P(HloShardingTest, DisassembleWithPartialTile) {
  auto device_list = GetDevices({0, 1, 2, 3, 4, 5});
  // 2-way sharded along axis 0, 1-way sharded along axis 1, each shard
  // replicated by 3 times.
  auto xla_hlo_sharding =
      xla::HloSharding::PartialTile(xla::TileAssignment({2, 1, 3}));
  std::shared_ptr<const HloSharding> sharding =
      HloSharding::Create(device_list, MemoryKind(), xla_hlo_sharding);

  Shape shape({10, 20});
  TF_ASSERT_OK_AND_ASSIGN(auto disassembled, sharding->Disassemble(shape));

  ASSERT_THAT(disassembled, SizeIs(6));
  for (int i = 0; i < 6; ++i) {
    const auto& [shape, sharding] = disassembled[i];
    EXPECT_EQ(shape, Shape({5, 20}));
    EXPECT_EQ(*sharding,
              *SingleDeviceSharding::Create(device_list[i], MemoryKind()));
  }
}

TEST_P(HloShardingTest, IndexDomainsWithSubgroupReplicated) {
  auto device_list = GetDevices({0, 1, 2, 3, 4, 5});
  // 2-way sharded along axis 0, 1-way sharded along axis 1, each shard
  // replicated by 3 times.
  auto xla_hlo_sharding = xla::HloSharding::Subgroup(
      xla::TileAssignment({2, 1, 3}), {xla::OpSharding::REPLICATED});
  std::shared_ptr<const HloSharding> sharding =
      HloSharding::Create(device_list, MemoryKind(), xla_hlo_sharding);

  Shape shape({10, 20});
  TF_ASSERT_OK_AND_ASSIGN(auto index_domains, sharding->IndexDomains(shape));

  EXPECT_THAT(index_domains,
              ElementsAre(IndexDomain(Index({0, 0}), Shape({5, 20})),
                          IndexDomain(Index({0, 0}), Shape({5, 20})),
                          IndexDomain(Index({0, 0}), Shape({5, 20})),
                          IndexDomain(Index({5, 0}), Shape({5, 20})),
                          IndexDomain(Index({5, 0}), Shape({5, 20})),
                          IndexDomain(Index({5, 0}), Shape({5, 20}))));
  EXPECT_THAT(
      index_domains,
      ElementsAreArray(TEST_HloShardingIndexDomainsSlowPath(*sharding, shape)));
}

TEST_P(HloShardingTest, DisassembleWithSubgroupReplicated) {
  auto device_list = GetDevices({0, 1, 2, 3, 4, 5});
  // 2-way sharded along axis 0, 1-way sharded along axis 1, each shard
  // replicated by 3 times.
  auto xla_hlo_sharding = xla::HloSharding::Subgroup(
      xla::TileAssignment({2, 1, 3}), {xla::OpSharding::REPLICATED});
  std::shared_ptr<const HloSharding> sharding =
      HloSharding::Create(device_list, MemoryKind(), xla_hlo_sharding);

  Shape shape({10, 20});
  TF_ASSERT_OK_AND_ASSIGN(auto disassembled, sharding->Disassemble(shape));

  ASSERT_THAT(disassembled, SizeIs(6));
  for (int i = 0; i < 6; ++i) {
    const auto& [shape, sharding] = disassembled[i];
    EXPECT_EQ(shape, Shape({5, 20}));
    EXPECT_EQ(*sharding,
              *SingleDeviceSharding::Create(device_list[i], MemoryKind()));
  }
}

TEST_P(HloShardingTest, IndexDomainsWithSubgroupMaximalSlowPath) {
  auto device_list = GetDevices({0, 1, 2, 3, 4, 5});
  // 2-way sharded along axis 0, 1-way sharded along axis 1, each shard
  // maximal-replicated by 3 times, device#0 in each replication is maximal.
  auto xla_hlo_sharding = xla::HloSharding::Subgroup(
      xla::TileAssignment({2, 1, 3}), {xla::OpSharding::MAXIMAL});
  std::shared_ptr<const HloSharding> sharding =
      HloSharding::Create(device_list, MemoryKind(), xla_hlo_sharding);

  Shape shape({10, 20});
  TF_ASSERT_OK_AND_ASSIGN(auto index_domains, sharding->IndexDomains(shape));

  EXPECT_THAT(index_domains,
              ElementsAre(IndexDomain(Index({0, 0}), Shape({5, 20})),
                          IndexDomain(Index({0, 0}), Shape({5, 20})),
                          IndexDomain(Index({0, 0}), Shape({5, 20})),
                          IndexDomain(Index({5, 0}), Shape({5, 20})),
                          IndexDomain(Index({5, 0}), Shape({5, 20})),
                          IndexDomain(Index({5, 0}), Shape({5, 20}))));
  EXPECT_THAT(
      index_domains,
      ElementsAreArray(TEST_HloShardingIndexDomainsSlowPath(*sharding, shape)));
}

TEST_P(HloShardingTest, DisassembleWithSubgroupMaximalSlowPath) {
  auto device_list = GetDevices({0, 1, 2, 3, 4, 5});
  // 2-way sharded along axis 0, 1-way sharded along axis 1, each shard
  // maximal-replicated by 3 times, device#0 in each replication is maximal.
  auto xla_hlo_sharding = xla::HloSharding::Subgroup(
      xla::TileAssignment({2, 1, 3}), {xla::OpSharding::MAXIMAL});
  std::shared_ptr<const HloSharding> sharding =
      HloSharding::Create(device_list, MemoryKind(), xla_hlo_sharding);

  Shape shape({10, 20});
  TF_ASSERT_OK_AND_ASSIGN(auto disassembled, sharding->Disassemble(shape));

  ASSERT_THAT(disassembled, SizeIs(6));
  for (int i = 0; i < 6; ++i) {
    const auto& [shape, sharding] = disassembled[i];
    EXPECT_EQ(shape, Shape({5, 20}));
    EXPECT_EQ(*sharding,
              *SingleDeviceSharding::Create(device_list[i], MemoryKind()));
  }
}

TEST_P(HloShardingTest, DisassembleFailsWithInvalidDeviceCount) {
  auto device_list = GetDevices({0});
  // 2-way sharded along axis 0, 1-way sharded along axis 1.
  auto xla_hlo_sharding = xla::HloSharding::Tile(xla::TileAssignment({2, 1}));
  std::shared_ptr<const HloSharding> sharding =
      HloSharding::Create(device_list, MemoryKind(), xla_hlo_sharding);

  Shape shape({10, 20});
  EXPECT_THAT(sharding->Disassemble(shape),
              StatusIs(tsl::error::INVALID_ARGUMENT,
                       HasSubstr("sharding's tile_assignment_devices and "
                                 "device count does not match: 2 vs. 1")));
}

TEST_P(HloShardingTest, DisassembleFailsWithMismatchingShapeDimsSize) {
  auto device_list = GetDevices({0, 1});
  // 2-way sharded along axis 0, 1-way sharded along axis 1.
  auto xla_hlo_sharding = xla::HloSharding::Tile(xla::TileAssignment({2, 1}));
  std::shared_ptr<const HloSharding> sharding =
      HloSharding::Create(device_list, MemoryKind(), xla_hlo_sharding);

  Shape shape({10});
  EXPECT_THAT(
      sharding->Disassemble(shape),
      StatusIs(
          tsl::error::INVALID_ARGUMENT,
          HasSubstr("shape must have 2 dimensions, but has 1 dimensions")));
}

TEST_P(HloShardingTest, DisassembleFailsWithDynamicShape) {
  auto device_list = GetDevices({0, 1});
  auto xla_hlo_sharding = xla::HloSharding::Tile(xla::TileAssignment({2}));
  std::shared_ptr<const HloSharding> sharding =
      HloSharding::Create(device_list, MemoryKind(), xla_hlo_sharding);

  TF_ASSERT_OK_AND_ASSIGN(
      DynamicShape dynamic_shape,
      DynamicShape::Create(Shape({10}), BoundedDynamicShapeTag({true})));
  EXPECT_THAT(sharding->Disassemble(dynamic_shape),
              StatusIs(tsl::error::INVALID_ARGUMENT,
                       HasSubstr("can only disassemble static shape")));
}

INSTANTIATE_TEST_SUITE_P(NumDevices, HloShardingTest,
                         testing::Values(test_util::ShardingTestParam{
                             .num_devices = 6, .num_addressable_devices = 4}));

}  // namespace
}  // namespace ifrt
}  // namespace xla
