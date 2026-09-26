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
| `swegca/define` | `identity` | 설정된 Main 규칙·초기 강도로 임시 연결 정의 |
| `swegca/observe` | 이벤트 필드 + `observation` | 기록된 관측을 저장·셔플·코어 검증 |
| `swegca/end` | `{}` | 명시적 종료·발행. EOF는 종료가 아님 |
| `swegca/work` | `seed`, `step` | 종료·발행된 미병합 원천 처리 및 조회 인덱스 갱신 |

`identity`는 64자리 소문자 hex다. 이벤트에는 `sequence`, `observedAt`, `seed`, `step`의
십진 문자열, `session`, `source`, `media`, 그리고 `content` 또는 `contentHex` 중 하나를
넣는다. text의 NUL은 JSON escape로 표현하고, 임의 바이너리는 hex로 전달한다.
호스트 확장은 `experimental.swegcaHostInput`으로 고지한다. 실제 클라이언트가 모델 호출
전에 모든 이벤트를 전달하는 연결은 아직 설치하지 않았다.

수신 응답은 현재 receipt, 기록된 원경험 주소, 기록 전 후보 목록을 돌려준다.
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
명시한다. 호스트와 서버는 같은 프로세스의 Runtime을 두 번 소유하지 않는다.
설정 파일은 최대 64KiB다. 예제의 4GB는 VRS 공유 할당 예산이며 실제 RSS·페이지 캐시·
부트스트랩 설정 파싱 예산을 모두 포함한 프로세스 제한은 아직 아니다.

## 검사와 제한

일반 빌드와 UBSan trap 빌드에서 각각 335개 확인이 통과했다. CPU 6·7만 사용했다.
`make check-stdio`는 테스트 전용 임시 저장소에 서버 자식 프로세스를 실제로 띄운다.
초기화·도구 목록, malformed/oversized 입력, Unicode/NUL/binary 보존, 기록 전 후보,
선택 Replay, 활성 세션에서 병합 없음, EOF 후 재개, 명시적 종료 후 재시작 병합을 검사한다.

현재 전송은 동기적이다. 배경 워커와 모든 실제 세션 이벤트 훅은 아직 남아 있다. 수신 성공 뒤 응답 전 연결이 끊긴 경우, 재전송 중복 방지를 위한 영속 요청 ID
체계도 아직 없다. 응답 오류를 자동으로 기록 미완료라고 간주해서는 안 된다.
세션 전체 자동 수집이나 입력→Recall 1ms 달성, SSD 속도·총 저장량 준수를 주장하지 않는다.
기존 서비스 및 Codex/Claude 설정은 변경하지 않았다.

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
