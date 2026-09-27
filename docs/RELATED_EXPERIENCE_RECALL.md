# 원경험에서 그 관측 경험으로 이어지는 Recall

## 구현

`ExperienceRouter::related(ReplayedInput)`와 Runtime 위임 경로를 추가했다.
현재 라우터가 이미 성공적으로 Replay한 원경험의 digest로 기존 context 색인을
조회한다. 관측을 만들 때 기록한 `context=inputOriginal.digest` 관계를 사용하며,
범위 이름을 새로 생성하거나 로그 문자열을 재검색하지 않는다.

- 다른 라우터가 발급한 Replay는 거부한다.
- 임시 context 색인에 연결이 있으면 임시를 사용하고, 부재할 때만 Main을 조회한다.
- 영속 병합 Main과 발행된 세션 Main 모두 기존 context 관계를 사용한다.
- 경로 선택은 기존 recall_scope/familiarity_key, 후보 선택은 prefer_replay,
  대조와 충돌 재검증은 기존 Runtime::cognize 및 코어를 사용한다.
- 반환은 기존 InputRecall이다. 여러 후보의 원문을 먼저 읽지 않는다. 선택 뒤 Replay는
  그 원경험 하나를 읽는다. 미리 읽은 부모 원경험의 원문을 다시 읽지 않는다.
- 연결된 지지/반박/불충분 경험을 버리지 않는다. 같은 문구라도 다른 입력 원경험에
  붙은 관측은 이 관계의 후보가 아니다. 없는 관계를 일반 대화로 대체하지 않는다.
- 조회/대조로 새 관측이나 연결 강도를 추가하지 않는다.

이 관계는 관측이 어떤 입력을 대상으로 기록됐는지를 나타낸다. 그 관측이 자연어
요구 전체를 충족한다는 의미 판정이 아니다. 부모 입력의 승인으로 승격하지 않는다.

## 검증

CPU6-7 / make -j2:

- Runtime 2039 checks. 같은 입력의 두 scoped 연결(지지/반박), 다른 목적 원문 제외,
  관계 부재, 다른 라우터 거부, 실제 종료/병합/재시작 뒤 Main 조회를 검사했다.
- Session Runtime 1363 checks. 발행 세션 Main의 관계 조회와 임시 관계 추가 뒤
  임시 우선, 메모리 반환을 검사했다.
- 실제 stdio subprocess 5903 checks. 기존 원문 수집/일반 및 scoped Replay/
  private query MCP/명시적 종료 경로의 회귀를 확인했다.

작은 상주 메타데이터 fixture에서 related Recall 중 pread/pwrite 증가가 없고,
Replay/대조가 추가 관측 쓰기를 만들지 않음을 확인했다. 디스크에 퇴거된 모든
메타데이터/거대한 Main에서 I/O가 없다는 주장이나 1ms 달성 증거는 아니다.

## 남은 연결

이번 변경은 Runtime 경로다. 에이전트 MCP에서는 아직 이 관계 탐색을 요청할 수 없다.
요구 범위 이름을 잊은 뒤의 발견 기능으로 연결하려면, 부모 cognition과 독립된
비교 이력 및 복구 좌표를 보존하고 같은 선택 경험에 후속 반증을 적용해야 한다.
현재 일반/scoped 복구 채널을 덮어쓰거나 매 조회마다 관측 경계를 새로 잡아
반증을 놓치는 방식으로 연결하지 않는다. 이번 변경은 설치본에 반영하지 않았다.
