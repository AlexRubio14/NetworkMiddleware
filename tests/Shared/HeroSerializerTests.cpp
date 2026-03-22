#include <gtest/gtest.h>
#include "Data/Network/HeroSerializer.h"
#include "Serialization/BitWriter.h"
#include "Serialization/BitReader.h"
#include "Gameplay/HeroDirtyBits.h"
#include "Data/HeroState.h"

using namespace NetworkMiddleware::Shared;
using namespace NetworkMiddleware::Shared::Network;
using namespace NetworkMiddleware::Shared::Data;

// ─── Helpers ─────────────────────────────────────────────────────────────────

static constexpr uint32_t kAllDirty =
    (1u << (uint32_t)HeroDirtyBits::Position)   |
    (1u << (uint32_t)HeroDirtyBits::Health)      |
    (1u << (uint32_t)HeroDirtyBits::MaxHealth)   |
    (1u << (uint32_t)HeroDirtyBits::Mana)        |
    (1u << (uint32_t)HeroDirtyBits::Level)       |
    (1u << (uint32_t)HeroDirtyBits::StateFlags);

static HeroState MakeFullState() {
    HeroState s;
    s.networkID  = 42u;
    s.dirtyMask  = kAllDirty;
    s.x          = 123.5f;
    s.y          = -250.0f;
    s.health     = 500.0f;   // VLE → 2 bytes (>=128)
    s.maxHealth  = 500.0f;   // VLE → 2 bytes
    s.mana       = 100.0f;   // VLE → 1 byte (<128)
    s.level      = 18u;
    s.stateFlags = 0x03u;
    return s;
}

// ─── Bit-count (thesis claim) ─────────────────────────────────────────────────

TEST(HeroSerializer, FullState_BitCount_Is149) {
    // networkID(32) + dirtyMask(32) + pos(16+16) + health VLE(16) +
    // maxHealth VLE(16) + mana VLE(8) + level(5) + stateFlags(8) = 149
    HeroState s = MakeFullState();
    BitWriter w;
    HeroSerializer::Serialize(s, w);
    EXPECT_EQ(w.GetBitCount(), 149u);
}

// ─── Round-trips ─────────────────────────────────────────────────────────────

TEST(HeroSerializer, FullState_RoundTrip) {
    HeroState src = MakeFullState();
    BitWriter w;
    HeroSerializer::Serialize(src, w);

    HeroState dst;
    auto data = w.GetCompressedData();
    BitReader r(data, w.GetBitCount());
    HeroSerializer::Deserialize(dst, r);

    EXPECT_EQ(dst.networkID,  src.networkID);
    EXPECT_EQ(dst.health,     src.health);
    EXPECT_EQ(dst.maxHealth,  src.maxHealth);
    EXPECT_EQ(dst.mana,       src.mana);
    EXPECT_EQ(dst.level,      src.level);
    EXPECT_EQ(dst.stateFlags, src.stateFlags);
}

TEST(HeroSerializer, PositionOnly_RoundTrip) {
    HeroState src;
    src.networkID = 7u;
    src.dirtyMask = (1u << (uint32_t)HeroDirtyBits::Position);
    src.x = 317.25f;
    src.y = -88.0f;

    BitWriter w;
    HeroSerializer::Serialize(src, w);

    // networkID(32) + dirtyMask(32) + x(16) + y(16) = 96 bits
    EXPECT_EQ(w.GetBitCount(), 96u);

    HeroState dst;
    auto data = w.GetCompressedData();
    BitReader r(data, w.GetBitCount());
    HeroSerializer::Deserialize(dst, r);

    EXPECT_EQ(dst.networkID, 7u);

    const float kMaxError = 1000.0f / ((1u << 16) - 1);
    EXPECT_NEAR(dst.x, src.x, kMaxError);
    EXPECT_NEAR(dst.y, src.y, kMaxError);
}

// ─── Position precision ───────────────────────────────────────────────────────

TEST(HeroSerializer, Position_MaxError_Within2cm) {
    // 16-bit quantization over 1000m range → max step ≈ 1.53cm
    const float kMaxError = 1000.0f / ((1u << 16) - 1);
    EXPECT_LT(kMaxError, 0.016f);

    HeroState src;
    src.networkID = 1u;
    src.dirtyMask = (1u << (uint32_t)HeroDirtyBits::Position);
    src.x = 499.9f;
    src.y = -499.9f;

    BitWriter w;
    HeroSerializer::Serialize(src, w);
    HeroState dst;
    auto data = w.GetCompressedData();
    BitReader r(data, w.GetBitCount());
    HeroSerializer::Deserialize(dst, r);

    EXPECT_NEAR(dst.x, src.x, kMaxError);
    EXPECT_NEAR(dst.y, src.y, kMaxError);
}

// ─── Empty dirty mask ─────────────────────────────────────────────────────────

TEST(HeroSerializer, EmptyDirtyMask_OnlyIdentity) {
    HeroState src;
    src.networkID = 99u;
    src.dirtyMask = 0u;

    BitWriter w;
    HeroSerializer::Serialize(src, w);

    // networkID(32) + dirtyMask(32) = 64 bits
    EXPECT_EQ(w.GetBitCount(), 64u);

    HeroState dst;
    auto data = w.GetCompressedData();
    BitReader r(data, w.GetBitCount());
    HeroSerializer::Deserialize(dst, r);
    EXPECT_EQ(dst.networkID, 99u);
}

// ─── Dirty bit isolation ──────────────────────────────────────────────────────

TEST(HeroSerializer, HealthOnly_DoesNotWritePosition) {
    HeroState src;
    src.networkID = 1u;
    src.dirtyMask = (1u << (uint32_t)HeroDirtyBits::Health);
    src.health    = 75.0f; // <128 → 1-byte VLE

    BitWriter w;
    HeroSerializer::Serialize(src, w);

    // networkID(32) + dirtyMask(32) + health VLE(8) = 72 bits
    EXPECT_EQ(w.GetBitCount(), 72u);

    HeroState dst;
    auto data = w.GetCompressedData();
    BitReader r(data, w.GetBitCount());
    HeroSerializer::Deserialize(dst, r);
    EXPECT_EQ(dst.health, src.health);
    EXPECT_EQ(dst.x, 0.0f);
}

TEST(HeroSerializer, StateFlags_RoundTrip) {
    HeroState src;
    src.networkID  = 3u;
    src.dirtyMask  = (1u << (uint32_t)HeroDirtyBits::StateFlags);
    src.stateFlags = 0x07u;

    BitWriter w;
    HeroSerializer::Serialize(src, w);
    HeroState dst;
    auto data = w.GetCompressedData();
    BitReader r(data, w.GetBitCount());
    HeroSerializer::Deserialize(dst, r);

    EXPECT_EQ(dst.stateFlags, 0x07u);
}

TEST(HeroSerializer, Level_RoundTrip) {
    HeroState src;
    src.networkID = 1u;
    src.dirtyMask = (1u << (uint32_t)HeroDirtyBits::Level);
    src.level     = 18u;

    BitWriter w;
    HeroSerializer::Serialize(src, w);
    HeroState dst;
    auto data = w.GetCompressedData();
    BitReader r(data, w.GetBitCount());
    HeroSerializer::Deserialize(dst, r);

    EXPECT_EQ(dst.level, 18u);
}
