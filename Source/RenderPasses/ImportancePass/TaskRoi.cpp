/***************************************************************************
 # TaskRoi.cpp  (spec_TSK.md R-5.1)
 ***************************************************************************/
#include "TaskRoi.h"

#include <algorithm>
#include <cmath>

bool inTaskRoi(const RoiInput& in)
{
    // 높이 단위 좌표: 가로를 종횡비로 늘려 원이 화면에서도 원으로 보이게 한다.
    const float qx = in.u * in.aspect;
    const float qy = in.v;
    const float cx = in.aimUV[0] * in.aspect;
    const float cy = in.aimUV[1];

    const float dxC = qx - cx;
    const float dyC = qy - cy;
    const bool inCircle = std::sqrt(dxC * dxC + dyC * dyC) <= in.centerRadiusUV;

    switch (in.task)
    {
    case UserTask::Hold:
        return inCircle;

    case UserTask::Scan:
    {
        // 수평 타원: 반너비 0.5 (화면 가로 절반), 반높이 bandHalfHeightUV. 중심이 회전 방향으로 0.25 이동한다.
        const float ex  = 0.5f * in.aspect;
        const float ecx = (0.5f + 0.25f * in.scanDirX) * in.aspect;
        const float ux  = (qx - ecx) / ex;
        const float uy  = (qy - 0.5f) / in.bandHalfHeightUV;
        return ux * ux + uy * uy <= 1.f;
    }

    case UserTask::Traverse:
    {
        if (inCircle)
            return true;
        const bool onPath = std::fabs(qx - 0.5f * in.aspect) <= in.pathHalfWidthUV;
        return onPath && (in.tag == ObjectTag::Wall || in.tag == ObjectTag::Floor);
    }

    case UserTask::Track:
    case UserTask::Flick:
    {
        if (!in.enemyValid)
            return false;
        const bool inBox = in.u >= in.enemyBBoxUV[0] && in.v >= in.enemyBBoxUV[1] &&
                           in.u <= in.enemyBBoxUV[2] && in.v <= in.enemyBBoxUV[3];
        if (inBox)
            return true;
        if (in.task == UserTask::Track)
            return false;

        // Flick: 조준점 -> 적 중심 선분에서 반너비 이내 (선분 끝을 넘지 않는다)
        const float bx  = in.enemyCentroidUV[0] * in.aspect;
        const float by  = in.enemyCentroidUV[1];
        const float abx = bx - cx;
        const float aby = by - cy;
        const float len2 = abx * abx + aby * aby;
        const float t = len2 > 1e-12f ? std::clamp(((qx - cx) * abx + (qy - cy) * aby) / len2, 0.f, 1.f) : 0.f;
        const float px = cx + t * abx - qx;
        const float py = cy + t * aby - qy;
        return std::sqrt(px * px + py * py) <= in.corridorHalfWidthUV;
    }
    }
    return false;
}
