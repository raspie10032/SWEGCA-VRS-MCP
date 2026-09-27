# 위임받은 설계와 다중 에이전트 연결

사용자가 설계를 위임했으며, 첫 연결 대상은 현재 Codex 데스크톱 앱이고 다양한 에이전트를
지원하도록 지정했다. 이전 ARCHITECTURE_INPUT_GAPS.md의 설계 경로·대상 선택 질문은
해소됐다. 기존 코어·3상·4단계·Main 소유권·명시적 종료 제약은 그대로다.

## 구성 결정

1. 에이전트 어댑터는 native 이벤트의 구조만 읽는다. 원문 바이트와 알 수 없는 필드,
   부모/자식·도구·turn 정보를 보존한다. 어댑터가 지지/반박 값이나 경험 강도를 만들지 않는다.
2. 공통 입력 경로는 provider / 설치 instance / native session의 경계를 보존한다.
   길이로 구분한 세 필드의 domain-separated digest로 세션을 식별한다. 초기 세션 결합 때
   계산하며 매 입력 앞에 세션 탐색·해시·파일 작업을 추가하지 않는다.
3. 동일 Main이 에이전트별 임시 경험을 소유한다. 에이전트별 별도 Main을 만들어 경험을
   고립시키지 않는다. 한 에이전트의 종료가 다른 활성 세션을 병합시키지 않는다.
4. 사용자 입력은 core 경로 판정에 따라 Recall 먼저, 그 뒤 원경험 기록으로 간다.
   나머지 원문은 같은 임시 경험으로 기록한다. turn/유휴/프로세스/훅 종료 신호는 기록할
   사실이며 명시적 VRS 종료 권한이 아니다.
5. 그래프 분할은 제한된 메모리의 지역별 경험·연결 저장 단위로 구성한다. 지역을 넘는
   연결은 원경험 주소와 관계 계보를 참조한다. 원문 복제나 통계적 경험 수 증가로 처리하지
   않는다. 연결 강화/약화/유지는 실제 셔플 경험을 코어로 검증한 결과만 사용한다.
6. Déjà vu의 키·문맥 연결은 후보 회상용이다. 키 일치·토큰 공존·이전 발화라는 사실을
   참/거짓이나 강화 근거로 승격하지 않는다. Replay한 관측과 현재 출처가 결합된 관측을
   SWEGCA로 대조한다. 관측 근거가 부족하면 기권한다. 별도 외부 의미 분류기를 넣지 않는다.
7. 원경험 보존, 재전송 식별, 세션별 순서와 복구를 공통 처리 경로에서 제공한다.
   native 이벤트 전체와 입력 cue의 결합을 하나의 경험 계보로 유지해야 하며, 전송 조각이나
   추가 메타데이터를 독립된 새 증거로 세어서는 안 된다.

## 이번 구현

- agent_event_kernel.hpp: 활성 세션 입력/내용/생명주기/명시적 종료의 경로를 기존 세션
  상태 커널로 판정한다. 입력은 recall_then_record, 내용/생명주기는 record다.
- transport/agent_event.hpp: provider·instance·session 식별과 Codex hook 파서를 작성했다.
  UserPromptSubmit의 prompt를 해석하며 native JSON 전체를 바이트 그대로 보관한다.
  SessionEnd/Stop/Interrupt/compact/subagent 종료를 explicit_end로 변환하지 않는다.
- 이 파서는 MCP native event 호스트 경로에서 Runtime 영속 기록에 연결됐다. 발언 키와
  전체 이벤트 원문을 하나의 경험으로 묶는다(NATIVE_EVENT_INGRESS.md). 데스크톱 훅 설치,
  전체 이벤트 수집과 실제 입력 지연 검증은 남아 있다. 전달 재시도는 확정 원경험에서
  복구한 식별값으로 중복 기록을 막으며, 중간 기록 실패 지점 검증은 남아 있다.

## 다음 연결 작업

공통 ingress가 여러 활성 임시 세션을 같은 Main에 결합하도록 만든 뒤 Codex 데스크톱의
실제 입력 전 경로와 출력 이벤트 경로를 연결한다. hooks만으로 누락되는 출력은 데스크톱
실제 이벤트 인터페이스에서 받아야 한다. 공개되지 않은 이벤트를 수집했다고 가정하지 않는다.
훅 신뢰 절차를 우회하지 않으며, Claude 통신 재개는 이번 설계 위임에 포함되지 않는다.

