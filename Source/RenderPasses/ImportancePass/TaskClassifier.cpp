#include "TaskClassifier.h"
#include <algorithm>
#include <cmath>

namespace
{
constexpr float kRadToDeg = 57.29577951f;

// 각도 차를 [-180, 180) 으로 감싼다 (yaw 가 ±180 을 넘나들 때).
float wrapDeg(float d)
{
    d = std::fmod(d + 180.f, 360.f);
    if (d < 0.f) d += 360.f;
    return d - 180.f;
}
} // namespace

const char* toString(UserTask task)
{
    switch (task)
    {
    case UserTask::Hold:     return "Hold";
    case UserTask::Scan:     return "Scan";
    case UserTask::Traverse: return "Traverse";
    case UserTask::Track:    return "Track";
    case UserTask::Flick:    return "Flick";
    }
    return "Unknown";
}

void TaskClassifier::reset()
{
    const TaskConfig config = mConfig;
    *this = TaskClassifier();
    mConfig = config;
}

// ② ω, ④ v, ⑤ φ
void TaskClassifier::updateCameraSignals(const FrameInput& in)
{
    TaskSignals& s = mSignals;
    s.yaw   = std::atan2(in.camFwd[0], in.camFwd[2]) * kRadToDeg;
    s.pitch = std::asin(std::clamp(in.camFwd[1], -1.f, 1.f)) * kRadToDeg;

    s.omega   = 0.f;
    s.v       = 0.f;
    s.phi     = 0.f;
    s.scanDir = 0.f;
    if (mHasPrevCam && in.dt > 0.f)
    {
        const float dYaw   = wrapDeg(s.yaw - mPrevYaw);
        const float dPitch = s.pitch - mPrevPitch;
        s.omega = std::sqrt(dYaw * dYaw + dPitch * dPitch) / in.dt;

        // 화면 오른쪽 U = cross(fwd, up) 이고 yaw = atan2(fx, fz) 이므로,
        // yaw 가 늘면 시선은 -U(왼쪽)로 돈다. 그래서 부호를 뒤집는다.
        s.scanDir = std::clamp(-dYaw / in.dt / mConfig.omegaScan, -1.f, 1.f);

        const float d[3] = { in.camPos[0] - mPrevPos[0], in.camPos[1] - mPrevPos[1], in.camPos[2] - mPrevPos[2] };
        s.v = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]) / in.dt;

        // φ: 수평 전방 벡터와 수평 이동 벡터 사이 각. 이동 중일 때만 정의한다.
        const float fLen = std::hypot(in.camFwd[0], in.camFwd[2]);
        const float mLen = std::hypot(d[0], d[2]);
        if (s.v >= mConfig.vMove && fLen > 1e-6f && mLen > 1e-6f)
        {
            const float c = (in.camFwd[0] * d[0] + in.camFwd[2] * d[2]) / (fLen * mLen);
            s.phi = std::acos(std::clamp(c, -1.f, 1.f)) * kRadToDeg;
        }
    }

    mHasPrevCam = true;
    std::copy(in.camPos, in.camPos + 3, mPrevPos);
    mPrevYaw   = s.yaw;
    mPrevPitch = s.pitch;
}

