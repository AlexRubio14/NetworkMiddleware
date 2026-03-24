// tests/Brain/KalmanTests.cpp — P-5.2 KalmanPredictor unit tests
//
// Test strategy (from validated design handoff):
//
//   Test_Kalman_Linear_Track
//     Warm-up over 20 ticks of straight-line motion.  The filter should
//     lock onto the velocity so that the first prediction tick still points
//     in the original direction (dot product > 0.9).
//
//   Test_Kalman_PacketLoss_Recovery
//     After warm-up, simulate 3 consecutive prediction ticks (no real input).
//     The cumulative position error vs. ideal straight-line target must be
//     < 0.5 units (50 % of one movement step at kMoveSpeed=100, dt=0.01).
//
//   Test_Kalman_DirectionChange_Tracking
//     After 20 warm-up ticks forward, reverse direction for 10 ticks with
//     real input.  The synthetic prediction should point into the new
//     hemisphere (dot product with the reversed direction > 0).

#include <gtest/gtest.h>
#include "../../Brain/KalmanPredictor.h"

using namespace NetworkMiddleware::Brain;

// Movement per tick at kMoveSpeed=100, dt=0.01: dir * 100 * 0.01 = dir * 1.0
static constexpr float kStepPerTick = 1.0f;

// ─────────────────────────────────────────────────────────────────────────────

TEST(KalmanTests, Test_Kalman_Linear_Track)
{
    KalmanPredictor predictor;
    predictor.AddEntity(1, 0.0f, 0.0f);
    ASSERT_TRUE(predictor.HasEntity(1));

    float x = 0.0f, y = 0.0f;

    // Warm-up: 20 ticks of straight-line motion in +X direction.
    // Each tick the filter sees z_k = [x, y] BEFORE the move, then we advance.
    for (int tick = 0; tick < 20; ++tick) {
        predictor.Tick(1, x, y, 1.0f, 0.0f);
        x += kStepPerTick;
    }

    // Prediction tick — no input packet.
    const PredictedInput pred = predictor.Predict(1, x, y);

    // The synthetic direction should still point strongly in +X.
    EXPECT_GT(pred.dirX, 0.9f);
    EXPECT_NEAR(pred.dirY, 0.0f, 0.1f);
}

// ─────────────────────────────────────────────────────────────────────────────

TEST(KalmanTests, Test_Kalman_PacketLoss_Recovery)
{
    KalmanPredictor predictor;
    predictor.AddEntity(1, 0.0f, 0.0f);

    float x = 0.0f, y = 0.0f;

    // Warm-up: 20 ticks straight +X.
    for (int tick = 0; tick < 20; ++tick) {
        predictor.Tick(1, x, y, 1.0f, 0.0f);
        x += kStepPerTick;
    }

    const float xBeforeLoss = x;

    // 3 consecutive prediction ticks (simulate packet loss).
    // We apply the synthetic input to the "simulated" position so subsequent
    // Predict() calls receive an updated position (mirrors what GameWorld would do).
    for (int i = 0; i < 3; ++i) {
        const PredictedInput pred = predictor.Predict(1, x, y);
        x += pred.dirX * kStepPerTick;
        y += pred.dirY * kStepPerTick;
    }

    // Ideal position after 3 straight-line steps.
    const float idealX = xBeforeLoss + 3.0f * kStepPerTick;
    const float idealY = 0.0f;

    const float errorX = std::abs(x - idealX);
    const float errorY = std::abs(y - idealY);

    EXPECT_LT(errorX, 0.5f) << "X error after 3 lost packets: " << errorX;
    EXPECT_LT(errorY, 0.5f) << "Y error after 3 lost packets: " << errorY;
}

// ─────────────────────────────────────────────────────────────────────────────

TEST(KalmanTests, Test_Kalman_DirectionChange_Tracking)
{
    KalmanPredictor predictor;
    predictor.AddEntity(1, 0.0f, 0.0f);

    float x = 0.0f, y = 0.0f;

    // Phase 1: 20 ticks in +X direction.
    for (int tick = 0; tick < 20; ++tick) {
        predictor.Tick(1, x, y, 1.0f, 0.0f);
        x += kStepPerTick;
    }

    // Phase 2: 10 ticks in -X direction (180° reversal with real input).
    for (int tick = 0; tick < 10; ++tick) {
        predictor.Tick(1, x, y, -1.0f, 0.0f);
        x -= kStepPerTick;
    }

    // After 10 ticks of real -X input the filter's velocity estimate should
    // have shifted toward -X.  Verify the next synthetic prediction points
    // into the same hemisphere as (-1, 0): dot product > 0.
    const PredictedInput pred = predictor.Predict(1, x, y);
    const float dot = pred.dirX * (-1.0f) + pred.dirY * 0.0f;

    EXPECT_GT(dot, 0.0f)
        << "Predicted direction (" << pred.dirX << ", " << pred.dirY
        << ") should point toward -X after 10 ticks of reversal";
}

// ─────────────────────────────────────────────────────────────────────────────

TEST(KalmanTests, Test_Kalman_AddRemove_Idempotent)
{
    KalmanPredictor predictor;

    predictor.AddEntity(42, 10.0f, 20.0f);
    EXPECT_TRUE(predictor.HasEntity(42));

    // Second AddEntity for the same id is a no-op (should not throw or corrupt).
    predictor.AddEntity(42, 99.0f, 99.0f);
    EXPECT_TRUE(predictor.HasEntity(42));

    predictor.RemoveEntity(42);
    EXPECT_FALSE(predictor.HasEntity(42));

    // Removing a non-existent entity is safe.
    predictor.RemoveEntity(42);
    EXPECT_FALSE(predictor.HasEntity(42));
}

TEST(KalmanTests, Test_Kalman_UnknownEntity_ReturnsZero)
{
    KalmanPredictor predictor;

    // Predict on an entity that was never registered.
    const PredictedInput pred = predictor.Predict(999, 0.0f, 0.0f);
    EXPECT_FLOAT_EQ(pred.dirX, 0.0f);
    EXPECT_FLOAT_EQ(pred.dirY, 0.0f);
}