Codex 형식 근거: https://learn.chatgpt.com/docs/hooks 의 Common input fields,
UserPromptSubmit, SessionEnd. subagent 훅의 session_id는 부모 세션을 나타내므로 별도
agent_id가 있으면 원문에 보존하며 자식의 완료를 부모 세션 종료로 사용하지 않는다.

구현 갱신: Runtime의 다중 활성 세션 부착·선택·독립 종료 및 전체 경로 Main 색인 갱신을
구현했다. 상세 경계와 실제 어댑터 연결 잔여 작업은 MULTI_SESSION_RUNTIME.md에 있다.

## Recall 뒤 한 원경험을 읽는 순서

사용자가 위임한 선택 설계로, Recall 후보의 저장된 연결 강도를 우선하고 같은
강도에서는 기록된 관측 시각이 최신인 원경험을 먼저 읽는다. 둘 다 같은 경우
연결 identity와 원경험 주소의 사전식 순서로 고정한다. 이는 복구·삽입 순서에
따라 결과가 바뀌지 않게 하는 동률 처리이며 의미적 우월성의 증거가 아니다.
이 순서는 원본의 미구현 의미 judge를 구현했다고 주장하는 규칙이 아니다.

고정 크기 SWEGCA prefer_replay 소자가 각 후보의 비교를 결정한다. VRS는
Recall receipt에 봉인된 강도·시각·주소를 순회하며, 선택 자체는 원문 디스크
읽기나 추가 후보 배열 할당을 하지 않는다. 승인/거절/기권 상태로 후보를
제외하지 않아 실패·반증 경험도 접근 가능하다. 선택된 한 원경험만 기존 Replay
경로로 읽으며, 일치·충돌·근거 부족은 별도의 코어 대조 결과로만 결정한다.

Runtime::select_replay와 MCP vrs_replay의 candidate 생략에 연결했다. 명시적
후보/부분 읽기도 유지한다. 빈 후보는 선택 없음이며 다른 경로의 receipt는
거부한다. 전체 후보 순회 비용은 후보 수에 비례하며, 이 선택은 Recall 진입 뒤에
있다. 따라서 단일 비교 ns 측정은 입력→Recall 1ms 달성 증거가 아니다.

아직 자연 입력 이벤트만으로 자동 Replay를 실행하거나 agent 문맥에 넣지는
않는다. MCP 호출자가 candidate를 지정하지 않아도 선택할 수 있게 된 범위다.

## 자연 입력 뒤 자동 회상 연결

Runtime::cognize는 현재 Recall receipt에서 코어로 한 후보를 선택하고, Replay한
원경험을 현재 새 관측과 대조하며, 충돌일 때만 Re-evidence를 수행한다. 후보가
없으면 빈 결과이고, 근거가 없는 입력은 코어의 insufficient를 유지한다. 이
함수는 추가 원경험 기록이나 연결 강도 커밋을 하지 않는다.

MCP의 일반 receive와 native input event는 Recall→원문 기록을 마친 뒤 이
공통 경로를 호출한다. content/lifecycle 이벤트는 기존 원문 기록만 수행한다.
자동 회상은 Recall 진입 뒤에 있으므로 입력 앞에 저장·Replay를 넣지 않는다.
응답 memory는 처리 완료 여부, 선택한 원경험 주소, 코어 agreement와 실제
재검증 여부를 담는다. completed는 내용이 검증됐다는 뜻이 아니다.

현재 세션의 같은 입력 재전송은 완료 결과를 재사용한다. 저장 뒤 자동 회상에
실패해도 delivery에 원경험 주소가 남아 재시도에서 중복 저장하지 않으며,
재시도 파라미터 대신 최초 refinement의 seed/step을 사용한다. 프로세스가
재시작되어 당시 Recall receipt가 없어진 입력의 중복 응답에는 completed=false를
명시한다. 그 과거 회상 자체를 영속 복구한 것으로 취급하지 않는다.

