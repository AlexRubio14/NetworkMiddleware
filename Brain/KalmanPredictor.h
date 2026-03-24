// Brain/KalmanPredictor.h — P-5.2 Silent Server-Side Kalman Predictor
//
// Maintains a 4-state constant-velocity Kalman filter per entity:
//   state x = [px, py, vx, vy]^T
//
// Purpose: synthesize a plausible InputPayload when a client's input packet
// is lost, keeping the GameWorld simulation smooth without any wire-format
// changes.  Brain has no dependency on MiddlewareShared; the orchestrator
// (main.cpp) converts PredictedInput ↔ InputPayload.
//
// Matrices (dt = 0.01 s, 100 Hz fixed tick):
//   F  = [[1,0,dt,0],[0,1,0,dt],[0,0,1,0],[0,0,0,1]]   4x4 transition
//   H  = [[1,0,0,0],[0,1,0,0]]                          2x4 observation (position)
//   Q  = diag(0.001, 0.001, 0.05, 0.05)                 process noise
//   R  = 0.015 * I2                                      measurement noise
//   P0 = I4                                              initial covariance
//
// Usage per tick (orchestrated from main.cpp):
//   z_k = GameWorld::GetHeroState(id)->[x, y]   // position BEFORE this tick's input
//   if (real input arrived):
//       kalman.Tick(id, z_k.x, z_k.y, dir.x, dir.y)   // predict + update
//       apply real InputPayload to GameWorld
//   else:
//       pred = kalman.Predict(id, z_k.x, z_k.y)        // predict only
//       apply synthetic InputPayload{pred.dirX, pred.dirY, 0} to GameWorld
#pragma once
#include <cstdint>
#include <unordered_map>

namespace NetworkMiddleware::Brain {

// Direction output from the predictor.
// Brain-internal type — no dependency on MiddlewareShared.
struct PredictedInput {
    float dirX = 0.0f;
    float dirY = 0.0f;
};

// Per-entity Kalman filter state.
struct KalmanState {
    float x[4]  = {};   // [px, py, vx, vy]
    float P[16] = {};   // 4x4 error covariance (row-major)
};

class KalmanPredictor {
public:
    static constexpr float kDt = 0.01f;

    KalmanPredictor();

    // Register a new entity at its spawn position (called on client connect).
    // Safe to call multiple times for the same id (idempotent).
    void AddEntity(uint32_t entityID, float initialX, float initialY);

    // Deregister an entity (called on client disconnect / removal).
    void RemoveEntity(uint32_t entityID);

    bool HasEntity(uint32_t entityID) const;

    // Called when a real input packet was received this tick.
    // Runs predict + update (z_k = currentX/Y from GameWorld).
    // Returns the same direction — allows uniform call site in main.cpp.
    PredictedInput Tick(uint32_t entityID,
                        float currentX, float currentY,
                        float realDirX, float realDirY);

    // Called when NO input arrived this tick.
    // Runs predict step only; synthesizes direction from predicted velocity.
    PredictedInput Predict(uint32_t entityID,
                           float currentX, float currentY);

private:
    std::unordered_map<uint32_t, KalmanState> m_states;

    // Pre-computed constant matrices (initialised in constructor).
    float m_F[16];   // 4x4 state transition
    float m_H[8];    // 2x4 observation
    float m_Q[16];   // 4x4 process noise
    float m_R[4];    // 2x2 measurement noise
    float m_P0[16];  // 4x4 initial covariance

    // Internal helpers.
    void  PredictStep(KalmanState& s, float xPred[4], float pPred[16]) const;
    void  UpdateStep (KalmanState& s,
                      const float xPred[4], const float pPred[16],
                      float zx, float zy) const;
    static PredictedInput SynthesizeFromVelocity(const float x[4]);
};

}  // namespace NetworkMiddleware::Brain
