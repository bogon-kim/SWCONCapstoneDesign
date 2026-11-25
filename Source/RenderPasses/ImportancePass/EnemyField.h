#pragma once
/***************************************************************************
 # EnemyField.h  (spec_TSK.md R-5.2 ~ R-5.4)
 # 적 영역 맵 I_enemy 의 CPU 기준 구현. Falcor 의존성이 없다.
 #   R-5.2 감쇠:   마스크 안 1, 밖 0.5 · exp(−(d/σ)²). d 는 경계 바깥까지 거리(px)
 #   거리 d:       Jump Flooding (JFA). EnemyField.cs.slang 과 같은 알고리즘이다
 #   R-5.4 α 램프: 적이 보이면 0 -> alphaMax 를 rampSec 동안 선형으로 올리고, 사라지면 같은 시간으로 내린다
 ***************************************************************************/
#include <cstdint>
#include <vector>

// R-5.2 감쇠. d 가 무한대(도달 안 함)이면 0이다.
float enemyFalloff(bool inside, float d, float sigma);

// JFA 단계 크기. 최대 단계가 3σ 이상이 되도록 2의 거듭제곱을 고르고, 1까지 절반씩 줄인다.
// 예: σ = 10.8 px -> {64, 32, 16, 8, 4, 2, 1}
std::vector<uint32_t> jfaSteps(float sigmaPx);

// JFA 로 마스크(0 아님 = 적)에서 가장 가까운 적까지 거리(px)를 구한다. 적이 없으면 무한대.
// steps 는 jfaSteps() 가 준 값이다. mask 크기는 w * h 이다.
std::vector<float> jfaDistance(const std::vector<uint8_t>& mask, uint32_t w, uint32_t h, const std::vector<uint32_t>& steps);

struct AlphaRampParams
{
    float alphaMax = 0.4f; // R-5.3 α (0 < α < 1)
    float rampSec  = 0.2f; // R-5.4 램프 시간 (초, 벽시계 기준)
};

// R-5.4: 한 프레임 동안 α 를 목표값(보이면 alphaMax, 아니면 0) 쪽으로 dt 만큼 이동한다.
float updateAlphaRamp(float alpha, bool enemyVisible, float dtSec, const AlphaRampParams& params);