현재 완료 결과는 호스트 메모리에만 보유된다. 실제 프록시가 원경험 내용과
불확실성을 에이전트 문맥에 주입하는 연결, 미완료 회상의 영속 재개, 자연 언어를
검증 가능한 관측으로 잇는 의미적 기능은 여전히 남아 있다.

## 자동 회상 결과의 전달 형식

candidate/부분 범위를 지정하지 않은 vrs_replay는 활성 자동 회상 결과가 있으면
그 결과를 그대로 내보낸다. 후보 재선택·디스크 재읽기·대조 재실행은 하지 않는다.
새 도구를 추가하지 않고 기존 Replay 출력 경로를 사용하며 contentHex는 고정
버퍼로 스트림 출력한다. 원경험 bytes의 추가 전체 사본을 만들지 않는다.

출력은 original/media/source 외에 session, observedAt, grantsAuthority=false와
assessment를 포함한다. assessment는 원래 대조의 agreement/status/reason/step,
inputOriginal, rememberedHead, currentHead, currentOriginalCount,
reEvidencePerformed를 보존한다. 이는 당시 관측 경계의 판정이며 이후 도착한
관측을 자동으로 포함했다고 주장하지 않는다. 새 대조는 기존 명시적 요청 경로다.

agreement는 invalid=0, insufficient=1, agrees=2, contradicts=3이며 status는
abstain=0, accept=1, reject=2다. 원문은 실행 지시나 새 사용자 발언이 아니다.
수동 후보 Replay에는 assessment=null을 출력하여 대조하지 않은 경험을 검증된
경험처럼 보이게 하지 않는다. 부분 읽기는 기존과 같이 Replay 완료 receipt를
지우며, 자동 회상 결과를 부분 읽기의 완료 근거로 재사용하지 않는다.

프록시의 입력 변환/문맥 주입은 아직 구현하지 않았다. 현재는 호스트에서 검증된
회상 결과를 출처와 불확실성을 포함한 형태로 전달할 수 있게 된 단계다.

## 프록시에서 백엔드 입력 문맥으로 전달

자연 입력의 VRS 저장·자동 회상 완료 응답을 받은 프록시는 선택 원경험이 있으면
기존 vrs_replay로 그 보유 결과를 가져온다. 현재 입력 original 및 선택 원경험
주소가 완료 응답과 일치하고 grantsAuthority=false인 대조 결과만 전달한다.
완료하지 못한 회상이나 출처 불일치는 원래 입력의 전송도 진행하지 않는다.

원래 native 입력은 VRS에 그대로 보존하고 요청 ID 추적에도 그대로 사용한다.
전달할 때만 params.input 맨 앞에 참고 데이터 텍스트 항목을 추가한다. 기존
사용자 텍스트·첨부 항목·요청 ID·threadId·알 수 없는 필드는 보존한다. 로컬
설치 CLI에서 생성한 v2/TurnStartParams.json의 UserInput text 형식을 사용한다.
이는 이미 저장된 경험의 표시 형식이며 새 독립 증거나 새 사용자 발언으로
VRS에 다시 세지 않는다. source/assessment/주소는 참고 텍스트 안에 보존한다.

