# 하나의 Main 아래 여러 활성 임시 세션

Runtime이 하나의 Main/자원 예산/저장 루트를 소유하며, 세션 identity별 Active 객체를
PMR map에 유지한다. Active 안의 SessionRuntime 참조와 ExperienceRouter는 등록 중
주소가 변하지 않는다. MainSources lease가 각 활성 세션의 캐시를 보호한다.

- attach_session: 새 세션을 부착한다. 선택하거나 기존 세션을 종료하지 않는다.
- attach_resumed_session: 영속 세션을 재개해 부착한다. 종료 여부는 저장된 core 상태다.
- select_session: 이미 부착된 경로를 선택한다. 할당·파일 접근·Main 갱신·종료가 없다.
- start_session/resume_session: 기존 단일 경로 호출은 선택 경로가 없어야 하며,
  부착 후 선택한다. 기존 호출자의 중복 시작 오류 의미를 유지한다.
- end_session: 선택한 세션만 명시적으로 종료·발행하고 그 경로를 해제한다.
  다른 부착 세션은 그대로 활성 상태다. 다른 세션을 자동 선택하지 않는다.
- work/poll_work: Main 발행 후 부착된 모든 경로의 색인을 갱신한다. 선택 경로가 없어도
  수행한다. 새 Main 색인 갱신 중 할당 실패하면 일부 경로가 먼저 갱신될 수 있으나
  각 경로의 기존 원자적 mount 계약은 유지된다. 후속 work에서 미갱신 경로를 재시도한다.

Public API는 Main 소유자에서 직렬 호출한다. 여러 어댑터 스레드가 Runtime을 동시에
호출해도 된다는 의미가 아니다. source의 병렬 준비와 활성 세션 이벤트의 기존 경계는 유지한다.
Main이 다른 에이전트의 활성 임시 경험을 미리 합치지 않는다. 각 조회는 자신의 임시 경험
우선이며 없을 때 발행된 Main을 읽는다.

조회 receipt는 발행한 router에 묶인다. 다른 세션을 선택한 동안에는 그 receipt를 사용할
수 없고, 원래 세션으로 돌아오면 종료되지 않은 receipt를 사용할 수 있다. Main head가
바뀐 옛 Main receipt는 기존 규칙대로 거부한다. 세션 종료/Runtime 소멸은 구분되며,
소멸은 모든 세션을 active 그대로 디스크에 남긴다. 메모리 예산은 외부 receipt보다 오래 살아야 한다.

새 검사는 서로 다른 agent provider가 동일 native session ID를 쓸 때 분리됨, 세션별 같은
원문 보존, cross-session receipt 거부, 선택의 무디스크 접근, 세 번째 세션만 종료/병합,
선택되지 않은 두 활성 세션의 Main 조회, EOF 뒤 두 세션 재개와 독립 종료를 확인한다.

이 변경은 native Runtime 연결 기반이다. 현재 MCP 서버의 선택 receipt 상태와 실제
데스크톱 이벤트 수신은 아직 여러 세션용으로 연결되지 않았다. 실제 데스크톱 설치·전체
이벤트 수집·1ms 충족 증거로 사용하지 않는다.

## MCP host session contexts

Host input protocol version 5 binds each attached Runtime session to its own
Recall and completed Replay state. `swegca/attach` accepts identity/name;
`swegca/attach/resume` accepts identity. Both attach without changing selection.
`swegca/select` selects an attached identity without recording or ending it.
Existing start/resume still attach and select only when none is selected.

Receipt numbers are unique within one server process. Switching sessions preserves
parked receipts, while replay/candidates/re-evidence validate only the selected
session's receipt. Ending drops only that context; transport EOF does not end any
session. Receipts do not survive process restart. The host must serialize selection
and the corresponding operation on this stdio connection: these methods are host
routing, not authentication or concurrent multi-client transport. Models still have
only Replay and Re-evidence tools, with no session lifecycle tools.

Context allocation precedes Runtime attachment. Attachment failure removes the
new empty context and preserves prior selection/receipts. Selected receipts are
cleared before explicit end releases their borrowed Runtime resources. A failed
end may therefore invalidate that session's receipts, but never another session's.

The subprocess test covers two simultaneous agents, cross-session receipt rejection,
parked full Replay retention, attachment/selection failures, isolated end/merge,
EOF without end, explicit resume and post-restart Recall. Native desktop event
capture and delivery are still separate unfinished integration work.
