# 요구 관측 연결별 선택 페이지

2026-09-27. NATURAL_INPUT_PLAN 1단계의 Runtime 기반 구현이며, 아직 MCP/프록시
다중 경험 전달이나 일반 자연어 요구 자동 추출을 완료한 것은 아니다.

## 구현

`Runtime::select_replay_connections(receipt, limit, after)`는 소유 라우터가 발행한
Recall 안의 서로 다른 연결마다 기존 SWEGCA `prefer_replay`로 후보 하나를 선택한다.
연결 digest 순서는 페이지 분할에만 쓰며 지지/반박/불충분의 우열 판정이 아니다.
각 결과는 연결과 해당 receipt 내 후보 인덱스다. 실제 Replay는 기존 검증 경로로 한다.

양수 limit만 허용하고 마지막 페이지는 next가 없다. 한 페이지의 임시 identity 집합은
최대 limit+1개, 반환 항목은 limit개이며 모두 기존 VRS MemoryBudget의 PMR을 사용한다.
커서는 같은 receipt에서 이어가야 한다. 다른 입력의 최신 Recall로 바꾸고 과거 커서를
재사용하는 것은 완전 열거 계약이 아니다. 전송 계층은 receipt 결속을 유지해야 한다.
원경험 payload는 열거에서 읽지 않는다. 기존 sealed metadata page reduction을 사용한다.
현 구현은 페이지당 메타데이터를 여러 번 순회하므로 대규모 속도 달성을 뜻하지 않는다.

## 검증

Runtime 2108 / 실제 stdio 6766 checks 통과.
- 같은 부모 입력에 지지/반박/불충분의 서로 다른 세 연결이 모두 남는다.
- 1개씩 이어받기와 한 번에 3개 선택 결과가 같고 중복/누락이 없다.
- 일반 응답과 다른 부모 입력의 관측은 기존 related 자격에 따라 제외된다.
- Main 병합/재개 뒤 2+1 페이지, 끝 커서, 0 limit 거부, 다른 라우터 receipt 거부.
- 동일 연결의 31개 후보/콜드 payload 상태에서도 항목 하나, 기존 코어와 같은
  후보30 선택. 의도적으로 실패시킨 payload read를 소비하지 않는다.
- 임시 페이지 열거는 원문 read/write를 발생시키지 않는다. 모든 규모/불완전
  메타데이터 페이지에서 I/O가 없다는 주장으로 확대하지 않는다.

## 다음 연결과 제한

MCP의 입력/부모 결속된 페이지 조회 및 연결별 저장·복구, 제한된 프록시 전달과
미전달 항목 표시가 다음 작업이다. 기존 단일 related 선택은 현재 그대로다.
관측이 아직 없는 요구는 이 목록에 없으므로 목록 끝을 전체 목적 검증 완료로
취급하지 않는다. 임시/Main 조회 범위는 기존 Recall 규칙을 따른다.
설치본은 이 기반 API 때문에 교체하지 않았다. 자연어 해석이나 외부 판정은 추가하지 않았다.

## MCP 연결 페이지 조회

`vrs_replay`와 소유 세션의 `swegca/agent/replay`에서 다음 인자를 받는다.

```json
{"receipt":"1","inputOriginal":{"block":"...","offset":"...","bytes":"...","digest":"..."},
 "related":true,"connections":{"limit":"16"}}
```

limit은 1..64. 응답은 inputOriginal, relatedFrom, temporary, snapshot,
connections(connection 및 원경험 주소), next를 포함한다. selectionOnly=true,
requirementsComplete=false, grantsAuthority=false를 명시한다. 원경험 본문이나
지지/반박 판정을 새로 생성하는 API가 아니다. scope/candidate/byte range와 혼용 불가.
다음 페이지는 connections에 after=직전 next, snapshot=반환 snapshot을 넣는다.
원래 입력 주소와 receipt 결속을 확인한다. snapshot은 현재 Recall의 연결 head,
관측 경계, 조회 키 및 구간 메타데이터를 코어 SHA256으로 식별한다. 관측이 추가되어
조회 내용이 달라졌으면 stale snapshot을 거부하고 첫 페이지 재조회를 요구한다.
페이지당 snapshot 계산은 후보 구간 메타데이터 전체에 비례한다. 대규모 지연 상한
달성을 주장하지 않는다. 새 후보 평가/외부 의미 판단을 추가하지 않는다.

검증: Runtime2108 / stdio6894 통과. 실제 subprocess에서 동일 입력의 3개 scope
지지/반박/불충분, 1+2 페이지와 전체 목록 일치, 잘못된 limit/snapshot/커서 거부,
재시작 복원 동일 목록, 후속 관측 추가 시 stale snapshot 거부, Main 조회를 확인했다.

다음 작업은 연결별 원경험 Replay·비교의 저장/복구와 프록시의 다중 항목 전달이다.
프록시/관측기 query bridge 스키마와 설치본에는 이 페이지 API를 아직 연결하지 않았다.

## 연결별 Replay와 비교 기록

`related:true, connection:"<목록의 연결 digest>"`로 그 연결 내 코어 선택을 요청한다.
receipt/inputOriginal이 필수이며 connections 목록/scope/candidate/byte range와 혼용할 수 없다.
임의 원경험 주소를 읽는 기능이 아니다. 현재 부모 원경험의 적격 related Recall 안에서
지정 연결을 찾고 기존 SWEGCA prefer_replay, Replay, 비교, 충돌 시 재검증을 수행한다.

