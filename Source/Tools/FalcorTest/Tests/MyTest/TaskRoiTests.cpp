#include "Testing/UnitTest.h"

// TaskRoi 는 ImportancePass 플러그인 DLL 안에 있어 링크할 수 없으므로 소스를 직접 포함한다.
#include "../../../../RenderPasses/ImportancePass/TaskRoi.cpp"

namespace Falcor
{
namespace
{
const float kAspect = 16.f / 9.f;

// 기본 파라미터: 원 반지름 0.2, 띠 반높이 0.15, 경로 반너비 0.15, 회랑 반너비 0.04
RoiInput makeInput(UserTask task, float u, float v)
{
    RoiInput in;
    in.task = task;
    in.u = u;
    in.v = v;
    in.aspect = kAspect;
    return in;
}

// 높이 단위 거리 d 만큼 떨어진 UV 가로 위치 (가로는 종횡비로 나눈다)
float uOffset(float heightUnits) { return heightUnits / kAspect; }
} // namespace

// ---- Hold ----
CPU_TEST(TaskRoi_HoldIsCircleAroundAim)
{
    EXPECT(inTaskRoi(makeInput(UserTask::Hold, 0.5f, 0.5f)));
    EXPECT(inTaskRoi(makeInput(UserTask::Hold, 0.5f + uOffset(0.15f), 0.5f)));   // 반지름 안
    EXPECT(!inTaskRoi(makeInput(UserTask::Hold, 0.5f + uOffset(0.25f), 0.5f)));  // 반지름 밖
    EXPECT(!inTaskRoi(makeInput(UserTask::Hold, 0.5f, 0.5f + 0.25f)));           // 세로 방향도 원
}

// 원은 종횡비를 반영한다: 같은 높이 거리의 가로·세로 점은 같은 판정을 받아야 한다.
CPU_TEST(TaskRoi_CircleIsNotStretchedByAspect)
{
    const bool horizontal = inTaskRoi(makeInput(UserTask::Hold, 0.5f + uOffset(0.19f), 0.5f));
    const bool vertical   = inTaskRoi(makeInput(UserTask::Hold, 0.5f, 0.5f + 0.19f));
    EXPECT(horizontal && vertical);
}

CPU_TEST(TaskRoi_HoldIgnoresTags)
{
    RoiInput in = makeInput(UserTask::Hold, 0.9f, 0.9f);
    in.tag = ObjectTag::Floor;
    EXPECT(!inTaskRoi(in)); // 원 밖의 바닥은 Hold ROI 가 아니다
}

// ---- Scan ----
CPU_TEST(TaskRoi_ScanBandIsHorizontalAndBiasedToRotation)
{
    RoiInput right = makeInput(UserTask::Scan, 0.9f, 0.5f);
    right.scanDirX = 1.f;
    EXPECT(inTaskRoi(right));              // 회전 방향(오른쪽) 쪽은 안

    RoiInput left = makeInput(UserTask::Scan, 0.1f, 0.5f);
    left.scanDirX = 1.f;
    EXPECT(!inTaskRoi(left));              // 반대쪽 끝은 밖

    RoiInput mirrored = makeInput(UserTask::Scan, 0.1f, 0.5f);
    mirrored.scanDirX = -1.f;
    EXPECT(inTaskRoi(mirrored));           // 방향을 뒤집으면 왼쪽이 안
}

CPU_TEST(TaskRoi_ScanBandIsThinVertically)
{
    RoiInput in = makeInput(UserTask::Scan, 0.5f, 0.1f);
    EXPECT(!inTaskRoi(in));                // 세로로 멀리 떨어진 곳은 띠 밖
    in = makeInput(UserTask::Scan, 0.5f, 0.5f);
    EXPECT(inTaskRoi(in));
}

// ---- Traverse ----
CPU_TEST(TaskRoi_TraverseKeepsCenterAndPathWallsAndFloor)
{
    RoiInput center = makeInput(UserTask::Traverse, 0.5f, 0.5f);
    EXPECT(inTaskRoi(center));             // 중심은 태그와 상관없이 안

    RoiInput pathFloor = makeInput(UserTask::Traverse, 0.5f, 0.9f);
    pathFloor.tag = ObjectTag::Floor;
    EXPECT(inTaskRoi(pathFloor));          // 진행 경로 위의 바닥

    RoiInput pathWall = makeInput(UserTask::Traverse, 0.5f, 0.9f);
    pathWall.tag = ObjectTag::Wall;
    EXPECT(inTaskRoi(pathWall));           // 진행 경로 위의 벽
}

CPU_TEST(TaskRoi_TraverseExcludesOffPathAndNonStructureTags)
{
    RoiInput offPath = makeInput(UserTask::Traverse, 0.95f, 0.9f);
    offPath.tag = ObjectTag::Floor;
    EXPECT(!inTaskRoi(offPath));           // 경로 밖의 바닥

    RoiInput untagged = makeInput(UserTask::Traverse, 0.5f, 0.9f);
    untagged.tag = ObjectTag::None;
    EXPECT(!inTaskRoi(untagged));          // 경로 위지만 태그가 없다

    RoiInput item = makeInput(UserTask::Traverse, 0.5f, 0.9f);
    item.tag = ObjectTag::Item;
    EXPECT(!inTaskRoi(item));              // 아이템은 Traverse 대상이 아니다
}

// ---- Track ----
CPU_TEST(TaskRoi_TrackIsEnemyBoxOnly)
{
    RoiInput in = makeInput(UserTask::Track, 0.5f, 0.5f);
    in.enemyValid = true;
    in.enemyBBoxUV[0] = 0.4f; in.enemyBBoxUV[1] = 0.4f;
    in.enemyBBoxUV[2] = 0.6f; in.enemyBBoxUV[3] = 0.6f;
    EXPECT(inTaskRoi(in));

    in.u = 0.7f;
    EXPECT(!inTaskRoi(in));                // 박스 밖

    in.u = 0.5f; in.v = 0.9f; in.enemyCentroidUV[0] = 0.5f; in.enemyCentroidUV[1] = 0.5f;
    EXPECT(!inTaskRoi(in));                // 회랑 안이어도 Track 은 박스만 쓴다
}

CPU_TEST(TaskRoi_TrackNeedsVisibleEnemy)
{
    RoiInput in = makeInput(UserTask::Track, 0.5f, 0.5f);
    in.enemyValid = false;
    in.enemyBBoxUV[0] = 0.4f; in.enemyBBoxUV[1] = 0.4f;
    in.enemyBBoxUV[2] = 0.6f; in.enemyBBoxUV[3] = 0.6f;
    EXPECT(!inTaskRoi(in));
}

// ---- Flick ----
CPU_TEST(TaskRoi_FlickIsBoxPlusCorridorFromAimToTarget)
{
    RoiInput base = makeInput(UserTask::Flick, 0.65f, 0.5f);
    base.enemyValid = true;
    base.enemyCentroidUV[0] = 0.8f;
    base.enemyCentroidUV[1] = 0.5f;
    base.enemyBBoxUV[0] = 0.78f; base.enemyBBoxUV[1] = 0.48f;
    base.enemyBBoxUV[2] = 0.82f; base.enemyBBoxUV[3] = 0.52f;

    EXPECT(inTaskRoi(base));               // 조준점과 적 중심을 잇는 선 위

    RoiInput target = base; target.u = 0.8f;
    EXPECT(inTaskRoi(target));             // 적 바운딩 박스 안

    RoiInput offLine = base; offLine.v = 0.6f;
    EXPECT(!inTaskRoi(offLine));           // 회랑 반너비(0.04 높이 단위)보다 멀다

    RoiInput beyond = base; beyond.u = 0.95f;
    EXPECT(!inTaskRoi(beyond));            // 선분 끝(적)을 넘어가면 회랑이 아니다

    RoiInput behindAim = base; behindAim.u = 0.3f;
    EXPECT(!inTaskRoi(behindAim));         // 조준점 뒤쪽도 회랑이 아니다
}

CPU_TEST(TaskRoi_FlickNeedsVisibleEnemy)
{
    RoiInput in = makeInput(UserTask::Flick, 0.65f, 0.5f);
    in.enemyValid = false;
    in.enemyCentroidUV[0] = 0.8f;
    in.enemyCentroidUV[1] = 0.5f;
    EXPECT(!inTaskRoi(in));
}

// 비교전 task 에서는 적 정보가 있어도 적 영역을 쓰지 않는다.
CPU_TEST(TaskRoi_CombatRegionsDoNotLeakIntoHold)
{
    RoiInput in = makeInput(UserTask::Hold, 0.8f, 0.5f);
    in.enemyValid = true;
    in.enemyBBoxUV[0] = 0.78f; in.enemyBBoxUV[1] = 0.48f;
    in.enemyBBoxUV[2] = 0.82f; in.enemyBBoxUV[3] = 0.52f;
    EXPECT(!inTaskRoi(in));
}
} // namespace Falcor
