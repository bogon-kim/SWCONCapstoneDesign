/***************************************************************************
 # EnemyField.cs.slang  (spec_TSK.md R-5.2)
 # 적 영역 맵 I_enemy. Jump Flooding 으로 가장 가까운 적 픽셀까지 거리 d 를 구하고 감쇠를 준다.
 #   jfaInitMain : 적 픽셀(tag == 1)은 자기 자신이 seed, 나머지는 seed 없음
 #   jfaStepMain : JFA_STEP 간격의 9 이웃 중 가장 가까운 seed 를 고른다 (단계마다 한 번, define 으로 간격 지정)
 #   jfaFinalMain: 마스크 안 1, 밖 0.5 · exp(−(d/σ)²)
 # 알고리즘은 EnemyField.cpp 의 jfaDistance() 와 같다.
 ***************************************************************************/
#ifndef JFA_STEP
#define JFA_STEP 1
#endif

cbuffer FieldCB
{
    uint2 frameDim;
    float sigmaPx;     // σ = sigmaRatio · 화면 높이 (px)
    uint  _pad;
};

Texture2D<uint>    gTagIn;      // 태그 맵 (TagPass 출력)
Texture2D<uint2>   gSeedIn;     // step, final: 이전 단계의 seed
RWTexture2D<uint2> gSeedOut;    // init, step: 이번 단계의 seed
RWTexture2D<float> gEnemyRoi;   // final: I_enemy

static const uint2 kNoSeed = uint2(0xFFFFFFFFu, 0xFFFFFFFFu);

// seed 까지 거리 제곱 (seed 가 없으면 아주 큰 값)
int seedDist2(int2 p, uint2 s)
{
    if (s.x == 0xFFFFFFFFu) return 0x7fffffff;
    int2 d = p - int2(s);
    return d.x * d.x + d.y * d.y;
}

[numthreads(16, 16, 1)]
void jfaInitMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint2 p = dispatchThreadId.xy;
    if (any(p >= frameDim)) return;
    gSeedOut[p] = (gTagIn[p] == 1u) ? p : kNoSeed;
}

[numthreads(16, 16, 1)]
void jfaStepMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint2 p = dispatchThreadId.xy;
    if (any(p >= frameDim)) return;

    int2 pi = int2(p);
    int2 dim = int2(frameDim);
    uint2 best = gSeedIn[p];
    int bestD = seedDist2(pi, best);
    for (int dy = -1; dy <= 1; dy++)
    {
        for (int dx = -1; dx <= 1; dx++)
        {
            int2 q = pi + int2(dx, dy) * JFA_STEP;
            if (q.x < 0 || q.y < 0 || q.x >= dim.x || q.y >= dim.y) continue;
            uint2 cand = gSeedIn[uint2(q)];
            int d = seedDist2(pi, cand);
            if (d < bestD)
            {
                bestD = d;
                best = cand;
            }
        }
    }
    gSeedOut[p] = best;
}

[numthreads(16, 16, 1)]
void jfaFinalMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint2 p = dispatchThreadId.xy;
    if (any(p >= frameDim)) return;

    if (gTagIn[p] == 1u)
    {
        gEnemyRoi[p] = 1.f;               // 마스크 안
        return;
    }
    uint2 s = gSeedIn[p];
    if (s.x == 0xFFFFFFFFu)
    {
        gEnemyRoi[p] = 0.f;               // 적이 없다
        return;
    }
    float d = length(float2(int2(p) - int2(s)));
    float r = d / max(sigmaPx, 1e-6f);
    gEnemyRoi[p] = 0.5f * exp(-r * r);    // 마스크 밖 (R-5.2)
}
