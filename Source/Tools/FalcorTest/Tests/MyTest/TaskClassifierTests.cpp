#include "Testing/UnitTest.h"

// TaskClassifier 는 ImportancePass 플러그인 DLL 안에 있어 링크할 수 없으므로 소스를 직접 포함한다.
// CMakeLists.txt 에서 이 파일에 /utf-8 을 지정한다. 없으면 한국어 주석이 다음 줄 코드를 지울 수 있다.
#include "../../../../RenderPasses/ImportancePass/TaskClassifier.cpp"

#include <cmath>
#include <vector>

namespace Falcor
{
namespace
{
const float kPi = 3.14159265f;
const float kZero[3] = {0.f, 0.f, 0.f};
const float kFocalPx = 945.f;

// 고정 60 fps 로 입력을 넣는 시뮬레이터
struct Sim
{
    TaskClassifier c;
    uint64_t frame = 0;
    double t = 0.0;
    float dt = 1.f / 60.f;
    float pos[3] = {0.f, 1.7f, 0.f};
    float yaw = 0.f; // deg

    Sim()
    {
        TaskConfig cfg;
        cfg.centroidEmaAlpha = 1.f; // 평활화 없이 규칙만 확인한다.
        c.setConfig(cfg);
    }

    UserTask step(float dYaw, const float move[3], bool fire = false, const EnemySample* pEnemy = nullptr)
    {
        yaw += dYaw;
        for (int i = 0; i < 3; i++)
            pos[i] += move[i];

        FrameInput in;
        in.frame = frame;
        in.time = t;
        in.dt = frame == 0 ? 0.f : dt;
        const float y = yaw * kPi / 180.f;
        in.camFwd[0] = std::sin(y);
        in.camFwd[1] = 0.f;
        in.camFwd[2] = std::cos(y);
        for (int i = 0; i < 3; i++)
            in.camPos[i] = pos[i];
        in.fire = fire;

        const UserTask task = c.update(in, pEnemy);
        frame++;
        t += dt;
        return task;
    }

    // 조준점에서 offsetPx 만큼 떨어진 적 샘플
    EnemySample enemyAt(float offsetPx, uint32_t pixels = 500, bool center = false) const
    {
        EnemySample e;
        e.frame = frame;
        e.time = t;
        e.pixelCount = pixels;
        e.aimPoint[0] = 960.5f;
        e.aimPoint[1] = 540.5f;
        e.centroid[0] = 960.5f + offsetPx;
        e.centroid[1] = 540.5f;
        e.focalPx = kFocalPx;
        e.centerHit = center;
        return e;
    }

