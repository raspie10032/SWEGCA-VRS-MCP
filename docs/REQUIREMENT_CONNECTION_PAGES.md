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
