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

## 전체 통신 경로 검증 및 설치

실제 wrapper/host/proxy/VRS와 모델을 호출하지 않는 시험 backend로 turn/start의
정상 turn 응답을 기록한 뒤 turn/steer를 보냈다. 한국어 원문/CRLF/정정 후 조건
문구가 그대로 유지되고 후보 인용을 합쳐 원문을 재구성할 수 있으며, 정정의
priorInput이 최초 입력의 정확한 원경험 주소인 것을 확인했다. replacementVerified는
false로 유지한다. 실제 stdio7120 checks 통과. 이는 의미적 대체 판정의 검증이 아니다.

설치 VRS/프록시와 manifest 갱신. 모든 기존 실행파일 해시와 설치 경로의 실행 중
프로세스 부재를 먼저 확인했고 설정/경험을 보존했다. 실제 backend 초기화 검사에서
도구 등록·소유 소켓·공유 I/O·동일 cgroup·정상 종료 확인. memory.max3999997952,
swap0, CPU6-7, modelCalls0. GUI 강제 재시작·실제 모델 대화·장기 압축 평가 없음.
위의 미설치 표기는 당시 이력이며 현재 후보 참조와 정정 관계는 설치본에 포함된다.

## 명시적 인용 정정의 대응 후보

코어 quoted_revision 소자로 전체 줄 형태 `정정: “이전” → “새 문구”` 또는
`Correction: "old" -> "new"`의 두 원문 범위를 구성한다. ASCII/방향 인용부호,
수평 공백, 말미 마침표와 CRLF를 처리하며 모델/외부 판정기를 호출하지 않는다.
후행 금지 문구·추가 화살표·비어 있는 인용·예시 접두어·인용 내부 escape/줄바꿈은
현재 문법에 해당하지 않아 후보를 만들지 않는다. 지원하지 않는 문장의 원문은 남는다.

inputCandidates.revisionProposals는 priorQuote/replacementQuote를 기존 requirement
형식으로 전달한다. 두 인용 모두 현재 입력의 원문 위치를 코어 인용 검증으로 확인한다.
priorQuote는 과거 원경험의 인용이 아니라 현재 사용자가 언급한 이전 문구다.
antecedentVerified=false/replacementVerified=false를 유지한다. 단순 문자열
일치만으로 과거 요구를 선택하거나 삭제/취소/강도 변경하지 않는다.

원문 후보와 같은 바이트 예산을 사용하며 대응 후보를 더 담지 못하면
revisionProposalsLimited=true를 표시한다. 해당 원문 span 자체는 보존한다.

검증: wire186 / 실제 stdio7161 통과. 한국어/영어, UTF-8 바이트 위치, 모호한
문구 비수용, 실제 desktop 통신의 세 번째 steer에서 대응 후보 전달을 확인했다.
같은 턴에 선행 입력이 여러 개 있으면 priorInput=null인 상태를 유지하며,
명시적 화살표가 있다는 이유로 선행 입력이나 의미 대체를 승인하지 않는다.
이 문법 범위의 후보 생성은 일반 자연어 의미 처리 완성이 아니다. 설치본 반영은 아직이다.

## 2026-09-27 정정 인용과 선행 원경험 위치 연결

`inputRelations.revisionReferences`는 명시적 인용 정정의 현재 두 인용과,
인증된 턴 응답으로 고유하게 확인된 선행 입력의 인용 위치를 연결한다.
선행 원경험은 정정 후보가 있을 때만 한 번 읽는다. 이 작업은 현재 입력의
Recall 진입 및 원경험 기록 이후 실행한다. 모든 위치의 정확성은 기존
SWEGCA content observation을 사용하는 requirement_matches로 검증한다.

- unique: 선행 입력의 전체 text 항목에서 해당 문구가 정확히 한 번 존재.
- ambiguous: 두 번 이상 존재. 겹치는 출현과 서로 다른 text 항목도 포함.
- missing: 확인된 선행 입력에 문구가 없음.
- unresolved-input: 선행 입력 자체가 고유하게 확인되지 않음.

고유 위치에만 priorAnchor를 넣는다. textIndex는 첨부물까지 포함한 원본 배열
위치이며 byteOffset은 UTF-8 바이트 위치다. antecedentVerified와
replacementVerified는 false다. 고유 문자열 위치가 의미적 선행 요구 또는
수정 권한의 증거는 아니다. 기존 경험/요구/연결 강도는 이 위치 연결로 변경하지 않는다.
최대 8개/인코딩 64KiB를 전달하고 초과 시 revisionReferencesLimited를 표시한다.
메모리는 기존 VRS PMR 예산으로 관리한다. 최초 cognition에 저장되어 재시작이나
동일 입력 재전송 때 후속 턴 상태로 재해석하지 않는다.

검증: wire 201 checks, 실제 stdio subprocess 7167 checks 통과.
한국어/영어, 첨부물 인덱스, 겹침/항목 간 중복, 미출현/선행 미확정,
개수/바이트 제한, 재시작 후 저장 관계 동일성, 실제 wrapper/host/proxy/VRS와
테스트 backend 사이의 고유 위치 및 미확정 전달을 확인했다.
실제 GUI/모델 사용, Main 병합 후 이 관계 복구, 범용 자연어 정정 의미 검증은
이 변경의 검증 범위에 포함되지 않는다. 설치본 반영은 아직이다.

### Main 병합 보존과 설치본 반영 확인

