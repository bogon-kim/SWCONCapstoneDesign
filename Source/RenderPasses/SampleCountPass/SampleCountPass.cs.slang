/***************************************************************************
 # SampleCountPass.cs.slang  (spec_TSK.md R-5.5)
 #
 # 픽셀별 spp 배분. SppAllocation.cpp 와 같은 알고리즘이다.
 #   w_p   = ε + I_p
 #   x_p   = clamp(s · w_p, 1, maxSpp),  s 는 Σ x_p = B 가 되도록 정한다
 #   spp_p = clamp(floor(x_p) + δ_p, 1, maxSpp),  Σ δ_p = B − Σ floor(x_p)
 #
 # [5 단계 dispatch]
 #   1. histMain  : w 의 히스토그램 (W_BINS 개)
 #   2. solveMain : s 를 이분법으로 구한다 (1 스레드)
 #   3. baseMain  : base = floor(x), Σbase, 소수 부분 히스토그램 (+1 가능 / −1 가능)
 #   4. deltaMain : L = B − Σbase 를 어느 소수 bin 까지 ±1 할지 정한다 (1 스레드)
 #   5. finalMain : spp = base + δ 를 쓴다. 같은 bin 의 픽셀 중 몇 개만 ±1 할지는 atomic 카운터로 고른다
 ***************************************************************************/
#ifndef HAS_SAMPLE_COUNT_VIS
#define HAS_SAMPLE_COUNT_VIS 0
#endif

#define W_BINS      1024
#define F_BINS      256
#define BISECT_ITERS 32
#define HIST_ADD    W_BINS                 // +1 가능 픽셀의 소수 bin
#define HIST_SUB    (W_BINS + F_BINS)      // −1 가능 픽셀의 소수 bin
#define HIST_SUMBASE (W_BINS + 2 * F_BINS) // Σ base
#define HIST_SIZE   (W_BINS + 2 * F_BINS + 1)

cbuffer CB
{
    uint2  frameDim;   // 해상도
    uint   budget;     // B = sTarget · 픽셀 수
    uint   maxSpp;     // 픽셀당 최대 spp (16)
    float  epsilon;    // ε
    float  wMax;       // 1 + ε (가중치 최대값, 히스토그램 범위)
    uint   _p0;
    uint   _p1;
};

Texture2D<float>    gImportance;     // I_p (ImportancePass 출력)
RWTexture2D<uint>   gOutput;         // spp (R8Uint). baseMain 에서 base 를 먼저 쓰고 finalMain 에서 고친다
RWTexture2D<float4> gSampleCountVis; // 시각화 (HAS_SAMPLE_COUNT_VIS 일 때만 쓴다)
RWStructuredBuffer<uint> gHist;      // HIST_SIZE 개. 매 프레임 0 으로 지운다
RWStructuredBuffer<float> gScale;    // [0] = s
RWStructuredBuffer<int>   gDelta;    // [0] mode(+1,−1,0)  [1] thr  [2] rem  [3] ticket (매 프레임 0)

uint weightBin(float w)
{
    float t = w / wMax * float(W_BINS);
    return min(uint(max(t, 0.f)), uint(W_BINS - 1));
}

uint fracBin(float f)
{
    return min(uint(max(f * float(F_BINS), 0.f)), uint(F_BINS - 1));
}

float weightOf(uint2 p)
{
    return epsilon + saturate(gImportance[p]);
}

// ---- 1. 가중치 히스토그램 -------------------------------------------------
[numthreads(16, 16, 1)]
void histMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint2 p = dispatchThreadId.xy;
    if (any(p >= frameDim)) return;
    InterlockedAdd(gHist[weightBin(weightOf(p))], 1u);
}

// ---- 2. s 찾기 -------------------------------------------------------------
// F(s) = Σ count_b · clamp(s · rep_b, 1, maxSpp)  (rep_b 는 bin 중심)
float evalF(float sc)
{
    float sum = 0.f;
    for (uint b = 0u; b < uint(W_BINS); b++)
    {
        uint c = gHist[b];
        if (c == 0u) continue;
        float rep = (float(b) + 0.5f) / float(W_BINS) * wMax;
        sum += float(c) * clamp(sc * rep, 1.f, float(maxSpp));
    }
    return sum;
}

