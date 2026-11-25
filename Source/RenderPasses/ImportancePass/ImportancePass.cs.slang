/***************************************************************************
 # ImportancePass.cs.slang
 # 1단계: max(albedoVar, geoNormVar, detailNormVar, metalness, glossiness)
 # 2단계: task override -> center weight -> luminance penalty -> motion penalty
 # 적 마스크 통계: 붉은색 알베도 픽셀 수·좌표 합·조준점 적중 (CPU 비동기 readback 용)
 ***************************************************************************/
#ifndef HAS_DIFFUSE_OPACITY
#define HAS_DIFFUSE_OPACITY 0
#endif
#ifndef HAS_GUIDE_NORMAL
#define HAS_GUIDE_NORMAL 0
#endif
#ifndef HAS_FACE_NORMAL
#define HAS_FACE_NORMAL 0
#endif
#ifndef HAS_SPEC_ROUGH
#define HAS_SPEC_ROUGH 0
#endif
#ifndef HAS_MOTION_VECTOR_W
#define HAS_MOTION_VECTOR_W 0
#endif
#ifndef HAS_LINEAR_Z
#define HAS_LINEAR_Z 0
#endif
#ifndef HAS_SHADOW_COUNT
#define HAS_SHADOW_COUNT 0
#endif
#ifndef HAS_LUMINANCE
#define HAS_LUMINANCE 0
#endif
#ifndef HAS_CENTER_DIST
#define HAS_CENTER_DIST 0
#endif
#ifndef HAS_METALNESS
#define HAS_METALNESS 0
#endif
#ifndef HAS_IMPORTANCE_VIS
#define HAS_IMPORTANCE_VIS 0
#endif

cbuffer CB
{
    uint2  frameDim;
    float2 invFrameDim;
    uint   activeTask;          // 0:Hold 1:Scan 2:Traverse 3:Track 4:Flick
    uint   debugView;           // 0:Imp 1:TaskOverlay 2:ImgOnly 3:TaskOnly
    uint   hasImportanceVis;
    uint   _pad0;
    float2 centerUV;
    float  centerRadiusUV;
    float  taskImportance;
    float  enemyRedThreshold;
    float  enemyRedDominance;
    float  lumLow;
    float  lumHigh;
    float  motionA;
    float  motionB;
    float  motionC;
    float  centerWeightMin;
    float  pathHalfWidthUV;     // Traverse 경로 반너비 (높이 단위)
    float  enemyGreenMaxRatio;
    float  _pad2;
    float  _pad3;
    float4 enemyBBox;           // 96: 적 바운딩 박스 x0 y0 x1 y1 (UV, 최근 readback)
    float2 enemyCentroidUV;     // 112: 적 중심 (UV, 평활화 후)
    uint   enemyValid;          // 120: 적이 가시이면 1
    uint   identifyMode;        // 124: 0 태그 기반, 1 색상 기반 (비교용)
    float  scanDirX;            // 128: 회전 방향 -1..+1
    float  bandHalfHeightUV;    // 132: Scan 띠 반높이 (높이 단위)
    float  corridorHalfWidthUV; // 136: Flick 회랑 반너비 (높이 단위)
    uint   instanceCount;       // 140: 태그 표 항목 수 (TagPass 용)
    float  enemyAlpha;          // 144: R-5.4 α (램프 적용 값)
    float  _pad4;
    float  _pad5;
    float  _pad6;
};

Texture2D<float4> gDiffuseOpacity;
Texture2D<float4> gGuideNormalW;
Texture2D<float4> gFaceNormalW;
Texture2D<float4> gSpecRough;
Texture2D<float4> gMotionVectorW;
Texture2D<float4> gLinearZ;
Texture2D<uint>   gShadowCount;
Texture2D<uint>   gTagIn;       // 태그 맵 (TagPass 출력, R-2.3)
Texture2D<float>  gEnemyRoi;    // 적 영역 맵 I_enemy (EnemyField 출력, R-5.2)
Texture2D<float>  gLuminanceTex;
Texture2D<float>  gCenterDist;
Texture2D<float>  gMetalness;

