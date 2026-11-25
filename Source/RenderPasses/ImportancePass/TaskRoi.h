#pragma once
/***************************************************************************
 # TaskRoi.h  (spec_TSK.md R-5.1)
 # task 별 ROI 템플릿의 CPU 기준 구현. Falcor 의존성이 없다.
 # ImportancePass.cs.slang 의 inTaskRoi() 와 같은 식을 써야 한다. 테스트는 이 기준으로 검사한다.
 #   Hold     : 조준점 중심 원
 #   Scan     : 회전 방향으로 치우친 수평 타원 띠
 #   Traverse : 중심 원 + 진행 경로(세로 띠) 위의 Wall / Floor
 #   Track    : 적 바운딩 박스
 #   Flick    : 적 바운딩 박스 + 조준점에서 적 중심까지의 회랑
 # 원·타원·회랑은 종횡비를 반영해 높이 단위로 계산한다. 바운딩 박스만 UV 단위다.
 ***************************************************************************/
#include "TagMapping.h"
#include "TaskClassifier.h"

struct RoiInput
{
    UserTask task = UserTask::Hold;
    float    u = 0.f, v = 0.f;                 // 픽셀 UV (0..1, 위쪽이 0)
    float    aspect = 16.f / 9.f;              // 가로 / 세로
    ObjectTag tag = ObjectTag::None;           // 이 픽셀의 태그 (tagMap)
    float    aimUV[2] = {0.5f, 0.5f};          // 조준점 (UV)
    float    centerRadiusUV = 0.2f;            // Hold 원 반지름 (높이 단위)
    float    scanDirX = 0.f;                   // Scan 치우침 -1(왼쪽) .. +1(오른쪽)
    float    bandHalfHeightUV = 0.15f;         // Scan 띠 반높이 (높이 단위)
    float    pathHalfWidthUV = 0.15f;          // Traverse 경로 반너비 (높이 단위)
    float    corridorHalfWidthUV = 0.04f;      // Flick 회랑 반너비 (높이 단위)
    bool     enemyValid = false;               // 적이 가시인지
    float    enemyBBoxUV[4] = {};              // x0, y0, x1, y1 (UV, 최근 readback)
    float    enemyCentroidUV[2] = {};          // 적 중심 (UV, 평활화 후)
};

// 이 픽셀이 현재 task 의 ROI 안에 있는지 판정한다.
bool inTaskRoi(const RoiInput& in);
