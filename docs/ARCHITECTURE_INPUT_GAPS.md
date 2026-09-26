# 핵심 그래프·자연 입력 구현의 근거 경계

현재 상태 갱신: 사용자가 설계를 위임했고 Codex 데스크톱을 첫 대상으로 여러 에이전트를
지원하도록 지정했다. 아래 선택 대기는 해소됐다. 구현 방향은 DELEGATED_DESIGN.md를 따른다.


확인 대상은 사용자 원본 SWEGCA-Architecture의
5901a5aa2dcbd0ac7ad12ac6dd745699f72288a8 커밋이다. 폐기된 C++ 후보는 참조하지 않았다.

## 실제 원본에서 확인한 것

- paper/swegca/ARCHITECTURE_SPEC.md §4.2는 hot address/semantic-key index와
  runtime cognition의 후보 판단을 구분한다. 접근 가능함은 내용 검증이나 권한이 아니다.
- src/swegca/mosaic_unrestricted_experience.py:build_hot_experience_index는
  파일 경로에서 추출한 키와 호출자가 제공한 semantic_keys_by_address를 색인한다.
- 같은 파일의 select_experience_for_cognition은 query 토큰으로 후보를 찾고,
  못 찾으면 전체 주소로 fallback한다. 각 후보의 의미 판단은 인자로 받은
  judge(artifact, context)가 반환한다. 이 함수 자체가 자연어를 SWEGCA의
  support/refute/insufficient 관측으로 변환하는 구현은 아니다.
- 현재 사용자 지시는 모든 경험을 한번에 불러오는 방식과 외부 대체 판단기를 금지한다.
  위 원본의 whole-universe fallback이나 미지정 judge를 그대로 새 규칙으로 채택하지 않는다.
- 확인한 아키텍처 명세와 VRS 논문은 소유권·반증·3상·4단계 계약을 기술하지만,
  region/portal 분할·연결의 구체적 구성 규칙을 제공하지 않는다. 이 확인을 모든 개인
  원본 파일에 그런 규칙이 없다는 주장으로 확대하지 않는다.

## 현재 C++와의 차이

SessionRuntime::retain_input은 원문 cue를 연결 identity로 삼고 미판정 관측을 기록한다.
임의의 자연어 참/거짓을 만들지 않는 보존 경로다. ExperienceRouter의 현재 정확 키 및
직전 Replay 연결 조회는 의미적 region/portal 그래프와 동일한 기능이 아니다.
Re-evidence는 기록된 같은 연결의 관측을 검증한다. 일반 자연어 의미 판단과 자동
재검증 진입의 구현 증거가 아니다.

이미 구현한 실제 셔플→코어 3상→동일 연결 강도, 임시 우선, 명시적 종료 후 Main 병합,
원경험 보존·복구와 메모리 개선을 이 빠진 기능의 대체 완료로 처리하지 않는다.

## 남아 있는 입력

1. region/portal 분할·연결과 현재 자연 입력에서 SWEGCA 검증값을 구성하는 사용자
   원본 설계 파일 경로 또는 명시적 규칙. 기존 셔플·3상 설명을 다시 요청하는 것이 아니다.
2. 실제 연결 대상: 현재 Codex 데스크톱 앱 또는 별도 app-server 클라이언트.
   서로 다른 대상이며 기존 훅 제약과 명시적 세션 종료 판별 문제는 CODEX_INPUT_BOUNDARY.md에 있다.

해당 근거 없이 외부 judge/분류기를 발명하거나 다른 클라이언트를 대신 설치하지 않는다.
장기목표의 범위를 축소하거나 완료로 표시하지 않는다. 확인 질문은 사용자에게 전달했다.
