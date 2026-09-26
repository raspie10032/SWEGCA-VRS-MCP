# C++ MCP 표준입출력 서버

2026-09-26. `build/swegca-vrs-mcp`는 직접 컴파일하는 C++ 실행 파일이다.
외부 SDK·JSON 런타임·프리빌트 휠을 추가하지 않는다. 전송 구문 처리는 `cpp/transport/`에
있고 경험 판단·수명·병합은 기존 `vrs::Runtime`과 SWEGCA 코어로 전달한다.

## 지원 프로토콜과 호스트 경계

[MCP 2025-06-18 stdio](https://modelcontextprotocol.io/specification/2025-06-18/basic/transports)의
UTF-8 JSON-RPC 한 줄 메시지와 [도구 호출](https://modelcontextprotocol.io/specification/2025-06-18/server/tools)을
구현한다. 지원 버전은 `2025-06-18`이다. 전체 MCP 인증·호환성 인증을 주장하지 않는다.

초기화, `notifications/initialized`, ping, 도구 목록·호출을 처리한다. 표준출력에는 프로토콜
응답만 내보내고 진단은 표준에러로 보낸다. 프레임 크기, 깊이, UTF-8, Unicode surrogate,
중복 JSON 키를 검사한다. JSON 숫자 원문을 보존하며 64비트 순번·시드·시간은 십진 문자열로
전달해 클라이언트 부동소수점 변환 손실을 피한다.

모델에 공개하는 도구는 `vrs_replay`와 `vrs_re_evidence` 두 개다. 입력 수집을 모델의
도구 선택에 맡기지 않는다. 호스트는 초기화 후 다음 **요청**을 직접 보낸다.
알림 형태의 호스트 변경 메시지는 실행하지 않는다.

| 메서드 | params | 동작 |
| --- | --- | --- |
| `swegca/start` | `identity`, `name` | 임시 세션 시작 |
| `swegca/resume` | `identity` | 지정된 세션 복구 |
| `swegca/receive` | 아래 이벤트 필드 | 먼저 Déjà vu/Recall, 이후 원문 전체 임시 반영 |
| `swegca/retain` | 아래 이벤트 필드 | 세션 이벤트 원문을 저장·셔플·코어 검증하며 현재 Recall/Replay 유지 |
| `swegca/candidates` | `receipt`, `offset`, 선택 `candidateLimit` | 고정된 Recall의 후보 주소 페이지 조회 |
| `swegca/define` | `identity` | 설정된 Main 규칙·초기 강도로 임시 연결 정의 |
| `swegca/observe` | 이벤트 필드 + `observation` | 기록된 관측을 저장·셔플·코어 검증 |
| `swegca/end` | `{}` | 명시적 종료·발행. EOF는 종료가 아님 |
| `swegca/work` | `seed`, `step` | 종료·발행된 미병합 원천 처리 및 조회 인덱스 갱신 |
| `swegca/work/start` | `seed`, `step` | 종료·발행 원천의 백그라운드 복구·병합 준비 예약 |
| `swegca/work/poll` | `{}` | 준비 완료 확인·Main 발행·조회 인덱스 갱신 |

`identity`는 64자리 소문자 hex다. 이벤트에는 `sequence`, `observedAt`, `seed`, `step`의
십진 문자열, `session`, `source`, `media`, 그리고 `content` 또는 `contentHex` 중 하나를
넣는다. text의 NUL은 JSON escape로 표현하고, 임의 바이너리는 hex로 전달한다.
호스트 확장은 `experimental.swegcaHostInput`으로 고지한다. 실제 클라이언트가 모델 호출
전에 모든 이벤트를 전달하는 연결은 아직 설치하지 않았다.

호스트 확장 버전은 `4`다. 버전 3에서 `retain`, 버전 4에서 작업 start/poll을 추가했다.
수신 응답은 현재 receipt, 기록된 원경험 주소, 기록 전 후보의
첫 페이지를 돌려준다. `candidateCount`는 전체 후보 수의 십진 문자열이고, `nextOffset`은
다음 페이지 시작 인덱스의 십진 문자열 또는 끝을 나타내는 `null`이다. `candidates`의
`index`는 전체 Recall 내 절대 인덱스다. 후보 순서·주소·판정은 페이지 크기로 바뀌지 않는다.

`receive`와 `candidates`에 `candidateLimit` 십진 문자열을 선택적으로 지정한다.
기본 64개, 허용 범위 1..256개다. `candidates`에는 현재 `receipt`와 `offset`을 전달한다.
offset이 전체 수와 같으면 빈 마지막 페이지, 더 크면 오류다. 잘못된 수신 페이지 크기는
경험 기록과 기존 receipt 폐기 전에 거부한다. 페이지 조회는 기록·셔플·Replay를 실행하지
않으며 기존 Replay receipt도 유지한다. 다음 수신이나 종료 후에는 옛 페이지 요청을 거부한다.

전체 후보를 응답 문자열로 한 번에 복제하지 않는다. 정확한 키로 찾은 내부 Recall은 후보마다
선택한 불변 경험 항목을 참조하며, 대화 연속 Recall은 불변 경험 구간을 공유한다. 전체 조회 메모리가
상수가 되지는 않는다.
`vrs_replay`는 `receipt`, `candidate` 문자열을 받아 선택 원경험 주소·미디어·출처·contentHex를
반환한다. `vrs_re_evidence`는 같은 receipt와 seed/step으로 실제 읽은 경험을 재검증한다.
새 수신 또는 종료는 이전 전송 receipt를 만료시킨다. 프로세스 간 영속 receipt는 아니다.
도구 성공 응답은 text와 structuredContent를 함께 제공한다.

## 빌드와 실행

```sh
make build/swegca-vrs-mcp
mkdir /path/to/new-vrs-root
build/swegca-vrs-mcp create /path/to/new-vrs-root examples/stdio-config.json
# 이후에는 동일한 identity, 초기 강도, 정책으로 복구
build/swegca-vrs-mcp open /path/to/new-vrs-root examples/stdio-config.json
```

예제 설정은 사용자가 선택할 Main identity·초기 강도·기존 EvidencePolicy 필드를 모두
명시한다. `mergeWorkers`는 독립 연결의 복구 및 Main 병합 준비에 사용할 작업 슬롯 수이며 예제는 2다.
각 연결 내부의 기록 순서와 코어 재검증은 직렬로 유지한다.
호스트와 서버는 같은 프로세스의 Runtime을 두 번 소유하지 않는다.
설정 파일은 최대 64KiB다. 예제의 4GB는 VRS 공유 할당 예산이며 실제 RSS·페이지 캐시·
부트스트랩 설정 파싱 예산을 모두 포함한 프로세스 제한은 아직 아니다.

## 검사와 제한

일반 빌드와 UBSan trap 빌드에서 각각 335개 확인이 통과했다. CPU 6·7만 사용했다.
`make check-stdio`는 테스트 전용 임시 저장소에 서버 자식 프로세스를 실제로 띄운다.
초기화·도구 목록, malformed/oversized 입력, Unicode/NUL/binary 보존, 기록 전 후보,
선택 Replay, 활성 세션에서 병합 없음, EOF 후 재개, 명시적 종료 후 재시작 병합을 검사한다.

요청 처리는 직렬이며, 명시적으로 예약한 병합 준비는 배경 워커에서 진행한다. 실제 세션
이벤트 훅은 아직 남아 있다. 수신 성공 뒤 응답 전 연결이 끊긴 경우, 재전송 중복 방지를 위한 영속 요청 ID
체계도 아직 없다. 응답 오류를 자동으로 기록 미완료라고 간주해서는 안 된다.
세션 전체 자동 수집이나 입력→Recall 1ms 달성, SSD 속도·총 저장량 준수를 주장하지 않는다.
기존 서비스 및 Codex/Claude 설정은 변경하지 않았다.

## 사용자 입력 이후 세션 이벤트

호스트는 사용자 입력을 `receive`로 전달하고, 이후 모델 발화·도구 출력·그 밖의 실제
세션 내용을 같은 이벤트 필드로 `retain`에 전달할 수 있다. source는 호스트가 제공한
출처를 그대로 보존한다. 이벤트 종류를 전송 계층에서 승인·거절 등의 증거로 변환하지 않는다.

`retain`은 `Runtime::retain` → `SessionRuntime::retain_input` → 원경험 저장·실제 경험
셔플·SWEGCA 검증을 호출한다. 로그만 따로 저장하지 않는다. 응답은 원경험 `original`과
실제 코어 `refinement`이며 새 receipt를 만들지 않는다. 현재 Recall의 후보 집합과 선택
Replay를 보존하므로, 이후 재검증은 원래 Recall 경계 이후에 추가된 같은 연결의 경험을
검사할 수 있다. 일반 원문에는 별도 진실성 판정이 없으므로 insufficient 관측으로 보존한다.
다른 내용 사이의 의미적 연결을 이 메서드가 추측하지 않는다.

후속 사용자 입력은 계속 `receive`로 보내야 한다. `retain`을 사용자 입력 훅 대신 사용하면
선행 Déjà vu/Recall을 실행하지 않으므로 잘못된 연결이다. `retain` 성공은 현재 임시 VRS에
반영됐다는 뜻이며 Main 병합이나 세션 종료를 뜻하지 않는다. 세션이 없거나 종료됐으면
기록을 거부한다. 명시적 종료와 `work` 경계는 동일하다.

이 전송 경로의 존재가 실제 클라이언트의 모든 이벤트가 연결됐음을 뜻하지 않는다.
호스트가 전달하지 않은 내용은 수집하지 못한다. 이벤트 기록 요청은 동기 실행하며 응답 유실 뒤
재전송 중복 방지 및 실제 앱 이벤트 연결은 여전히 남아 있다.

검증: CPU 6·7에서 네이티브 서버를 실행한 `tests/stdio_tests.py`의 1,105개 확인이 통과했다.
모델·도구·reasoning·compaction 출처의 테스트 이벤트, UTF-8/NUL/바이너리, 현재 receipt 유지,
같은 연결의 새 원경험만 재검증, 잘못된 요청과 알림의 무기록, EOF 후 복구, 종료 전 병합 없음,
종료 후 Main 조회·선택 Replay를 포함한다. 출처 이름은 테스트 입력이며 실제 앱 수집 증거가 아니다.

## 백그라운드 병합 요청

`work/start`는 `started` boolean을 반환한다. false이면 예약할 미병합 원천이 없었다.
true이면 종료·발행 원천 목록을 예약했으며 준비가 끝나도 자동 발행하지 않는다.
준비 중 다른 세션의 입력·이벤트 기록·Replay를 받을 수 있다. 중복 start와 기존 동기 work는
현재 예약이 해제될 때까지 거부한다. 알림 형태의 start/poll은 다른 변경 요청처럼 실행하지 않는다.

`work/poll`은 준비 중이면 `{"running":true,"merged":null}`, 모든 예약 원천 처리가
끝났으면 `{"running":false,"merged":"N"}`을 반환한다. N은 해당 예약에서 실제 발행한
원천 수다. 예약이 없는 poll은 0이다. 준비 실패는 오류 응답이며, 이전 원천이 이미 발행됐을
수 있으므로 자동 롤백이나 무기록으로 해석하지 않는다. 영속 쓰기 실패는 재개방이 필요하다.
EOF는 작업을 join하고 미발행 결과를 폐기하며 세션을 끝내거나 Main에 발행하지 않는다.

준비 중 poll은 계산 완료를 기다리지 않지만, 완료 후 발행·조회 인덱스 갱신은 동기 처리다.
호스트는 입력 처리 바깥에서 이를 호출해야 한다. 자세한 계약은 ASYNC_RUNTIME_WORK.md를 따른다.

네이티브 stdio 검사 1,184개가 통과했다. 작업 예약 후 새 세션 입력 처리, 중복 작업 거부,
명시적 poll 후 Main 원경험 Replay를 포함한다. 실제 앱 자동 호출이 설치된 결과는 아니다.

## 기록된 관측의 전송 (후속)

`swegca/observe`는 일반 이벤트 필드와 다음 `observation` 객체를 받는다.

- `hypothesis`, `source`, `context`, `producer`: 각 64자리 소문자 hex.
- `axis`, `expiresAt`: 십진 문자열.
- `hasExpiry`: JSON boolean. `confidence`: JSON number.
- `outcome`: `support`, `refute`, `insufficient` 중 하나인 **관측 결과**.

관측 시각은 이벤트의 `observedAt`으로 묶고 원경험 주소는 실제 저장 결과로 부여한다.
호출자가 코어의 승인·반려·기권 판정이나 갱신 강도를 설치하는 필드는 없다.
관측 내용의 진실성을 전송 성공만으로 보장하지 않는다. 원문과 관측값의 기록 결합 및
출처·다양성·불확실성·만료 검사는 기존 SWEGCA 경로를 따른다.

응답의 `refinement`에는 코어 status/reason, 갱신 강도, 개정이 들어 있다. `receive`에도
같은 형식의 실제 검증 결과를 반환한다. 관측 기록은 기존 선택 Replay를 만료시키지 않아서,
이후 `vrs_re_evidence`가 Recall 이후의 같은 연결 관측만 새로 검증할 수 있다.
재검증 응답에는 `replayedOriginal`, `rememberedHead`, `currentHead`, `currentOriginals`도
포함한다. 반환 주소는 실제 검증에 사용한 원경험 계보이며 원문 로그 검색 결과가 아니다.

실제 서버 프로세스에서 지지·반박 관측을 보내 3상별 ×1.01/×0.995/유지를 확인하고,
과거 지지 원경험을 Replay한 후 새 반박 8개만 재검증해 불일치를 확인했다.
현재 관측 주소 목록이 보낸 반박 원경험 주소와 정확히 일치하는 것도 확인했다.

### Storage allowance

Configuration requires `storageBytes` as a decimal string; the example uses
`"500000000000"`. Runtime inventories and deduplicates existing files under its
exclusive root lock before accepting writes. One allowance covers session,
catalog and Main block writes. Quota exhaustion returns an error; it does not
implicitly end a session or merge it. See STORAGE_BUDGET.md for conservative
failed-write/replaced-file accounting and physical-device limitations.

### Transfer allowance

Configuration also requires `ioBytesPerSecond`, default example `"625000000"`
(5Gbps). One limiter covers block reads and writes together, with a 1MiB burst.
This controls logical requests rather than the entire physical SSD; see
TRANSFER_BUDGET.md. The setting is VRS-level and does not alter SWEGCA verdicts.

### Whole-process memory profile

On this Linux PC, use `build/swegca-vrs-mcp limited-create ROOT CONFIG.json` or
`limited-open` to launch the complete VRS child in a transient systemd user
service. `memoryBytes` becomes MemoryMax, swap is disabled, and `cpuAffinity`
selects its CPUs (example `"6 7"`). The child verifies the actual controls before
opening Runtime. Direct create/open only apply C++ budgets. Details and the
isolated OOM test are in PROCESS_RESOURCE_PROFILE.md.

## 선택 원문 구간 반환

기존 `vrs_replay`에 `offset`, `count`를 십진 문자열로 함께 지정하면 현재 receipt의
선택 후보에서 원문 바이트 구간만 반환한다. 단위는 UTF-8 문자 수가 아닌 바이트다.
`original`, `partial:true`, `offset`, `totalBytes`, `contentHex`를 반환한다.
현재 레코드 형식에서는 전체 무결성 검증 때문에 한 구간마다 선택 레코드 전체를 읽지만,
메모리에는 요청 구간만 남긴다. 임시/Main 둘 다 동일한 주소·계보 검사를 거친다.

부분 요청은 기존 전체 Replay 상태를 폐기하며 Re-evidence 완료 조건을 충족하지 않는다.
후속 `vrs_re_evidence`는 거부된다. offset/count를 모두 생략한 전체 Replay가 성공하면
다시 Re-evidence를 사용할 수 있다. 둘 중 하나만 지정하거나 원문 범위를 벗어나면
도구 오류를 반환한다. 모든 구간을 따로 읽어도 자동으로 전체 완료를 인정하지 않는다.
현재 구현은 별도 앱의 입력·세션 이벤트 훅을 설치하지 않는다.