RWTexture2D<float>  gImportance;
RWTexture2D<float4> gImportanceVis;
// [0] 적 픽셀 수 [1] x 좌표 합 [2] y 좌표 합 [3] 조준점 픽셀이 적이면 1
// [4] 적 x 최대 [5] 적 y 최대 [6] (W - x) 최대 [7] (H - y) 최대 -> 바운딩 박스 (x 최소 = W - [6])
// 좌표 합은 uint32 라서 적 픽셀이 약 200만 개(1080p 전체)를 넘으면 넘칠 수 있다.
RWStructuredBuffer<uint> gEnemyStats;

// ---------- helpers ----------
int2 clampP(int2 p)
{
    return clamp(p, int2(0, 0), int2(frameDim) - int2(1, 1));
}

float lum601(float3 c) { return dot(c, float3(0.299f, 0.587f, 0.114f)); }

float3 sampleAlbedo(int2 p)
{
#if HAS_DIFFUSE_OPACITY
    return gDiffuseOpacity[clampP(p)].rgb;
#else
    return float3(0.f, 0.f, 0.f);
#endif
}

float3 sampleGuideN(int2 p)
{
#if HAS_GUIDE_NORMAL
    return gGuideNormalW[clampP(p)].xyz;
#else
    return float3(0.f, 0.f, 1.f);
#endif
}

float3 sampleFaceN(int2 p)
{
#if HAS_FACE_NORMAL
    return gFaceNormalW[clampP(p)].xyz;
#else
    return float3(0.f, 0.f, 1.f);
#endif
}

float sampleRough(int2 p)
{
#if HAS_SPEC_ROUGH
    return saturate(gSpecRough[clampP(p)].a);
#else
    return 1.f;
#endif
}

float sampleMet(int2 p)
{
#if HAS_METALNESS
    return saturate(gMetalness[clampP(p)]);
#else
    return 0.f;
#endif
}

float sampleLum(int2 p)
{
#if HAS_LUMINANCE
    return gLuminanceTex[clampP(p)];
#elif HAS_DIFFUSE_OPACITY
    return lum601(gDiffuseOpacity[clampP(p)].rgb);
#else
    return 0.f;
#endif
}

float sampleMotion(int2 p)
{
#if HAS_MOTION_VECTOR_W
    return length(gMotionVectorW[clampP(p)].xyz);
#else
    return 0.f;
#endif
}

float sampleCDist(int2 p)
{
#if HAS_CENTER_DIST
    return gCenterDist[clampP(p)];
#else
    // float2 uv = (float2(p) + 0.5f) * invFrameDim;
    // return length(uv - float2(0.5f, 0.5f));

    float2 uv = (float2(p) + 0.5f) * invFrameDim;
    float2 diff = uv - float2(0.5f, 0.5f);
    float aspect = float(frameDim.x) / float(frameDim.y); // 예: 1920/1080 ≈ 1.778
    diff.x *= aspect;                                     // X축 거리를 픽셀 비율로 보정
    return length(diff);
#endif
}

// 3x3 scalar variance (population)
float scalarVar3x3(int2 p, bool useAlbedo)
{
    float s = 0.f;
    float ss = 0.f;
    [unroll]
    for (int dy = -1; dy <= 1; dy++)
    {
        [unroll]
        for (int dx = -1; dx <= 1; dx++)
        {
            float v = useAlbedo
                ? lum601(sampleAlbedo(p + int2(dx, dy)))
                : sampleMet(p + int2(dx, dy));
            s  += v;
            ss += v * v;
        }
    }
    float mean = s / 9.f;
    float var  = max(ss / 9.f - mean * mean, 0.f);
    return saturate(var * 4.f); // scale to [0,1] range
}

// 3x3 normal variance: mean(1 - |dot(n_i, n_center)|)
float normalVar3x3(int2 p, bool useGuide)
{
    float3 nc = useGuide ? sampleGuideN(p) : sampleFaceN(p);
    nc = normalize(nc + float3(0.f, 0.f, 1e-6f)); // guard zero-length
    float acc = 0.f;
    [unroll]
    for (int dy = -1; dy <= 1; dy++)
    {
        [unroll]
        for (int dx = -1; dx <= 1; dx++)
        {
            if (dx == 0 && dy == 0) continue;
            float3 ni = useGuide
                ? sampleGuideN(p + int2(dx, dy))
                : sampleFaceN(p + int2(dx, dy));
            ni = normalize(ni + float3(0.f, 0.f, 1e-6f));
            acc += 1.f - abs(dot(nc, ni));
        }
    }
    return saturate(acc / 8.f);
}


