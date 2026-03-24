// Brain/KalmanPredictor.cpp — P-5.2

#include "KalmanPredictor.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace NetworkMiddleware::Brain {

// ─── Fixed-size matrix helpers (anonymous namespace) ─────────────────────────
//
// All matrices stored row-major.  Naming convention: mat_mul<ROWS_A, INNER, COLS_B>.

namespace {

// C[r,c] = sum_k A[r,k] * B[k,c]
template<int ROWS_A, int INNER, int COLS_B>
void mat_mul(const float*  A, const float*  B,
             float*  C)
{
    for (int r = 0; r < ROWS_A; ++r) {
        for (int c = 0; c < COLS_B; ++c) {
            float sum = 0.0f;
            for (int k = 0; k < INNER; ++k)
                sum += A[r * INNER + k] * B[k * COLS_B + c];
            C[r * COLS_B + c] = sum;
        }
    }
}

// out[r] = sum_c M[r,c] * v[c]
template<int ROWS, int COLS>
void mat_mul_vec(const float*  M, const float*  v,
                 float*  out)
{
    for (int r = 0; r < ROWS; ++r) {
        float sum = 0.0f;
        for (int c = 0; c < COLS; ++c)
            sum += M[r * COLS + c] * v[c];
        out[r] = sum;
    }
}

// Transpose: AT[c,r] = A[r,c]
template<int ROWS, int COLS>
void mat_transpose(const float*  A, float*  AT)
{
    for (int r = 0; r < ROWS; ++r)
        for (int c = 0; c < COLS; ++c)
            AT[c * ROWS + r] = A[r * COLS + c];
}

// Element-wise add (same shape, N total elements).
template<int N>
void mat_add(const float*  A, const float*  B,
             float*  C)
{
    for (int i = 0; i < N; ++i) C[i] = A[i] + B[i];
}

// 2x2 invert via explicit determinant formula.
// Returns false if singular (|det| < eps); caller falls back to predict-only.
bool mat22_inverse(const float*  M, float*  inv)
{
    const float det = M[0] * M[3] - M[1] * M[2];
    if (std::abs(det) < 1e-10f) return false;
    const float d = 1.0f / det;
    inv[0] =  M[3] * d;
    inv[1] = -M[1] * d;
    inv[2] = -M[2] * d;
    inv[3] =  M[0] * d;
    return true;
}

}  // anonymous namespace

// ─── KalmanPredictor ──────────────────────────────────────────────────────────

KalmanPredictor::KalmanPredictor()
{
    // F: constant velocity model
    //   [1  0  dt  0 ]
    //   [0  1   0  dt]
    //   [0  0   1   0]
    //   [0  0   0   1]
    std::fill(m_F, m_F + 16, 0.0f);
    m_F[0]  = 1.0f;  m_F[2]  = kDt;   // row 0
    m_F[5]  = 1.0f;  m_F[7]  = kDt;   // row 1
    m_F[10] = 1.0f;                    // row 2
    m_F[15] = 1.0f;                    // row 3

    // H: observe position only
    //   [1  0  0  0]
    //   [0  1  0  0]
    std::fill(m_H, m_H + 8, 0.0f);
    m_H[0] = 1.0f;  // row 0 → px
    m_H[5] = 1.0f;  // row 1 → py

    // Q: diag(0.001, 0.001, 5.0, 5.0)
    // Low position noise (GameWorld is authoritative).
    // Higher velocity noise: MOBA heroes change direction every ~10-30 ticks
    // (100-300 ms reaction time).  Q_vel=5.0 keeps K_vel large enough that
    // a 180° reversal is reflected in the velocity estimate within ~5-8 ticks,
    // while the packet-loss prediction test (3 ticks, straight line) still
    // passes because the velocity ESTIMATE after warm-up remains accurate.
    std::fill(m_Q, m_Q + 16, 0.0f);
    m_Q[0]  = 0.001f;
    m_Q[5]  = 0.001f;
    m_Q[10] = 5.0f;
    m_Q[15] = 5.0f;

    // R: 0.015 * I2  (measurement noise ≈ 16-bit quantisation step)
    std::fill(m_R, m_R + 4, 0.0f);
    m_R[0] = 0.015f;
    m_R[3] = 0.015f;

    // P0: I4
    std::fill(m_P0, m_P0 + 16, 0.0f);
    m_P0[0] = m_P0[5] = m_P0[10] = m_P0[15] = 1.0f;
}

void KalmanPredictor::AddEntity(uint32_t entityID, float initialX, float initialY)
{
    if (m_states.count(entityID)) return;  // idempotent

    KalmanState s{};
    s.x[0] = initialX;
    s.x[1] = initialY;
    // vx, vy initialised to 0 — hero starts stationary
    std::copy(m_P0, m_P0 + 16, s.P);
    m_states.emplace(entityID, s);
}