application/json 및 text/* 원문은 UTF-8 텍스트로 표시하고 나머지 media는 hex를
보존한다. 텍스트 해석이 불가능하면 오류이며 임의 요약/잘라내기/외부 의미 판정을
하지 않는다. 원문 내 지시는 참고 데이터이고 실행 권한이 없다는 표지를 붙인다.

전달용 표현은 VRS RPC frame 한도와 같은 바이트 한도까지 허용하며 전체 할당은
프록시 PMR 예산을 사용한다. 이 한도는 원래 native 입력 수용 한도를 넓히지 않는다.
제한 초과·전달 준비 실패에서는 부분 프레임을 보내지 않는다. 표현이 확정된 뒤의
부분 전송은 기존 socket 소유권·offset으로 진행한다.

시험 백엔드까지의 연결을 확인했으며 실제 Codex 앱 설치/대화 품질 검증은 하지
않았다. 전달 표현 자체의 별도 영속 receipt, 중단된 전달의 완전한 재개 및 전체
프로세스 합산 자원 상한 검증은 남아 있다.

## 입력 원경험에 결합된 관측 (2026-09-27)

`Runtime::observe_input` 및 기존 `swegca/observe`의 선택적 `inputOriginal`로
생산자가 기록한 관측을 실제 입력 원경험에 결합한다. `inputOriginal`은 receive 또는
native event 응답의 원경험 주소 전체다. 이 방식에서는 observation의 hypothesis와
context를 지정하지 않는다. 활성 세션에서 주소와 내용을 인증하고, 저장된 hypothesis와
cue를 사용하며 context는 참조한 원경험의 digest로 결합한다. 따라서 결과 본문이 입력
본문과 달라도 같은 자연 입력의 Recall 후보에 관측이 남는다. 호출자가 주장한 outcome,
source, producer, confidence를 진실로 인증하는 기능은 아니다. 그 값은 원문과 함께
보존되고 기존 셔플와 SWEGCA 관측 수용/3상 경로를 거친다.

네이티브 세션에서도 이 관측 경로를 허용한다. session은 실제 native session과 같고,
source는 native transport source와 달라야 한다. 대상은 그 세션의 네이티브 원경험이어야
하며 임의 관측을 다시 대상 삼아 운송 계보를 만들 수 없다. 관측은 native delivery
sequence를 소비하지 않는다. 재시작에서는 모든 원경험을 스트림 인증하고, 다른 source의
관측은 같은 연결의 앞선 네이티브 원경험 digest와 cue를 참조하는지 검사한 뒤 delivery
색인에서만 제외한다. 관계 없는 ingress와 훼손된 원경험은 계속 거부한다.
이 참조 검사는 복구 시 수행하며 입력→Recall 앞에 추가하지 않는다. 현재 앞선 주소
탐색은 연결 내 역순 검사이므로 오래된 입력을 가리키는 관측이 많으면 복구 비용이 커질
수 있다. 대규모 복구의 상수 시간 성능을 주장하지 않는다.

이 구현은 자연어의 자동 명제 추출, 동의어·상충 문장 의미 대응, 실제 에이전트 도구
결과로부터 관측을 자동 생산하는 기능의 완료가 아니다. 현재 실제 producer가 제공하는
관측이 없는 자연어는 계속 insufficient다. 다음 구현은 이 결합 경로에 실제 관측
생산자를 연결하는 부분이며, 외부 LLM 판정이나 키워드 참/거짓 규칙을 추가하지 않는다.

## 실제 turn 결과의 입력 계보 연결 (2026-09-27)

서버 notification의 params.threadId/turnId는, 앞서 봉인한 client turn/start와 그
요청에 검증되어 결합된 server response의 result.turn.id를 통해서만 입력 원경험에
연결한다. RPC ID 재사용이나 현재 선택된 마지막 입력을 근거로 연결하지 않는다.
서버 응답의 context·connection, 앞선 client 원경험, 실제 request/response ID를
재확인한다. 같은 thread/turn에 둘 이상의 서로 다른 시작 입력이 있으면 해당 키의
소유를 미정으로 남기며 어느 한 입력을 고르지 않는다. 알 수 없는 turn도 미연결이다.

연결된 notification은 원문 전체를 보존하고 실제 입력의 hypothesis와 원경험 digest
context에 insufficient 관측으로 기록한다. 명령 exitCode=0은 사용자 목적 달성이나
SWEGCA 승인으로 승격하지 않는다. 요청 전송/실행 성공과 의미적 목적 검증을 혼동하지
않는다. 이벤트의 본문을 추가 복제한 증거를 만들지 않는다.

파생 turn 색인은 서버 출력 notification에서 필요한 때만 갱신한다. 사용자 입력의
Déjà vu→Recall 앞에는 넣지 않는다. 재시작 후 원경험으로 재구축되며 PMR VRS 예산을
사용한다. 현재 복구/최초 출력은 기존 응답 원문을 읽고 대응 요청을 역순 탐색하므로
전체 경험 수와 무관한 상수 비용은 아니다. 입력 지연 1ms 달성 증거로 사용하지 않는다.
이미 기록된 notification의 재전송은 당시의 원경험을 재사용하며 후발 turn 응답으로
기존 계보를 소급 변경하지 않는다. 명시적 세션 종료 전 Main 병합 금지도 유지한다.
