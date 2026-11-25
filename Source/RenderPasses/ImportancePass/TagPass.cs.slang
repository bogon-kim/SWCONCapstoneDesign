/***************************************************************************
 # TagPass.cs.slang  (spec_TSK.md R-2.1 ~ R-2.3)
 # 픽셀 태그 맵을 만든다. ImportancePass.cs.slang 보다 먼저 실행된다.
 #   identifyMode 0 (기본): objectID -> 인스턴스 태그 표
 #   identifyMode 1 (비교): 알베도 붉은색 판정 (적 = 1, 나머지 = 0)
 # 상수 버퍼는 ImportancePass.cs.slang 과 같은 레이아웃이어야 한다 (C++ ImportanceCB).
 ***************************************************************************/
#ifndef HAS_DIFFUSE_OPACITY
#define HAS_DIFFUSE_OPACITY 0
#endif
#ifndef HAS_OBJ_ID
#define HAS_OBJ_ID 0
#endif

cbuffer CB
{
    uint2  frameDim;
    float2 invFrameDim;
    uint   activeTask;
    uint   debugView;
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
    float  pathHalfWidthUV;
    float  enemyGreenMaxRatio;
    float  _pad2;
    float  _pad3;
    float4 enemyBBox;
    float2 enemyCentroidUV;
    uint   enemyValid;
    uint   identifyMode;        // 0 태그, 1 색상
    float  scanDirX;
    float  bandHalfHeightUV;
    float  corridorHalfWidthUV;
    uint   instanceCount;       // 태그 표 항목 수
    float  enemyAlpha;
    float  _pad4;
    float  _pad5;
    float  _pad6;
};

Texture2D<float4> gDiffuseOpacity;
Texture2D<uint>   gObjectID;
StructuredBuffer<uint> gInstanceTag;   // 인스턴스 인덱스 -> 태그 (CPU 가 씬 로드 때 만든다, R-2.2)
RWTexture2D<uint> gTagOut;             // 0 None, 1 Enemy, 2 Item, 3 Wall, 4 Floor

// objectID 가 범위 밖(miss 포함)이면 None
uint lookupTag(uint objectID)
{
    return (objectID < instanceCount) ? gInstanceTag[objectID] : 0u;
}

// 붉은색 적 판정 (알베도 기준, 비교 모드용)
bool isEnemyColor(float3 c)
{
    float maxC = max(c.r, max(c.g, c.b)) + 1e-6f;
    return (c.r > enemyRedThreshold) && ((c.r / maxC) > enemyRedDominance) && (c.g < (c.r * enemyGreenMaxRatio));
}

[numthreads(16, 16, 1)]
void tagMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint2 pixel = dispatchThreadId.xy;
    if (any(pixel >= frameDim)) return;

    uint tag = 0u;
    if (identifyMode == 0u)
    {
#if HAS_OBJ_ID
        tag = lookupTag(gObjectID[pixel]);
#endif
    }
    else
    {
#if HAS_DIFFUSE_OPACITY
        tag = isEnemyColor(gDiffuseOpacity[pixel].rgb) ? 1u : 0u;
#endif
    }
    gTagOut[pixel] = tag;
}