// ① 중앙 픽셀, 적 가시, ⑦ θ, ⑧ dθ/dt (R-2.2, R-2.3, R-3.5, R-3.6)
void TaskClassifier::updateEnemySignals(const FrameInput& in, const EnemySample* pEnemy)
{
    TaskSignals& s = mSignals;
    if (!pEnemy)
    {
        // 새 샘플이 없으면 직전 적 신호를 유지하고 지연 프레임 수만 늘린다.
        if (mEnemyReceived) s.enemyLatencyFrames++;
        return;
    }

    mEnemyReceived = true;
    s.enemyLatencyFrames = static_cast<uint32_t>(in.frame - pEnemy->frame);
    s.enemyPixels  = pEnemy->pixelCount;
    s.centerHit    = pEnemy->centerHit;
    s.enemyVisible = pEnemy->pixelCount >= mConfig.enemyMinPixels;

    if (!s.enemyVisible)
    {
        mHasEnemySample = false;
        s.theta    = 0.f;
        s.dThetaDt = 0.f;
        return;
    }

    // 마스크 중심 EMA. 적이 새로 보이면 원래 값에서 시작한다.
    const float a = mHasEnemySample ? mConfig.centroidEmaAlpha : 1.f;
    s.centroid[0] += a * (pEnemy->centroid[0] - s.centroid[0]);
    s.centroid[1] += a * (pEnemy->centroid[1] - s.centroid[1]);

    // θ ≈ atan(|p - c| / f)
    const float dist = std::hypot(s.centroid[0] - pEnemy->aimPoint[0], s.centroid[1] - pEnemy->aimPoint[1]);
    s.theta = std::atan(dist / std::max(pEnemy->focalPx, 1e-6f)) * kRadToDeg;

    // dθ/dt 는 샘플이 계산된 프레임 시각 사이로 나눈다 (readback 지연과 무관).
    const double sampleDt = pEnemy->time - mPrevEnemyTime;
    s.dThetaDt = (mHasEnemySample && sampleDt > 0.0) ? static_cast<float>((s.theta - mPrevTheta) / sampleDt) : 0.f;

    mHasEnemySample = true;
    mPrevEnemyTime  = pEnemy->time;
    mPrevTheta      = s.theta;
}

// ⑩ 발사, 사격 묶음 (R-3.7)
void TaskClassifier::updateFire(const FrameInput& in)
{
    TaskSignals& s = mSignals;
    s.fire = in.fire;
    if (in.fire)
    {
        if (!mHasFired || in.time - mLastFireTime >= mConfig.fireChunkGapSec) s.fireChunk++;
        mHasFired     = true;
        mLastFireTime = in.time;
    }
    s.recentFire = mHasFired && (in.time - mLastFireTime) < mConfig.recentFireWindowSec;
}

// 판정 순서 (spec 3.3절). 유지 시간은 여기서 보지 않는다.
UserTask TaskClassifier::classifyRaw(bool& flickExited) const
{
    const TaskSignals& s = mSignals;
    const TaskConfig&  c = mConfig;

    // Flick 은 종료 조건(발사 또는 ω < ω_flick_exit)이 올 때까지 유지한다 (R-4.5).
    flickExited = false;
    if (mTask == UserTask::Flick)
    {
        if (s.fire || s.omega < c.omegaFlickExit) flickExited = true;
        else return UserTask::Flick;
    }

    // 같은 프레임에 Flick 을 끝내고 다시 들어가지 않는다.
    if (!flickExited && s.enemyVisible && s.omega >= c.omegaFlick && s.dThetaDt <= -c.k)
        return UserTask::Flick;

    // θ_track 진입/해제 히스테리시스 (R-4.4)
    float thetaThr = s.recentFire ? c.thetaTrackFire : c.thetaTrack;
    if (mTask == UserTask::Track) thetaThr += c.thetaTrackExitMargin;
    const bool aiming = s.centerHit || s.theta <= thetaThr;
    if (s.enemyVisible && aiming) return UserTask::Track;

    const bool movingHeadOn = s.v >= c.vMove && s.phi <= c.phiHead;
    if (s.omega >= c.omegaScan && !movingHeadOn) return UserTask::Scan;
    if (s.v >= c.vMove) return UserTask::Traverse;
    return UserTask::Hold;
}

UserTask TaskClassifier::update(const FrameInput& input, const EnemySample* pEnemy)
{
    updateCameraSignals(input);
    updateEnemySignals(input, pEnemy);
    updateFire(input);

    bool flickExited = false;
    const UserTask raw = classifyRaw(flickExited);

    if (raw == UserTask::Flick || flickExited)
    {
        // Flick 진입과 종료는 유지 시간 없이 즉시 반영한다 (R-4.3, R-4.5).
        mTask        = raw;
        mPending     = raw;
        mPendingTime = 0.f;
    }
    else if (raw == mTask)
    {
        mPending     = mTask;
        mPendingTime = 0.f;
    }
    else
    {
        // 그 외 전환은 같은 후보가 minHoldSec 이상 이어져야 반영한다 (R-4.3).
        if (raw != mPending)
        {
            mPending     = raw;
            mPendingTime = 0.f;
        }
        mPendingTime += input.dt;
        if (mPendingTime >= mConfig.minHoldSec)
        {
            mTask        = raw;
            mPendingTime = 0.f;
        }
    }
    return mTask;
}