    void stepEnemy(int frames, float thetaDeg)
    {
        const float offsetPx = kFocalPx * std::tan(thetaDeg * kPi / 180.f);
        for (int i = 0; i < frames; i++)
        {
            const EnemySample e = enemyAt(offsetPx);
            step(0.f, kZero, false, &e);
        }
    }
};
} // namespace

CPU_TEST(TaskClassifier_DefaultsToHold)
{
    Sim s;
    EXPECT(s.step(0.f, kZero) == UserTask::Hold);
    for (int i = 0; i < 30; i++)
        s.step(0.f, kZero);
    EXPECT(s.c.getTask() == UserTask::Hold);
}

CPU_TEST(TaskClassifier_TraverseAfterMinHold)
{
    Sim s;
    s.step(0.f, kZero);
    const float forward[3] = {0.f, 0.f, 2.f / 60.f};
    s.step(0.f, forward);
    EXPECT(s.c.getTask() == UserTask::Hold);
    for (int i = 0; i < 10; i++)
        s.step(0.f, forward);
    EXPECT(s.c.getTask() == UserTask::Traverse);
    EXPECT_LT(std::fabs(s.c.getSignals().v - 2.f), 1e-3f);
    EXPECT_LT(s.c.getSignals().phi, 1.f);
}

CPU_TEST(TaskClassifier_ScanInPlace)
{
    Sim s;
    s.step(0.f, kZero);
    for (int i = 0; i < 12; i++)
        s.step(1.f, kZero);
    EXPECT(s.c.getTask() == UserTask::Scan);
    EXPECT_LT(std::fabs(s.c.getSignals().omega - 60.f), 0.5f);
}

CPU_TEST(TaskClassifier_RotateWhileMoving)
{
    // 진행 방향을 보며 이동 + 회전 -> Traverse
    Sim headOn;
    headOn.step(0.f, kZero);
    const float forward[3] = {0.f, 0.f, 2.f / 60.f};
    for (int i = 0; i < 12; i++)
        headOn.step(0.6f, forward);
    EXPECT(headOn.c.getTask() == UserTask::Traverse);

    // 옆으로 이동 + 회전 -> Scan
    Sim strafe;
    strafe.step(0.f, kZero);
    const float side[3] = {2.f / 60.f, 0.f, 0.f};
    for (int i = 0; i < 12; i++)
        strafe.step(0.6f, side);
    EXPECT(strafe.c.getTask() == UserTask::Scan);
}

CPU_TEST(TaskClassifier_FlickEntersImmediatelyAndFireEndsIt)
{
    Sim s;
    s.step(0.f, kZero);
    for (int i = 0; i < 20; i++)
        s.step(0.f, kZero);

    EnemySample e = s.enemyAt(300.f);
    s.step(0.f, kZero, false, &e);
    EXPECT(s.c.getTask() == UserTask::Hold); // 적이 보여도 조준 전이면 비교전

    e = s.enemyAt(200.f);
    s.step(4.f, kZero, false, &e);
    EXPECT(s.c.getTask() == UserTask::Flick);

    e = s.enemyAt(100.f);
    s.step(4.f, kZero, false, &e);
    e = s.enemyAt(20.f);
    s.step(3.f, kZero, false, &e);
    EXPECT(s.c.getTask() == UserTask::Flick);

    e = s.enemyAt(5.f);
    s.step(3.f, kZero, true, &e);
    EXPECT(s.c.getTask() == UserTask::Track);
}

CPU_TEST(TaskClassifier_LowOmegaEndsFlick)
{
    Sim s;
    s.step(0.f, kZero);
    EnemySample e = s.enemyAt(300.f);
    s.step(0.f, kZero, false, &e);
    e = s.enemyAt(200.f);
    s.step(4.f, kZero, false, &e);
    EXPECT(s.c.getTask() == UserTask::Flick);
    s.step(0.5f, kZero); // 30°/s < omegaFlickExit
    EXPECT(s.c.getTask() != UserTask::Flick);
}

CPU_TEST(TaskClassifier_ThetaTrackHysteresis)
{
    Sim s;
    s.step(0.f, kZero);
    s.stepEnemy(12, 4.f);
    EXPECT(s.c.getTask() == UserTask::Hold); // 진입 3°
    s.stepEnemy(12, 2.f);
    EXPECT(s.c.getTask() == UserTask::Track);
    s.stepEnemy(12, 4.f);
    EXPECT(s.c.getTask() == UserTask::Track); // 해제 5°
    s.stepEnemy(12, 6.f);
    EXPECT(s.c.getTask() == UserTask::Hold);
}

CPU_TEST(TaskClassifier_MinPixelsFiltersNoise)
{
    Sim s;
    s.step(0.f, kZero);
    for (int i = 0; i < 12; i++)
    {
        const EnemySample e = s.enemyAt(0.f, 5, true);
        s.step(0.f, kZero, false, &e);
    }
    EXPECT(!s.c.getSignals().enemyVisible);
    EXPECT(s.c.getTask() == UserTask::Hold);
}

CPU_TEST(TaskClassifier_FireChunks)
{
    Sim s;
    s.step(0.f, kZero);
    s.step(0.f, kZero, true);
    for (int i = 0; i < 29; i++) // +0.5 s
        s.step(0.f, kZero);
    s.step(0.f, kZero, true);
    EXPECT_EQ(s.c.getSignals().fireChunk, 1u);
    for (int i = 0; i < 89; i++) // +1.5 s
        s.step(0.f, kZero);
    EXPECT(!s.c.getSignals().recentFire);
    s.step(0.f, kZero, true);
    EXPECT_EQ(s.c.getSignals().fireChunk, 2u);
}

CPU_TEST(TaskClassifier_YawWrap)
{
    Sim s;
    s.yaw = 179.f;
    s.step(0.f, kZero);
    s.step(2.f, kZero);
    EXPECT_LT(std::fabs(s.c.getSignals().omega - 120.f), 0.5f);
}

CPU_TEST(TaskClassifier_ReplayIsDeterministic)
{
    std::vector<UserTask> runs[2];
    for (int r = 0; r < 2; r++)
    {
        Sim s;
        for (int i = 0; i < 300; i++)
        {
            const float move[3] = {0.f, 0.f, (i % 100 < 50) ? 2.f / 60.f : 0.f};
            const EnemySample e = s.enemyAt(300.f - (i % 60) * 5.f);
            runs[r].push_back(s.step((i % 40 < 10) ? 3.f : 0.2f, move, i % 97 == 0, (i % 3 == 0) ? &e : nullptr));
        }
    }
    EXPECT(runs[0] == runs[1]);
}

CPU_TEST(TaskClassifier_EmaReducesCentroidJitter)
{
    Sim s;
    TaskConfig cfg = s.c.getConfig();
    cfg.centroidEmaAlpha = 0.3f;
    s.c.setConfig(cfg);
    s.step(0.f, kZero);

    float maxDev = 0.f;
    for (int i = 0; i < 60; i++)
    {
        const EnemySample e = s.enemyAt(100.f + ((i % 2) ? 10.f : -10.f));
        s.step(0.f, kZero, false, &e);
        if (i > 10)
            maxDev = std::fmax(maxDev, std::fabs(s.c.getSignals().centroid[0] - 1060.5f));
    }
    EXPECT_LT(maxDev, 10.f); // 입력 떨림은 ±10 px
}

// R-5.1 Scan: yaw 감소(오른쪽 회전)는 +1, 증가(왼쪽 회전)는 -1, 정지는 0
CPU_TEST(TaskClassifier_ScanDirFollowsTurnDirection)
{
    Sim right;
    right.step(0.f, kZero);
    for (int i = 0; i < 10; i++)
        right.step(-1.f, kZero);
    EXPECT_LT(std::fabs(right.c.getSignals().scanDir - 1.f), 1e-3f);

    Sim left;
    left.step(0.f, kZero);
    for (int i = 0; i < 10; i++)
        left.step(1.f, kZero);
    EXPECT_LT(std::fabs(left.c.getSignals().scanDir + 1.f), 1e-3f);

    Sim still;
    still.step(0.f, kZero);
    still.step(0.f, kZero);
    EXPECT_EQ(still.c.getSignals().scanDir, 0.f);
}
} // namespace Falcor