후속 검증에서 명시적 세션 종료 후 Main work의 merged=1을 확인하고 프로세스를
재시작했다. live session 재연결 없이 identity+inputOriginal로 최초 cognition을
조회해 inputRelations 전체가 종료 전 값과 동일함을 확인했다. 이 검증은
보존된 계보의 주소 조회이며, 새 문장의 자동 의미 회상/정정 승인 증거는 아니다.

2026-09-27 소스 6c34f5f의 VRS/proxy를 다시 빌드하고 실제 subprocess 회귀검증
7229 checks를 통과했다(비동기 polling에 따라 검사 횟수는 변동).
설치 prefix /home/raspie/.local/share/swegca-vrs/desktop의 전체 실행 파일 해시와
설치 프로세스 부재를 확인한 후 두 바이너리와 manifest를 원자 교체했다.
resource/desktop/proxy 설정은 byte 동일성을 확인했다. 이 설치에는 앞선
측정 보고서 일관성 검증, 인용 정정 후보, 선행 위치 연결 변경이 포함된다.

설치본 backend inventory smoke: VRS/host/proxy/observer의 같은 cgroup,
공유 I/O 소유자, 소유 VRS 조회 socket, 관련 Replay 및 requirement schema 등록,
종료 후 socket 정리를 확인했다. memory.max=3999997952, swap=0, CPU6-7,
modelCalls=0, exitCode=0. GUI 세션 사용/실제 모델 목적 유지/대규모 Main 5ms는
이 설치 확인으로 검증됐다고 주장하지 않는다.

## 문단 내 구조 후보 분절

2026-09-27 input_span_kernel.hpp를 추가해 줄바꿈 외에도 공백 뒤로 이어지는
문장부호(.?!)에서 가능한 후보 경계를 만든다. 이는 문장/요구 의미 판정이
아니며 inputCandidates.boundariesVerified=false를 명시한다. 약어 등 일반
언어의 모든 경계를 정확히 결정한다고 주장하지 않는다.

따옴표/곡선 따옴표/백틱 코드 안에서는 분절하지 않는다. 닫히지 않은 인용은
남은 입력 전체를 보존한다. 인용 안 escape, 숫자 바로 뒤 마침표(목록/숫자),
공백 없는 소수/URL을 보존한다. 모든 공백/CRLF/UTF-8 바이트가 원래 순서에
남으며 조합하면 원문을 복원한다. 다음 미전달 위치도 같은 바이트 좌표다.
기존 8개/바이트 예산 및 요구 인용의 SWEGCA 원문 일치 검증은 유지한다.

정정 문법은 원래의 완전한 물리적 줄일 때만 파싱한다. 예를 들어
`정정: "a" -> "b". 적용 금지.`를 분절했다고 앞쪽만 정정 제안으로 만들지 않는다.
수정 계보와 의미 대체 승인 규칙은 유지한다. 분절은 현재 입력의 Recall/기록과
Replay 이후 proxy에서 수행하므로 입력→Recall 앞에 처리하지 않는다.

검증: wire221/실제 stdio7341 checks 통과. 한국어 한 문단의 여러 후보,
부정/조건 원문 보존, 원문 재조합/위치 일치, 개수 제한의 다음 위치, 인용/코드/
미종결 인용/숫자/URL, 문장 일부만 정정으로 읽지 않는 조건을 포함한다.
실제 wrapper/host/proxy/VRS와 테스트 backend 간 한 문단 후보 전달 및 정정
원경험 위치 연결도 확인했다. 이번 분절 변경은 아직 설치본에 반영하지 않았다.

## 현재 입력 후보의 후속 페이지 조회

기존 vrs_replay에 inputCandidates:{limit,after?}를 연결했다. receipt와
inputOriginal은 필수이며 scope/related/connection/byte-range와 혼합할 수 없다.
limit은 1..64, after는 이전 inputCandidates.next의 textIndex/byteOffset이다.
해당 입력 원경험이 불변이므로 별도 가변 snapshot 토큰은 필요하지 않다.

예: {"receipt":"...","inputOriginal":{...},"inputCandidates":{"limit":"8",
"after":{"textIndex":"0","byteOffset":"123"}}}

소유 VRS는 현재 또는 복구된 receipt와 inputOriginal을 함께 검증한 뒤 해당
원경험 하나를 읽는다. client/source/session/media/method가 맞는 native 입력만
허용한다. offset까지 기존 core span 경계를 재계산해 인용/UTF-8 중간의 임의
위치 요청을 거부한다. 후보는 기존 코어 인용 검증을 사용하고 Replay 선택이나
경험 강도를 변경하지 않는다. 처음 입력으로 Recall 후보가 없어도 조회 가능하다.

페이지 JSON 예산은 64KiB, 원경험 읽기/JSON 메모리는 기존 VRS 한도를 따른다.
한 후보 자체가 예산보다 크면 byteLimited와 같은 다음 위치가 반환된다.
이 경우를 자동 완전 전달로 주장하지 않는다. 구조 후보 자체는 여전히
semanticVerified/boundariesVerified/requirementsComplete=false다.

검증: wire224 / observer154 / stdio7390 checks 통과. 12개 요구 후보를 3개씩
조회해 원문 그대로 연결, 잘못된 item/byte 경계/만료 receipt/혼합 옵션 거부,
실제 observer→소유 VRS socket 조회, 조회 중 block 파일 크기 불변 확인.
설치 VRS/proxy/observer를 함께 반영했고 설치 backend inventory에서 새
inputCandidates schema와 기존 자원 제한/소유자/소켓/I/O/정상 종료 확인.
앞선 1a1b6c8 문단 분절도 이 설치에 포함된다.
