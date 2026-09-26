# Codex 선행 입력 연결 지점: 설치본 확인

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

## 이번 확인이 증명하지 않는 것

현재 데스크톱 작업에 VRS 훅이 설치됐거나, 세션 전체가 자동 수집되거나, 사용자 입력부터
Recall까지 1ms 미만이 달성됐다는 의미가 아니다. 이 제약 확인을 근거로 실제 연결을 계속
구현해야 한다. API 키·토큰·대화 본문은 이 조사에서 읽거나 출력하지 않았다.
