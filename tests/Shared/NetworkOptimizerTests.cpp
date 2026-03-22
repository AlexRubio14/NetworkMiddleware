#include <gtest/gtest.h>
#include "Data/Network/NetworkOptimizer.h"

using namespace NetworkMiddleware::Shared;
using namespace NetworkMiddleware::Shared::Network;

// ─── QuantizeFloat ────────────────────────────────────────────────────────────

TEST(NetworkOptimizer, Quantize_MinValue_ReturnsZero) {
    EXPECT_EQ(NetworkOptimizer::QuantizeFloat(-500.0f, -500.0f, 500.0f, 14), 0u);
}

TEST(NetworkOptimizer, Quantize_MaxValue_ReturnsMaxQuantized) {
    const uint32_t expected = (1u << 14) - 1; // 16383
    EXPECT_EQ(NetworkOptimizer::QuantizeFloat(500.0f, -500.0f, 500.0f, 14), expected);
}

TEST(NetworkOptimizer, Quantize_Zero_ReturnsMidpoint) {
    const uint32_t q = NetworkOptimizer::QuantizeFloat(0.0f, -500.0f, 500.0f, 14);
    EXPECT_NEAR(static_cast<int>(q), 8191, 1);
}

TEST(NetworkOptimizer, Quantize_ClampsBelow) {
    EXPECT_EQ(NetworkOptimizer::QuantizeFloat(-999.0f, -500.0f, 500.0f, 14), 0u);
}

TEST(NetworkOptimizer, Quantize_ClampsAbove) {
    const uint32_t expected = (1u << 14) - 1;
    EXPECT_EQ(NetworkOptimizer::QuantizeFloat(999.0f, -500.0f, 500.0f, 14), expected);
}

// ─── DequantizeFloat ──────────────────────────────────────────────────────────

TEST(NetworkOptimizer, Dequantize_Zero_ReturnsMin) {
    EXPECT_FLOAT_EQ(NetworkOptimizer::DequantizeFloat(0u, -500.0f, 500.0f, 14), -500.0f);
}

TEST(NetworkOptimizer, Dequantize_Max_ReturnsMax) {
    const uint32_t maxQ = (1u << 14) - 1;
    EXPECT_FLOAT_EQ(NetworkOptimizer::DequantizeFloat(maxQ, -500.0f, 500.0f, 14), 500.0f);
}

// ─── Round-trip precision ─────────────────────────────────────────────────────

TEST(NetworkOptimizer, RoundTrip_14bits_PrecisionWithinOneStep) {
    // step = 1000 / 16383 ≈ 0.061m
    const float kMaxError = 1000.0f / ((1u << 14) - 1);

    for (float v : {-500.0f, -250.0f, 0.0f, 123.45f, 250.0f, 499.99f, 500.0f}) {
        uint32_t q       = NetworkOptimizer::QuantizeFloat(v, -500.0f, 500.0f, 14);
        float    restored = NetworkOptimizer::DequantizeFloat(q, -500.0f, 500.0f, 14);
        EXPECT_NEAR(restored, v, kMaxError) << "Failed for v=" << v;
    }
}

TEST(NetworkOptimizer, RoundTrip_8bits) {
    const float kMaxError = 100.0f / ((1u << 8) - 1);
    uint32_t q = NetworkOptimizer::QuantizeFloat(42.0f, 0.0f, 100.0f, 8);
    float    r = NetworkOptimizer::DequantizeFloat(q, 0.0f, 100.0f, 8);
    EXPECT_NEAR(r, 42.0f, kMaxError);
}

// ─── WriteVLE / ReadVLE ───────────────────────────────────────────────────────

TEST(NetworkOptimizer, VLE_Zero_SingleByte) {
    BitWriter w;
    NetworkOptimizer::WriteVLE(w, 0u);
    EXPECT_EQ(w.GetBitCount(), 8u);
    auto data = w.GetCompressedData();
    BitReader r(data, w.GetBitCount());
    EXPECT_EQ(NetworkOptimizer::ReadVLE(r), 0u);
}

TEST(NetworkOptimizer, VLE_SmallValue_SingleByte) {
    BitWriter w;
    NetworkOptimizer::WriteVLE(w, 127u);
    EXPECT_EQ(w.GetBitCount(), 8u);
    auto data = w.GetCompressedData();
    BitReader r(data, w.GetBitCount());
    EXPECT_EQ(NetworkOptimizer::ReadVLE(r), 127u);
}

TEST(NetworkOptimizer, VLE_128_TwoBytes) {
    BitWriter w;
    NetworkOptimizer::WriteVLE(w, 128u);
    EXPECT_EQ(w.GetBitCount(), 16u);
    auto data = w.GetCompressedData();
    BitReader r(data, w.GetBitCount());
    EXPECT_EQ(NetworkOptimizer::ReadVLE(r), 128u);
}

TEST(NetworkOptimizer, VLE_LargeValue_RoundTrip) {
    BitWriter w;
    NetworkOptimizer::WriteVLE(w, 16383u);
    auto data = w.GetCompressedData();
    BitReader r(data, w.GetBitCount());
    EXPECT_EQ(NetworkOptimizer::ReadVLE(r), 16383u);
}

TEST(NetworkOptimizer, VLE_TypicalHealth_RoundTrip) {
    BitWriter w;
    NetworkOptimizer::WriteVLE(w, 500u);
    auto data = w.GetCompressedData();
    BitReader r(data, w.GetBitCount());
    EXPECT_EQ(NetworkOptimizer::ReadVLE(r), 500u);
}
