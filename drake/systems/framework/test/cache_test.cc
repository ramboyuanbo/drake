#include "drake/systems/framework/cache.h"

#include <memory>
#include <stdexcept>

#include "gtest/gtest.h"

#include "drake/systems/framework/value.h"

namespace drake {
namespace systems {
namespace {

class CacheTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ticket0_ = cache_.MakeCacheTicket({});
    ticket1_ = cache_.MakeCacheTicket({ticket0_});
    ticket2_ = cache_.MakeCacheTicket({ticket0_, ticket1_});

    cache_.Set(ticket0_, PackValue(0));
    cache_.Set(ticket1_, PackValue(1));
    cache_.Set(ticket2_, PackValue(2));
  }

  std::unique_ptr<AbstractValue> PackValue(int value) {
    return std::unique_ptr<AbstractValue>(new Value<int>(value));
  }

  int UnpackValue(const AbstractValue* value) {
    return dynamic_cast<const Value<int>*>(value)->get_value();
  }

  Cache cache_;
  CacheTicket ticket0_;
  CacheTicket ticket1_;
  CacheTicket ticket2_;
};

TEST_F(CacheTest, SetReturnsValue) {
  CacheTicket ticket = cache_.MakeCacheTicket({});
  AbstractValue* value = cache_.Set(ticket, PackValue(42));
  EXPECT_EQ(42, UnpackValue(value));
}

TEST_F(CacheTest, GetReturnsValue) {
  CacheTicket ticket = cache_.MakeCacheTicket({});
  cache_.Set(ticket, PackValue(42));
  AbstractValue* value = cache_.Get(ticket);
  EXPECT_EQ(42, UnpackValue(value));
}

TEST_F(CacheTest, SwapReturnsAndSetsValue) {
  CacheTicket ticket = cache_.MakeCacheTicket({});
  cache_.Set(ticket, PackValue(42));
  std::unique_ptr<AbstractValue> value = cache_.Swap(ticket, PackValue(43));
  EXPECT_EQ(42, UnpackValue(value.get()));
  EXPECT_EQ(43, UnpackValue(cache_.Get(ticket)));
}

TEST_F(CacheTest, InvalidationIsRecursive) {
  cache_.Invalidate(ticket1_);
  EXPECT_EQ(0, UnpackValue((cache_.Get(ticket0_))));
  EXPECT_EQ(nullptr, cache_.Get(ticket1_));
  EXPECT_EQ(nullptr, cache_.Get(ticket2_));
}

// Tests that a pointer to a cached value remains valid even after it is
// invalidated. Only advanced, careful users should ever rely on this behavior!
TEST_F(CacheTest, InvalidationIsNotDeletion) {
  AbstractValue* value = cache_.Get(ticket1_);
  cache_.Invalidate(ticket1_);
  EXPECT_EQ(nullptr, cache_.Get(ticket1_));
  EXPECT_EQ(1, UnpackValue(value));
}

TEST_F(CacheTest, InvalidationDoesNotStopOnNullptr) {
  cache_.Invalidate(ticket1_);
  cache_.Set(ticket2_, PackValue(76));
  cache_.Invalidate(ticket1_);
  EXPECT_EQ(nullptr, cache_.Get(ticket2_));
}

TEST_F(CacheTest, Clone) {
  std::unique_ptr<Cache> clone = cache_.Clone();
  // The clone should have the same values.
  EXPECT_EQ(0, UnpackValue((clone->Get(ticket0_))));
  EXPECT_EQ(1, UnpackValue((clone->Get(ticket1_))));
  EXPECT_EQ(2, UnpackValue((clone->Get(ticket2_))));

  // The clone should have the same invalidation topology.
  clone->Invalidate(ticket0_);
  EXPECT_EQ(nullptr, clone->Get(ticket0_));
  EXPECT_EQ(nullptr, clone->Get(ticket1_));
  EXPECT_EQ(nullptr, clone->Get(ticket2_));

  // Changes to the clone should not affect the original.
  EXPECT_EQ(0, UnpackValue((cache_.Get(ticket0_))));
  EXPECT_EQ(1, UnpackValue((cache_.Get(ticket1_))));
  EXPECT_EQ(2, UnpackValue((cache_.Get(ticket2_))));
}

}  // namespace
}  // namespace systems
}  // namespace drake
