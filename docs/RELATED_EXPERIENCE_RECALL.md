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

2026-09-27 후속: related 관측 조회에는 기존 코어 `context_reference_eligible`과
봉인된 `has_input_key`를 적용한다. 일반 네이티브 응답은 보존하되 관측 조회의
후보로 사용하지 않는다. 임시의 자격 있는 후보가 없으면 Main으로 넘어간다.
임시/발행 세션 Main/병합 Main/재시작에 같은 자격을 적용하며 원경험 하나의
Replay 뒤에 보유한 봉인 표식으로 journal 복원 자격도 확인한다.
그 표식은 명시적인 입력 연결의 기록이며, 관측 내용의 진실성이나 전체 요구의
충족 판정이 아니다. 그 판정은 기존 SWEGCA 대조를 따른다.

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

## 에이전트 조회와 별도 비교 이력

`vrs_replay`에 `related: true`를 추가했다. 현재 입력의 receipt/inputOriginal을
확인한 뒤 자동 선택된 부모 Replay에서 관계를 따라간다. scope/candidate/부분 읽기와
동시에 지정할 수 없다. 부모 Replay가 없거나 연결된 관측이 없으면 오류를 반환하며
다른 대화나 가짜 검증 결과로 대신하지 않는다.

선택한 관측은 부모 및 생산자 scope와 다른 SHA-256 도메인의 cognition 채널에
저장한다. 선택 원경험, 부모 원경험 주소, 연결 head, 실제 context 조회 키, 관측 기준점,
source 및 처리 seed/step을 보존한다. 후속 관측 ACK 전에 기존 코어 대조를 갱신하고
충돌일 때만 Re-evidence한다. 부모 cognition을 이 결과로 덮어쓰지 않는다.

재시작 시 실제 입력/부모 원경험/저장 lookup 관계를 확인하고 기존 Runtime 복구를
사용한다. 다시 조회할 때 관측 기준점을 최신 위치로 옮겨 반증을 지우지 않는다.
변경 없는 반복 조회는 저장량과 개정이 같으며 임시 및 Main 선택 모두 복구된다.
첫 Main 조회에도 해당 연결의 최신 처리 단계를 반영한다. 이를 빠뜨리면 재시작 시
step이 불필요하게 달라지는 문제를 발견하여 수정했다.

후보 선택을 먼저 한 경우 Runtime::cognize_selected가 그 선택의 Replay/대조를
조합한다. 후보 배열을 두 번 순회하지 않는다. 기존 cognize도 이 동일 경로로 위임한다.
호스트의 `swegca/agent/cognition`은 related=true와 latest/revision으로 별도 이력을
조회할 수 있다. 이 관리 조회는 에이전트 소켓에 노출하지 않는다.

후속 검증: Runtime2039 / 실제 stdio6368 / 관측기129 checks 통과. 통합 검사에는
실제 MCP 소켓 왕복, 부모 판정 불변, 반복 조회 저장량 불변, 8개 독립 문맥 반증의
ACK 전 개정/충돌 재검증, 임시 및 Main 프로세스 재시작 뒤 같은 원경험·개정 복구가
포함된다. 프레임 상한 검사는 유지하며 관계 응답 fixture는 16KiB를 설정한다.

설치 VRS/관측기를 갱신하고 실제 backend의 related boolean 스키마 등록,
같은 private 소켓/자원 그룹/공유 I/O 및 종료 정리를 확인했다. 모델 호출과 새 대화
생성은 없었다. 자원·Main 식별자·원경험 설정을 보존하고 manifest 해시를 확인했다.

새 채널 계산의 중앙 묶음 평균은75.44ns, 기존4축 판정47.88ns,
판정+강도 조합54.23ns였다. CPU7, 100000회 예열 및 31개 묶음 측정이며 개별 호출
최악 지연이나 입력→Recall 전체1ms 증거가 아니다. 원시 결과는
measurements/related-cognition-core-20260927.txt에 보존한다.

## 남은 범위

선택된 부모 원경험에 실제로 연결된 관측 하나를 찾는 기능이다. 전체 요구의 자동
추출/목록화, 현재 명제와 관측의 일반적인 의미 적합성을 증명하지 않는다. 실제 GUI
에이전트의 사용 및 전체 대규모 자원/지연·압축 유지 평가는 여전히 남아 있다.
