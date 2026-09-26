# Codex 선행 입력 연결 지점: 설치본 확인

현재 상태 갱신: 사용자가 설계를 위임했고 Codex 데스크톱을 첫 대상으로 여러 에이전트를
지원하도록 지정했다. 아래 선택 대기는 해소됐다. 구현 방향은 DELEGATED_DESIGN.md를 따른다.


2026-09-26. 설치된 `codex-cli 0.153.4`의 app-server 스키마를 직접 생성해 확인했다.
기존 VRS C++ 후보나 폐기된 브리지 코드는 참조하지 않았다. 현재 앱 설정과 서비스는
변경하지 않았으며 실제 모델 요청도 보내지 않았다.

## 확인한 인터페이스

`codex app-server generate-json-schema --out build/codex-schema` 결과:

| 파일 | SHA-256 |
| --- | --- |
| `v2/HooksListResponse.json` | `891dd10ef7f78e59631fce05fff2becddb8004b3c0338dbbbd6b4f17ef1fa64f` |
| `v2/TurnStartParams.json` | `a3835e8c1e942e4b358e1a670939b89918b16c4d13105a579899892b7ade6dea` |
| `v2/TurnSteerParams.json` | `4a52eb76e7a717bb388484ccd7538737fca0df35481fc30a21e259f1bfe96e37` |
| `ServerNotification.json` | `b3e76cf11842f3e8b3270c05e000212b56eabafb0152fc38e8f920e2ef902991` |

- 설치본 HookEventName에 `userPromptSubmit`, `sessionStart`, `sessionEnd`, `preToolUse`,
  `postToolUse`, `preCompact`, `postCompact`, `stop`, `interrupt` 등이 있다.
- HookMetadata에 command와 mcpTool 핸들러 및 trustStatus가 있다.
- `turn/start` 필수 값은 `threadId`, `input`이다. `toolOutput`도 제공한다.
- `turn/steer`는 `threadId`, `input`, `expectedTurnId`를 요구한다.
- app-server 알림에 item 시작/완료, 모델 발화 delta, 도구 출력 delta, reasoning delta,
  compaction, turn 완료가 있다. 이는 생성된 프로토콜 스키마의 존재 확인이며 이 앱에서
  모든 내용이 실제로 전달됐다는 검증은 아니다.

## 공식 문서에서 확인한 제한