비교 기록은 코어의 별도 provenance domain `related_connection_cognition_channel`로
입력과 연결에 결속한다. 기본 related/부모/scoped 기록과 충돌하지 않는다. 기존
숫자 판정식은 변경하지 않았다. 메모리는 한 related 비교 캐시만 유지하고, 연결을
바꿀 때 기록을 완료한 뒤 해당 연결 기록을 복원한다. 원문 선택을 새 후보로 바꾸지
않고 기존 비교 경계 이후의 관측을 대조한다. journal에는 relatedConnection이 있으며
복구 시 requested connection, recovery connection, 기록 필드, 부모 주소를 검증한다.

`swegca/agent/cognition`도 related:true, connection으로 독립 기록을 조회한다.
선택되어 있지 않은 연결의 기록을 현재 캐시의 liveRevision으로 잘못 표시하지 않는다.

검증: Runtime2108 / 실제 stdio6991 통과. 세 연결의 원경험·revision을 번갈아 조회,
독립 journal, 없는 연결 거부, 임시/Main 재시작 복구를 확인했다. 별개 D 관측은
A/B/C 기록을 바꾸지 않으며 B에 후속 관측을 추가하면 B의 revision만 갱신되고
선택 원경험과 A/C 기록은 유지된다. 원경험 출처와 기권/반박을 전체 목적 통과로
승격하는 경로를 추가하지 않았다.

다음: query bridge 스키마와 프록시 다중 전달/분량 제한·미전달 표시. 설치본은 아직 이전
검증 버전이며 이번 연결별 Replay와 페이지 기능을 설치하지 않았다.

## 에이전트 query bridge

관측기 MCP의 vrs_replay 스키마에 connections/connection을 연결했다. 같은 소유 VRS
소켓으로 전달하며 새 VRS 인스턴스나 별도 판정 경로를 만들지 않는다. 구문/필수 주소/
관련 인자 조합/limit/커서 형태는 연결 전에 검사한다. 실제 소유권·원경험 결속·snapshot·
선택·판정은 VRS가 검증한다. 공유 I/O/메모리와 query socket 제한은 유지한다.

실제 observer→Unix socket→VRS→응답 경로로 목록과 연결별 원경험을 확인했다.
없는 endpoint를 사용한 잘못된 인자 검사는 연결 시도 전에 구문 오류가 반환됨을
확인한다. stdio7006 / content observer129 checks 통과. 현재 빌드에 구현했으며
설치본 및 프록시 자동 다중 전달은 아직 갱신하지 않았다.

## 프록시 다중 경험 자동 전달 및 설치

부모 Replay 후 적격 관측이 있으면 프록시가 연결 페이지를 조회하고 연결별 Replay를
수행하여 `relatedExperiences` 배열을 전달한다. 각 항목의 입력/부모/권한 없음/부모 판정
불변을 검증하고 원문을 전달한다. `relatedCoverage`는 조회 snapshot, 목록, next,
실제 deliveredConnections, limited, requirementsComplete=false를 포함한다. 목록에 없는
요구를 검증 완료로 취급하지 않는다. 기존 사용자 input 배열은 수정하지 않는다.

프록시 설정 `relatedConnections` 기본8(1..64), `relatedContextBytes` 기본 min(262144,
vrsFrameBytes/2), 0 허용, 최대 vrsFrameBytes/2. 한 페이지의 항목만 자동 전달하고
다음 페이지는 coverage.next로 남긴다. 원경험 봉투 크기로 보수적으로 사전 확인하고,
반환 후에는 hex→JSON 텍스트의 최악 확장까지 계산해 바이트 예산을 확인한다.
예산에 들지 않는 항목은 원문을 자르지 않고 coverage 목록에 남긴다. 바이트 예산은
관련 Replay 항목의 표현에 적용하며 부모/목록 메타데이터와 사용자 입력은 기존
전체 프레임/PMR 예산이 적용된다. 프로세스의 전체 최대 메모리 보장을 뜻하지 않는다.

검증: wire160 / 실제 stdio7078 checks 통과(폴링에 따라 총 check 수 변동 가능).
실제 wrapper/host/proxy/VRS+시험 backend에서 세 요구의 support/refute/insufficient
원경험이 모두 먼저 전달됨을 확인했다. 1항목 제한의 next와 limited, 0바이트 제한의
빈 전달 배열과 원래 3개 연결 목록 유지, 사용자 원문 보존을 확인했다. 다중 배열의
두 번째 항목이 다른 부모를 가리키면 거부한다. 내부 모델 호출 없음.

설치 전 기존 manifest의 모든 실행 파일 해시, prefix의 실행 중 프로세스 부재 확인.
VRS/proxy/observer와 manifest 교체, 설정 파일 보존. 실제 backend 초기화 검증에서
도구/소유 query socket/공유 I/O/동일 실행 그룹/정상 종료 확인. memory.max3999997952,
swap0, CPU6-7, modelCalls0. 사용자 GUI 강제 재시작이나 실제 모델 대화는 수행하지 않았다.

남은 1단계 한계: 아직 관측이 없는 자연어 요구 자체를 자동 구성하지 않는다. 같은
연결의 전체 반증 원문을 모두 전달하는 기능도 아니며 선택 원경험과 비교 결과를
전달한다. 임시/Main 범위는 기존 related Recall을 따른다. 전체 자연어 처리 완성 아님.
