#pragma once
/***************************************************************************
 # TaskClassifier.h
 # 사용자 task 5분류 판정기 (spec_TSK.md R-1 ~ R-4)
 #   Hold / Scan / Traverse / Track / Flick
 # Falcor 의존성이 없는 순수 CPU 코드다. 같은 입력 열을 넣으면 같은 판정이
 # 나와야 하므로(R-6.2 재생) 내부에서 시계를 읽지 않고 입력의 시각만 쓴다.
 ***************************************************************************/
#include <cstdint>

// GPU 로 넘기는 task ID. ImportancePass.cs.slang 의 activeTask 와 값이 같아야 한다.
enum class UserTask : uint32_t
{
    Hold     = 0,
    Scan     = 1,
    Traverse = 2,
    Track    = 3,
    Flick    = 4,
};

const char* toString(UserTask task);

// 판정 임계값. 실제 값은 TaskConfig.json 에서 읽는다 (R-4.6).
// 아래 기본값은 설정 파일을 읽지 못했을 때만 쓰며 TaskConfig.json 과 같게 유지한다.
struct TaskConfig
{
    // 적 식별 (R-2)
    float    enemyRedThreshold   = 0.55f; // 알베도 r 최소값
    float    enemyRedDominance   = 0.90f; // r / max(r,g,b) 최소값
    float    enemyGreenMaxRatio  = 0.40f; // g < r * ratio
    uint32_t enemyMinPixels      = 20;    // 적 가시로 보는 최소 마스크 픽셀 수
    float    centroidEmaAlpha    = 0.5f;  // 마스크 중심 EMA 계수 (1 = 평활화 없음)

    // task 판정 (R-4)
    float omegaScan            = 30.f;   // °/s
    float omegaFlick           = 150.f;  // °/s
    float omegaFlickExit       = 60.f;   // °/s
    float k                    = 50.f;   // Flick 진입: dθ/dt ≤ -k (°/s)
    float thetaTrack           = 3.f;    // °, Track 진입
    float thetaTrackFire       = 6.f;    // °, 최근 발사 후 Track 진입
    float thetaTrackExitMargin = 2.f;    // °, Track 해제 = 진입 + margin
    float vMove                = 0.5f;   // m/s
    float phiHead              = 30.f;   // °
    float minHoldSec           = 0.15f;  // Flick 외 전환의 최소 유지 시간

    // 발사 (R-3.7)
    float fireChunkGapSec      = 1.0f;   // 이 간격 미만의 연속 발사는 한 묶음
    float recentFireWindowSec  = 1.0f;   // "최근 발사" 판정 창
};

// 프레임마다 CPU 에서 수집하는 입력 (R-1)
struct FrameInput
{
    uint64_t frame = 0;      // 프레임 번호
    double   time  = 0.0;    // 타임스탬프 (s)
    float    dt    = 0.f;    // 직전 프레임과의 간격 (s), 첫 프레임은 0
    float    camPos[3] = {}; // 카메라 위치 (씬 단위 = m)
    float    camFwd[3] = {}; // 카메라 전방 단위 벡터
    bool     keyW = false, keyA = false, keyS = false, keyD = false;
    bool     fire = false;   // 이번 프레임에 발사 입력이 있었는지
};

// GPU 적 마스크 통계 (비동기 readback 으로 몇 프레임 늦게 도착한다, R-4.7)
struct EnemySample
{
    uint64_t frame      = 0;     // GPU 가 이 값을 계산한 프레임 번호
    double   time       = 0.0;   // 그 프레임의 타임스탬프 (s)
    uint32_t pixelCount = 0;     // 적 마스크 픽셀 수
    float    centroid[2] = {};   // 마스크 중심 (픽셀 좌표)
    float    aimPoint[2] = {};   // 조준점 c (픽셀 좌표)
    float    focalPx    = 1.f;   // 초점거리 (픽셀 단위)
    bool     centerHit  = false; // 조준점 픽셀이 적 마스크 안인지 (①)
    float    bboxUV[4]  = {};    // 적 마스크 바운딩 박스 x0, y0, x1, y1 (UV). pixelCount 가 0이면 무효 (R-5.1)
};

// 계산된 신호 (R-3). 로그(R-6.1)와 UI 표시에 쓴다.
struct TaskSignals
{
    float    yaw = 0.f, pitch = 0.f;  // °
    float    omega = 0.f;             // ② 카메라 각속도 °/s
    float    v = 0.f;                 // ④ 이동 속도 m/s
    float    phi = 0.f;               // ⑤ 시선-이동 각도 차 °, v ≥ vMove 일 때만 의미 있음 (아니면 0)
    float    scanDir = 0.f;           // 회전 방향 -1(왼쪽)..+1(오른쪽). -yaw속도 ÷ ω_scan 을 포화 (R-5.1 Scan)
    bool     enemyVisible = false;    // 마스크 픽셀 수 ≥ enemyMinPixels
    bool     centerHit = false;       // ①
    float    theta = 0.f;             // ⑦ 화면 기반 θ °, 적 비가시면 0
    float    dThetaDt = 0.f;          // ⑧ °/s (평활화 후)
    float    centroid[2] = {};        // 평활화된 마스크 중심 (픽셀)
    uint32_t enemyPixels = 0;
    bool     fire = false;            // ⑩ 이번 프레임 발사
    bool     recentFire = false;      // 최근 recentFireWindowSec 안에 발사
    uint32_t fireChunk = 0;           // 사격 묶음 번호 (0 = 아직 발사 없음)
    uint32_t enemyLatencyFrames = 0;  // 적 신호가 몇 프레임 늦은 값인지
};

class TaskClassifier
{
public:
    void setConfig(const TaskConfig& config) { mConfig = config; }
    const TaskConfig& getConfig() const { return mConfig; }

    void reset();

    /** 한 프레임을 판정한다. 매 프레임 정확히 하나의 task 를 반환한다 (R-4.2).
        @param input 이번 프레임 입력.
        @param pEnemy 이번 프레임에 새로 도착한 적 통계. 없으면 nullptr (직전 값을 유지).
    */
    UserTask update(const FrameInput& input, const EnemySample* pEnemy);

    UserTask getTask() const { return mTask; }
    const TaskSignals& getSignals() const { return mSignals; }

private:
    void updateCameraSignals(const FrameInput& input);
    void updateEnemySignals(const FrameInput& input, const EnemySample* pEnemy);
    void updateFire(const FrameInput& input);
    UserTask classifyRaw(bool& flickExited) const;

    TaskConfig  mConfig;
    TaskSignals mSignals;

    UserTask mTask        = UserTask::Hold;
    UserTask mPending     = UserTask::Hold;
    float    mPendingTime = 0.f;

    bool  mHasPrevCam = false;
    float mPrevPos[3] = {};
    float mPrevYaw = 0.f, mPrevPitch = 0.f;

    bool   mEnemyReceived  = false;   // 적 통계를 한 번이라도 받았는지
    bool   mHasEnemySample = false;   // 평활화 상태가 유효한지
    double mPrevEnemyTime = 0.0;
    float  mPrevTheta = 0.f;

    bool   mHasFired = false;
    double mLastFireTime = 0.0;
};