[numthreads(1, 1, 1)]
void solveMain()
{
    uint n = frameDim.x * frameDim.y;
    float s;
    if (budget <= n)
    {
        s = 0.f;                                   // 예산이 최소치 이하: 전부 1 spp
    }
    else if (budget >= n * maxSpp)
    {
        s = float(maxSpp) / epsilon * 2.f;         // 예산이 최대치 이상: 전부 maxSpp
    }
    else
    {
        float lo = 0.f;
        float hi = float(maxSpp) / epsilon;        // F(hi) = maxSpp · n ≥ B
        for (uint it = 0u; it < uint(BISECT_ITERS); it++)
        {
            float mid = 0.5f * (lo + hi);
            if (evalF(mid) < float(budget)) lo = mid;
            else                            hi = mid;
        }
        s = hi;
    }
    gScale[0] = s;
}

// ---- 3. 정수 부분과 소수 부분 히스토그램 -----------------------------------
[numthreads(16, 16, 1)]
void baseMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint2 p = dispatchThreadId.xy;
    if (any(p >= frameDim)) return;

    float x = clamp(gScale[0] * weightOf(p), 1.f, float(maxSpp));
    uint base = uint(floor(x));
    gOutput[p] = base;
    InterlockedAdd(gHist[HIST_SUMBASE], base);

    uint fb = fracBin(x - float(base));
    if (base < maxSpp) InterlockedAdd(gHist[HIST_ADD + fb], 1u);   // +1 을 받을 수 있다
    if (base > 1u)     InterlockedAdd(gHist[HIST_SUB + fb], 1u);   // −1 을 받을 수 있다
}

// ---- 4. 보정 기준 정하기 (1 스레드) ----------------------------------------
[numthreads(1, 1, 1)]
void deltaMain()
{
    int L = int(budget) - int(gHist[HIST_SUMBASE]);
    int mode = 0;
    int thr = 0;
    int rem = 0;
    if (L > 0)
    {
        // 소수 부분이 큰 쪽부터 +1. 가능한 픽셀이 부족하면 thr = −1 (전부 +1)
        mode = 1;
        thr = -1;
        int cum = 0;
        for (int t = F_BINS - 1; t >= 0; t--)
        {
            int c = int(gHist[HIST_ADD + uint(t)]);
            if (cum + c >= L)
            {
                thr = t;
                rem = L - cum;
                break;
            }
            cum += c;
        }
    }
    else if (L < 0)
    {
        // 소수 부분이 작은 쪽부터 −1. 가능한 픽셀이 부족하면 thr = F_BINS (전부 −1)
        mode = -1;
        int need = -L;
        int cum = 0;
        thr = F_BINS;
        for (int t = 0; t < F_BINS; t++)
        {
            int c = int(gHist[HIST_SUB + uint(t)]);
            if (cum + c >= need)
            {
                thr = t;
                rem = need - cum;
                break;
            }
            cum += c;
        }
    }
    gDelta[0] = mode;
    gDelta[1] = thr;
    gDelta[2] = rem;
    gDelta[3] = 0;
}

// ---- 5. 최종 spp ------------------------------------------------------------
[numthreads(16, 16, 1)]
void finalMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint2 p = dispatchThreadId.xy;
    if (any(p >= frameDim)) return;

    int base = int(gOutput[p]);
    float x = clamp(gScale[0] * weightOf(p), 1.f, float(maxSpp));
    int fb = int(fracBin(x - floor(x)));

    int mode = gDelta[0];
    int thr = gDelta[1];
    int rem = gDelta[2];
    int v = base;
    if (mode > 0 && base < int(maxSpp))
    {
        if (fb > thr) v += 1;
        else if (fb == thr)
        {
            int ticket;
            InterlockedAdd(gDelta[3], 1, ticket);
            if (ticket < rem) v += 1;
        }
    }
    else if (mode < 0 && base > 1)
    {
        if (fb < thr) v -= 1;
        else if (fb == thr)
        {
            int ticket;
            InterlockedAdd(gDelta[3], 1, ticket);
            if (ticket < rem) v -= 1;
        }
    }
    v = clamp(v, 1, int(maxSpp));
    gOutput[p] = uint(v);

    // [디버그 시각화] 1spp → 파랑, 16spp → 빨강 (파랑→시안→초록→노랑→빨강)
#if HAS_SAMPLE_COUNT_VIS
    float t = saturate(float(v - 1) / 15.f);
    float u = t * 4.f;
    float3 heatCol;
    if      (u < 1.f) heatCol = lerp(float3(0,0,1), float3(0,1,1), u);
    else if (u < 2.f) heatCol = lerp(float3(0,1,1), float3(0,1,0), u - 1.f);
    else if (u < 3.f) heatCol = lerp(float3(0,1,0), float3(1,1,0), u - 2.f);
    else              heatCol = lerp(float3(1,1,0), float3(1,0,0), u - 3.f);
    gSampleCountVis[p] = float4(heatCol, 1.f);
#endif
}