// [히트맵] 스칼라 값 t ∈ [0,1] 을 파랑→초록→빨강 으로 매핑한다.
float3 heatmap(float t)
{
    t = saturate(t);
    return (t < 0.5f)
        ? lerp(float3(0.f, 0.f, 1.f), float3(0.f, 1.f, 0.f), t * 2.f)        // 파랑 → 초록
        : lerp(float3(0.f, 1.f, 0.f), float3(1.f, 0.f, 0.f), t * 2.f - 1.f); // 초록 → 빨강
}

// task 별 ROI 판정 (R-5.1). TaskRoi.cpp 의 inTaskRoi() 와 같은 식이어야 한다.
// 원·타원·회랑은 종횡비를 반영해 높이 단위로 계산한다. 바운딩 박스만 UV 단위다.
bool inTaskRoi(float2 uv, uint tag)
{
    float aspect = float(frameDim.x) / float(frameDim.y);
    float2 q = float2(uv.x * aspect, uv.y);
    float2 c = float2(centerUV.x * aspect, centerUV.y);
    float2 dc = q - c;
    bool inCircle = length(dc) <= centerRadiusUV;

    if (activeTask == 0u) return inCircle; // Hold
    if (activeTask == 1u)                  // Scan: 회전 방향으로 치우친 수평 타원
    {
        float ecx = (0.5f + 0.25f * scanDirX) * aspect;
        float2 d = (q - float2(ecx, 0.5f)) / float2(0.5f * aspect, bandHalfHeightUV);
        return dot(d, d) <= 1.f;
    }
    if (activeTask == 2u)                  // Traverse: 중심 원 + 경로 위 Wall / Floor
    {
        if (inCircle) return true;
        bool onPath = abs(q.x - 0.5f * aspect) <= pathHalfWidthUV;
        return onPath && (tag == 3u || tag == 4u);
    }

    // Track / Flick: 적이 가시일 때만
    if (enemyValid == 0u) return false;
    bool inBox = all(uv >= enemyBBox.xy) && all(uv <= enemyBBox.zw);
    if (inBox) return true;
    if (activeTask != 4u) return false;    // Track 은 박스만

    // Flick: 조준점 -> 적 중심 선분에서 반너비 이내 (선분 끝을 넘지 않는다)
    float2 b = float2(enemyCentroidUV.x * aspect, enemyCentroidUV.y);
    float2 ab = b - c;
    float len2 = dot(ab, ab);
    float t = (len2 > 1e-12f) ? saturate(dot(q - c, ab) / len2) : 0.f;
    return length(q - (c + t * ab)) <= corridorHalfWidthUV;
}

// 그룹 단위 적 마스크 통계 (공유 메모리에서 모은 뒤 그룹당 global atomic 으로 기록)
groupshared uint gsEnemyCount;
groupshared uint gsEnemySumX;
groupshared uint gsEnemySumY;
groupshared uint gsMaxX;
groupshared uint gsMaxY;
groupshared uint gsMaxInvX;
groupshared uint gsMaxInvY;