[Codex Hooks](https://learn.chatgpt.com/docs/hooks):

- UserPromptSubmit은 전송 직전 prompt를 받는다.
- MCP 훅은 기존 연결을 사용하며 동기 실행한다. 연결을 새로 시작하거나 재연결하지 않는다.
- SessionStart 시 MCP가 준비되지 않았을 수 있다. MCP 훅 실패/서버 부재는 작업을 막지 않는다.
- **SessionEnd는 MCP 훅을 지원하지 않는다.** command 훅은 기본 1초, 최대 3초다.
- hosted tool과 일부 특수 도구 경로는 도구 훅을 거치지 않는다.
- 비관리 훅은 현재 정의에 대한 사용자의 검토·신뢰 절차를 통과해야 실행된다.

[Codex App Server](https://learn.chatgpt.com/docs/app-server):

- 클라이언트가 turn/start, turn/steer를 보내고 item/turn 이벤트를 수신한다.
- item/completed는 해당 항목의 완료 상태를 제공한다. turn 완료는 세션 종료가 아니다.

## 구현에 적용할 경계

입력 전에 호출할 인터페이스는 확인됐다. 하지만 MCP 훅만으로 세션 전체 수집과 종료를
달성했다고 볼 수 없다. 현재 `swegca/receive`는 호스트 요청이고, 그 이름을 MCP 훅의
`tool`에 적어도 도구로 자동 변환되지 않는다.

실제 연결은 모델 호출 전 입력 경로, 도구·모델 출력 등 세션 이벤트 경로, 명시적 종료 경로를
모두 Runtime 소유자에게 전달해야 한다. 세 경로는 같은 임시 경험/원천을 사용해야 한다.
종료 훅에서는 종료 사실의 영속 전달까지 맡고 병합 자체는 종료 이후 작업 경로에서 처리해야
한다. 타임아웃, 창 닫힘, turn/completed를 세션 종료로 추측하지 않는다.

입력 전 준비가 끝나지 않았거나 필요한 수집 경로가 비어 있으면 연결 완료로 표시하지 않는다.
앱의 과거 transcript를 읽는 구현만으로 선행 입력 훅을 대체하지 않는다. 훅 생성·등록과 실제
실행은 별도 상태이며, 사용자의 신뢰 설정을 우회해 자동 승인하지 않는다.

## 실제 데스크톱 내장본 재확인

실행 중인 프로세스의 `/proc/PID/exe`를 확인한 결과 현재 데스크톱은
`/usr/lib/chatgpt/resources/codex`를 사용한다. 이 바이너리의 `--version`은
`codex-cli 0.155.0-alpha.9.2`다. 위 최초 조사에서 사용한 PATH의 `codex`는
별도 standalone 설치본 `0.153.4`이므로 데스크톱 연결 계약을 그것만으로 확정하면 안 된다.
프로세스 인자, 환경 변수, 인증 파일, 대화 본문은 읽지 않았다.

내장본으로 `app-server generate-json-schema --out build/codex-desktop-schema`를 실행했다.

| 파일 | 데스크톱 SHA-256 | standalone 스키마와 동일 |
| --- | --- | --- |
| `v2/HooksListResponse.json` | `891dd10ef7f78e59631fce05fff2becddb8004b3c0338dbbbd6b4f17ef1fa64f` | 예 |
| `v2/TurnStartParams.json` | `2dfcf68705896fadc344ccfeb2e9fe5a6bcbbb8b9a90cf449ce232b636daf05a` | 아니오 |
| `v2/TurnSteerParams.json` | `857e7a2b061ae46ed6aedea6a5fddd3a6ce14f0334bf7eafaf10d4cbe68ec78b` | 아니오 |
| `ServerNotification.json` | `df70f8f8ded90d8da63c744ccfef7018223d99e918e5e0aefa0d90856a7f67cc` | 아니오 |

내장본 `turn/start`에는 `disabledPluginIds`, `turn/steer`에는 `clientUserMessageId`가
추가돼 있다. hook 목록 자체가 동일하다는 사실과 전체 입력·이벤트 프로토콜이 동일하다는
주장은 구분해야 한다. 현재 도구 목록에는 임의의 선행 입력 훅이나 app-server 이벤트 구독을
설치하는 도구가 없었다. 생성 스키마 확인은 실제 데스크톱 연결·이벤트 전달의 증거가 아니다.

## SessionEnd를 VRS 명시적 종료로 치환하지 않는다

[현재 공식 Hooks 문서](https://learn.chatgpt.com/docs/hooks#sessionend)는 SessionEnd가
명시적 대화 보관·삭제 외에도 앱 정상 종료 및 클라이언트가 열지 않은 채 30분 유휴 상태일 때
발생한다고 설명한다. `reason`은 현재 모두 `other`다. 따라서 이 훅만으로 사용자의 명시적
종료와 자동 종료를 식별할 수 없다. 이전의 “명시적 종료 경로” 요구를 SessionEnd라는 이름만
보고 충족했다고 판단하면 사용자가 금지한 자동 유휴 병합이 생긴다.

연결 시 SessionEnd만으로 `swegca/end`를 보내지 않는다. VRS 종료를 확정하는 명시적
호스트 입력을 별도로 확보해야 한다. `turn/completed`, 창 닫힘, EOF, 유휴, 압축 후
SessionStart도 VRS 종료 증거로 사용하지 않는다. 이 문서 변경은 새로운 종료 기능이나
추측 판정을 구현한 것이 아니라 사용자 종료 제약을 실제 호스트 신호에 대조한 결과다.

사용자에게 실제 통합 대상이 현재 데스크톱인지 별도 app-server 클라이언트인지 질문을
전달했다. 둘은 같은 배포·연결 작업이 아니므로 선택 전 데스크톱 파일을 수정하거나 별도
클라이언트를 실제 앱 연결의 대체물로 설치하지 않는다. 서비스와 훅 설정은 변경하지 않았다.

## 이번 확인이 증명하지 않는 것

현재 데스크톱 작업에 VRS 훅이 설치됐거나, 세션 전체가 자동 수집되거나, 사용자 입력부터
Recall까지 1ms 미만이 달성됐다는 의미가 아니다. 이 제약 확인을 근거로 실제 연결을 계속
구현해야 한다. API 키·토큰·대화 본문은 이 조사에서 읽거나 출력하지 않았다.

## 현재 구현 갱신

같은 데스크톱 내장본의 버전과 app-server help를 다시 확인했다. 네이티브 turn/start,
turn/steer 및 threadId가 있는 요청·알림을 직접 처리하는 C++ 어댑터를 MCP 수신 경로에
연결했다. 다중 입력 항목 전체를 조회 키에 보존하며 원본 JSON을 그대로 저장한다.
실제 데스크톱 배선 설치와 요청 ID에 따른 응답 귀속은 아직 남아 있다.
구현·검증 경계는 APP_SERVER_EVENT_ADAPTER.md에 기록한다.
