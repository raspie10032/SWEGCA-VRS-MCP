# 자연 입력의 원문 요구 후보 참조

2026-09-27. NATURAL_INPUT_PLAN 2단계의 구조 처리 첫 구현이다.
일반 자연어의 대상/행위/조건 해석이나 요구 추출 완성이 아니다.

## 경로

프록시가 VRS 입력 ACK와 기존 Replay 처리를 받은 뒤, 에이전트에 전달하는 참조
JSON에 inputCandidates를 추가한다. 처음 입력과 후속 입력 모두 원경험 주소를
포함한다. 입력→Déjà vu→Recall 앞에 분절이나 파싱을 추가하지 않는다.

turn/start와 turn/steer의 text 항목을 줄바꿈까지 포함한 원문 범위로 나눈다.
CRLF, 공백, 부정, 조건, 인용을 삭제/정규화하지 않는다. 복합 문장을 의미 단위로
독단적으로 나누지 않으며 줄 자체도 요구라는 결론이 아니다. 모든 후보는 기존
core-backed requirement_matches를 통과해야 한다. 이것은 인용 실재 검증일 뿐이다.

각 후보는 기존 관측기의 requirement 형식인 textIndex/byteOffset/quote다.
첨부를 포함한 원래 input 배열 위치와 UTF-8 바이트 위치를 사용한다. 같은 줄의
조건/부정은 그대로 유지하며, 다른 줄에 걸친 관계의 의미는 아직 해석하지 않는다.
semanticVerified=false, requirementsComplete=false, grantsAuthority=false,
kind=uninterpreted-text-spans를 명시한다. 원문은 이미 VRS 원경험으로 보존되며,
후보 표현 자체가 관측 지지나 새 강화 근거로 기록되지 않는다.

현재 자동 전달은 8개 후보, 인용 JSON 합계 min(65536, rpc_frame/8) 바이트다.
한 줄이 예산보다 크면 자르거나 건너뛰지 않고 처음 미전달 textIndex/byteOffset과
byteLimited를 남긴다. next는 원문 내 미전달 위치이며 MCP 페이지 토큰은 아니다.
전체 원문 input은 그대로 뒤따른다. 비텍스트 항목 수를 표시하고 해석하지 않는다.

## 검증

wire172 / 실제 stdio7084 checks 통과. 한국어 UTF-8/CRLF/다중 text/첨부 위치,
조건과 부정의 원문 보존, 각 인용의 기존 코어 검증, 항목 한도/0바이트 예산의
미전달 위치를 확인했다. 실제 wrapper/host/proxy/VRS 경로의 첫 입력과 재개 입력에
올바른 inputOriginal 및 해당 원문에서 추출한 인용이 포함되는 것을 확인했다.
테스트에서 빈 참조 객체의 쉼표 처리 오류와 시험 변수 이름 충돌을 수정 후 재검증했다.

## 남은 작업

후보와 실제 관측의 관련성·충분성, 여러 줄의 조건/정정 관계, 새 의미 관계 검증과
일반 목적의 일관성은 아직 미완료다. 원문 범위가 목록에 있다는 사실만으로 요구가
추출/이해/충족됐다고 간주하지 않는다. 현재 빌드에 구현했고 설치본은 직전의
다중 경험 전달 버전이다.

## 명시적 turn/steer의 선행 입력 관계

네이티브 정정 입력을 Recall하고 기록한 뒤, 기존 코어 기반 AppServerRequests의
인증된 요청/응답 턴 결속에서 선행 입력을 찾는다. 같은 threadId/expectedTurnId의
입력이 하나로 확정될 때만 priorInput 주소를 제공한다. 여러 입력이 같은 턴에
결속돼 있거나 턴이 없으면 null이다. 가장 최근 입력을 임의 선택하지 않는다.

inputRelations는 explicit-turn-steer, threadId, expectedTurnId, priorInput,
replacementVerified=false, grantsAuthority=false다. 초기 cognition 원경험에 함께
보존하며 같은 입력 재전송과 재시작 복구는 당시 관계를 반환한다. 나중에 그 턴에
다른 입력이 추가됐다고 과거의 관계를 다시 추정하지 않는다. 프록시의 입력 참조에
그대로 포함한다. 관계만으로 과거 요구를 삭제/취소/약화하거나 전체 대체하지 않는다.

wire174 / 실제 stdio7104 통과. 유일한 선행 입력, 누적 후 모호한 턴, 없는 턴,
재시작 후 당시 관계 복구와 journal 보존, 미확정 관계의 참조 전달을 확인했다.
단순 입력 후보와 마찬가지로 설치본 반영은 아직 하지 않았다. 자연어만으로
어느 요구가 정정됐는지 판단하거나 부분 취소 범위를 계산하는 것은 남아 있다.