[numthreads(16, 16, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID, uint groupIndex : SV_GroupIndex)
{
    uint2 pixel = dispatchThreadId.xy;
    int2 p = int2(pixel);
    float2 uv = (float2(pixel) + 0.5f) * invFrameDim;

    // ---- 적 마스크 통계 (gEnemyStats 는 매 프레임 0 으로 초기화된다) ----
    // 배리어 때문에 화면 밖 스레드도 여기까지는 함께 실행한 뒤 반환한다.
    bool inside  = all(pixel < frameDim);
    // 픽셀 태그는 TagPass 가 만들었다 (R-2.3). 적 마스크는 그 태그에서 나온다.
    uint pixelTag = inside ? gTagIn[pixel] : 0u;
    bool isEnemy = pixelTag == 1u;

    if (groupIndex == 0u)
    {
        gsEnemyCount = 0u;
        gsEnemySumX  = 0u;
        gsEnemySumY  = 0u;
        gsMaxX       = 0u;
        gsMaxY       = 0u;
        gsMaxInvX    = 0u;
        gsMaxInvY    = 0u;
    }
    GroupMemoryBarrierWithGroupSync();
    if (isEnemy)
    {
        InterlockedAdd(gsEnemyCount, 1u);
        InterlockedAdd(gsEnemySumX, pixel.x);
        InterlockedAdd(gsEnemySumY, pixel.y);
        InterlockedMax(gsMaxX, pixel.x);
        InterlockedMax(gsMaxY, pixel.y);
        InterlockedMax(gsMaxInvX, frameDim.x - pixel.x);
        InterlockedMax(gsMaxInvY, frameDim.y - pixel.y);
    }
    GroupMemoryBarrierWithGroupSync();
    if (groupIndex == 0u && gsEnemyCount > 0u)
    {
        InterlockedAdd(gEnemyStats[0], gsEnemyCount);
        InterlockedAdd(gEnemyStats[1], gsEnemySumX);
        InterlockedAdd(gEnemyStats[2], gsEnemySumY);
        InterlockedMax(gEnemyStats[4], gsMaxX);
        InterlockedMax(gEnemyStats[5], gsMaxY);
        InterlockedMax(gEnemyStats[6], gsMaxInvX);
        InterlockedMax(gEnemyStats[7], gsMaxInvY);
    }
    if (!inside) return;
    if (all(pixel == uint2(centerUV * float2(frameDim))))
        gEnemyStats[3] = isEnemy ? 1u : 0u;

    // =========================================================
    // 1단계 (1): Image-space max-variance importance
    // =========================================================
    float albedoVar  = scalarVar3x3(p, true);
    float guideNVar  = normalVar3x3(p, true);
    float faceNVar = normalVar3x3(p, false);
    float metalness = sampleMet(p);
    float glossiness = 1.f - sampleRough(p);  // rough=1 -> gloss=0

    float imageImp = max(albedoVar,
                    max(guideNVar,
                    max(faceNVar, 
                    max(metalness, glossiness))));

    // =========================================================
    // 1단계 (2): Task-Object space importance
    // =========================================================
    float taskImp = 0.f;

    // task ROI 템플릿 (R-5.1) 과 적 영역 (R-5.3): I = max(I_task, α · I_enemy)
    float taskRoi = inTaskRoi(uv, pixelTag) ? taskImportance : 0.f;
    float enemyTerm = enemyAlpha * gEnemyRoi[pixel];
    taskImp = max(taskRoi, enemyTerm);

    // ---- max(image, task) ----
    float importance = max(imageImp, taskImp);

    // =========================================================
    // 2단계 (A): Center weighting  (center=1.0, edge=min)
    //   centerDist in [0, ~0.707]
    // =========================================================
    float cd = sampleCDist(p);
    float t  = saturate(cd / 0.7071068f);
    float centerW = lerp(1.f, centerWeightMin, t);
    importance *= centerW;

    // =========================================================
    // 2단계 (B): Luminance penalty
    // =========================================================
    float L = sampleLum(p);
    if (L < lumLow)
        importance *= saturate(L / max(lumLow, 1e-4f));
    else if (L > lumHigh)
        importance *= saturate((1.f - L) / max(1.f - lumHigh, 1e-4f));



    // =========================================================
    // 2단계 (C): Motion penalty
    //   w = (1 / (1 + (A*v)^B))^C
    // =========================================================
    float v   = sampleMotion(p);
    float av  = max(motionA * v, 0.f);
    float denom = 1.f + pow(av + 1e-6f, motionB);
    float mw  = pow(max(1.f / denom, 1e-6f), motionC);
    importance *= mw;

    importance = saturate(importance);

    // ---- debug view selection ----
    float outScalar = importance;
    if (debugView == 2u)      outScalar = saturate(imageImp);
    else if (debugView == 3u) outScalar = saturate(taskImp);

    gImportance[pixel] = outScalar;

#if HAS_IMPORTANCE_VIS
    if (hasImportanceVis != 0u)
    {
        // [히트맵] 중요도 값을 파랑(낮음) → 초록(중간) → 빨강(높음) 으로 시각화
        float3 col = heatmap(outScalar);

        if (debugView == 1u) // task overlay: 히트맵 위에 태스크 tint 를 혼합
        {
            float3 tint = float3(1.f, 1.f, 1.f);
            if      (activeTask == 3u || activeTask == 4u) tint = float3(1.0f, 0.2f, 0.2f); // Track/Flick: red
            else if (activeTask == 2u) tint = float3(0.3f, 0.6f, 1.0f); // Traverse: blue
            col = lerp(col, tint, taskImp * 0.7f);
        }
        gImportanceVis[pixel] = float4(col, 1.f);
    }
#endif
}