void KalmanPredictor::RemoveEntity(uint32_t entityID)
{
    m_states.erase(entityID);
}

bool KalmanPredictor::HasEntity(uint32_t entityID) const
{
    return m_states.count(entityID) > 0;
}

PredictedInput KalmanPredictor::Tick(uint32_t entityID,
                                      float currentX, float currentY,
                                      float realDirX, float realDirY)
{
    auto it = m_states.find(entityID);
    if (it == m_states.end())
        return {realDirX, realDirY};

    KalmanState& s = it->second;

    float xPred[4], pPred[16];
    PredictStep(s, xPred, pPred);
    UpdateStep(s, xPred, pPred, currentX, currentY);

    return {realDirX, realDirY};
}

PredictedInput KalmanPredictor::Predict(uint32_t entityID,
                                         float currentX, float currentY)
{
    auto it = m_states.find(entityID);
    if (it == m_states.end())
        return {};

    KalmanState& s = it->second;

    // Suppress unused-parameter warning: currentX/Y are not used in predict-only
    // mode (no measurement to assimilate), but are part of the uniform API so
    // callers always pass the authoritative position.
    (void)currentX;
    (void)currentY;

    float xPred[4], pPred[16];
    PredictStep(s, xPred, pPred);

    // No update — commit the predicted state directly.
    std::copy(xPred,  xPred  + 4,  s.x);
    std::copy(pPred,  pPred  + 16, s.P);

    return SynthesizeFromVelocity(s.x);
}

// ─── Private helpers ──────────────────────────────────────────────────────────

// Predict step: x_pred = F * x,  P_pred = F * P * F^T + Q
void KalmanPredictor::PredictStep(KalmanState& s,
                                   float xPred[4],
                                   float pPred[16]) const
{
    // x_pred = F(4x4) * x(4x1)
    mat_mul_vec<4, 4>(m_F, s.x, xPred);

    // P_pred = F * P * F^T + Q
    float FP[16], FT[16], FPFT[16];
    mat_mul<4, 4, 4>(m_F, s.P, FP);
    mat_transpose<4, 4>(m_F, FT);
    mat_mul<4, 4, 4>(FP, FT, FPFT);
    mat_add<16>(FPFT, m_Q, pPred);
}

// Update step: correct x_pred with measurement z = [zx, zy]
void KalmanPredictor::UpdateStep(KalmanState& s,
                                  const float xPred[4],
                                  const float pPred[16],
                                  float zx, float zy) const
{
    // Innovation: y = z - H * x_pred   (2x1)
    float Hx[2];
    mat_mul_vec<2, 4>(m_H, xPred, Hx);
    const float y[2] = {zx - Hx[0], zy - Hx[1]};

    // S = H * P_pred * H^T + R   (2x2)
    float HP[8], HT[8], HPHT[4], S[4];
    mat_mul<2, 4, 4>(m_H, pPred, HP);       // 2x4
    mat_transpose<2, 4>(m_H, HT);           // 4x2
    mat_mul<2, 4, 2>(HP, HT, HPHT);        // 2x2
    mat_add<4>(HPHT, m_R, S);

    // S^{-1}  (2x2)
    float Sinv[4];
    if (!mat22_inverse(S, Sinv)) {
        // S is singular (degenerate covariance) — fall back to predict-only.
        std::copy(xPred, xPred + 4,  s.x);
        std::copy(pPred, pPred + 16, s.P);
        return;
    }

    // K = P_pred * H^T * S^{-1}   (4x2)
    float PHT[8], K[8];
    mat_mul<4, 4, 2>(pPred, HT, PHT);      // 4x2
    mat_mul<4, 2, 2>(PHT, Sinv, K);        // 4x2

    // x̂ = x_pred + K * y   (4x1)
    float Ky[4];
    mat_mul_vec<4, 2>(K, y, Ky);
    for (int i = 0; i < 4; ++i) s.x[i] = xPred[i] + Ky[i];

    // P = (I - K*H) * P_pred   (4x4)
    float KH[16], IKH[16];
    mat_mul<4, 2, 4>(K, m_H, KH);
    constexpr float I4[16] = {
        1,0,0,0,
        0,1,0,0,
        0,0,1,0,
        0,0,0,1
    };
    for (int i = 0; i < 16; ++i) IKH[i] = I4[i] - KH[i];
    mat_mul<4, 4, 4>(IKH, pPred, s.P);
}

// Derive a normalised direction vector from the velocity part of the state.
PredictedInput KalmanPredictor::SynthesizeFromVelocity(const float x[4])
{
    const float vx = x[2];
    const float vy = x[3];
    const float speed = std::sqrt(vx * vx + vy * vy);
    if (speed < 1e-6f)
        return {};  // stationary — no movement
    return {vx / speed, vy / speed};
}

}  // namespace NetworkMiddleware::Brain
