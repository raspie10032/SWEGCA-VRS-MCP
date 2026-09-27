# 재시작 전후 대화 연속 키 불일치

2026-09-27. 미해결 기능 결함이며, 재현 프로그램은 일반 통과 검사에 포함하지 않는다.

## 재현 명령

```sh
taskset -c 6,7 make -j2 build/continuation-recovery-probe
taskset -c 7 build/continuation-recovery-probe
```

`benchmarks/continuation_recovery_probe.cpp`는 실제 Runtime/SessionStore/Main을
격리 임시 디렉터리에서 사용한다. 자연 입력과 그 입력에 연결된 반박 원경험을
기록한 뒤 related Replay한다. 다른 문구의 후속 입력이 선택하는 원경험을
프로세스 상태 폐기 전후에 비교한다. 명시적 종료나 Main 병합은 하지 않는다.

관측 결과와 종료 상태:

```json
{"continuationPreserved":false,"beforeKeyKind":2,"afterKeyKind":3,"beforeWasObservation":true,"afterWasObservation":false,"afterWasInput":true,"mainGeneration":0,"modelCalls":0}
```

종료 코드 2. 재시작 전에는 continuation 키로 반박 관측을 선택하고,
재개 후에는 session context seed로 사용자 입력을 선택했다.
이것은 원경험 유실이 아니라 마지막 회상 위치의 연속성 유실이다.

## 코드 근거와 수정 경계

- `ExperienceRouter::replay`는 인증된 원경험을 읽은 뒤 `continuation_`과
  `continued_context_`를 갱신한다. 현재 이 두 값은 라우터 메모리에만 있다.
- `Runtime::Active`의 재구성은 새 라우터를 만들고 Main을 mount한다.
- `restore_cognition`은 지정된 journal의 Replay를 복원할 수 있지만,
  어떤 Replay가 세션의 마지막 회상 위치였는지를 자동 선택하는 영속 기준점은 없다.
- 최신 파일 시간, 마지막 사용자 입력, 특정 scope/related journal의 존재만으로
  마지막 선택을 추측하면 안 된다. 정확히 성공한 Replay 위치를 보존하고
  원경험·소유 세션·계보를 검증해 복원해야 한다.
- 복구 I/O를 매 사용자 입력의 Déjà vu 앞에 삽입하지 않는다. 세션 부착/재개
  단계에서 완료하고, EOF를 세션 종료나 Main 병합으로 바꾸지 않는다.

원본 SWEGCA의 `HotExperienceIndex` semantic key는 호출자 제공 posting과
아티팩트 경로 토큰의 색인이다. 이를 일반 자연어 이해 판정기로 해석하지 않는다.
현재 문제를 고쳐도 일반 명제의 의미 적합성 전체를 해결한 것으로 보고하지 않는다.
