# 재시작 전후 대화 연속 키 불일치

2026-09-27. 아래 최초 결함은 정확한 Replay 위치 영속화와 부착 시 복원으로 수정했다.
재현 프로그램은 독립 검사로 유지한다. 설치 VRS 바이너리와 manifest에도 반영했다.

## 재현 명령

```sh
taskset -c 6,7 make -j2 build/continuation-recovery-probe
taskset -c 7 build/continuation-recovery-probe
```

`benchmarks/continuation_recovery_probe.cpp`는 실제 Runtime/SessionStore/Main을
격리 임시 디렉터리에서 사용한다. 자연 입력과 그 입력에 연결된 반박 원경험을
기록한 뒤 related Replay한다. 다른 문구의 후속 입력이 선택하는 원경험을
프로세스 상태 폐기 전후에 비교한다. 명시적 종료나 Main 병합은 하지 않는다.

수정 전 관측 결과와 종료 상태:

```json
{"continuationPreserved":false,"beforeKeyKind":2,"afterKeyKind":3,"beforeWasObservation":true,"afterWasObservation":false,"afterWasInput":true,"mainGeneration":0,"modelCalls":0}
```

종료 코드 2. 재시작 전에는 continuation 키로 반박 관측을 선택하고,
재개 후에는 session context seed로 사용자 입력을 선택했다.
이것은 원경험 유실이 아니라 마지막 회상 위치의 연속성 유실이다.

## 코드 근거와 수정 경계

- `ExperienceRouter::replay`는 인증된 원경험을 읽은 뒤 `continuation_`과
  `continued_context_`를 갱신한다. 수정 전에는 이 두 값이 라우터 메모리에만 있었다.
- `Runtime::Active`의 재구성은 새 라우터를 만들고 Main을 mount한다.
- `restore_cognition`은 지정된 journal의 Replay를 복원할 수 있지만,
  어떤 Replay가 세션의 마지막 회상 위치였는지를 자동 선택하는 영속 기준점은 없었다.
- 최신 파일 시간, 마지막 사용자 입력, 특정 scope/related journal의 존재만으로
  마지막 선택을 추측하면 안 된다. 정확히 성공한 Replay 위치를 보존하고
  원경험·소유 세션·계보를 검증해 복원해야 한다.
- 복구 I/O를 매 사용자 입력의 Déjà vu 앞에 삽입하지 않는다. 세션 부착/재개
  단계에서 완료하고, EOF를 세션 종료나 Main 병합으로 바꾸지 않는다.

원본 SWEGCA의 `HotExperienceIndex` semantic key는 호출자 제공 posting과
아티팩트 경로 토큰의 색인이다. 이를 일반 자연어 이해 판정기로 해석하지 않는다.
현재 문제를 고쳐도 일반 명제의 의미 적합성 전체를 해결한 것으로 보고하지 않는다.

## 수정 및 검증

- 성공한 Replay의 출처·연결·원경험 주소·인덱스를 세션 소유 ExperienceBlock에
  기록하고, 완성된 블록의 현재 별칭을 원자적으로 발행한다. 같은 위치는 쓰지 않는다.
- 부착 시 원경험 계보와 주소를 검증하여 선택한 원문 하나에서 연속 키를 복원한다.
  입력마다 후보를 재선택하거나 복구 I/O를 앞세우지 않는다. 판정식/강도는 변경하지 않는다.
- Runtime 검사 2085개 통과. 임시/Main 복구, 쓰기 실패 후 이전 위치 유지,
  위조 출처/인덱스 거부, 동일 위치 재조회 무쓰기, 복구 시 병합 없음 포함.
- 실제 stdio 검사 6765개 통과. 응답 완료된 동일 저장 상태를 복제하여,
  계속 실행한 분기와 재시작한 분기에 동일한 새 입력을 전달했다. 과거 이벤트
  재전송 없이 후보/선택 원경험/판정이 일치한다. 복제는 시험 전용이며 런타임 기능이 아니다.
- 독립 재현 종료0: continuationPreserved=true, beforeKeyKind=2, afterKeyKind=2,
  beforeWasObservation=true, afterWasObservation=true, afterWasInput=false,
  mainGeneration=0, modelCalls=0.
- 한계: 서로 다른 회상 위치의 블록은 저장 한도에 포함된다. 저장 공간이 완전히
  찬 상태에서 새 위치의 영속화는 StorageLimit으로 실패하고 세션은 복구를 요구한다.
  이전 위치와 읽기 전용 원경험 접근은 보존된다. 한도를 늘려 검사를 통과시키지 않았다.
- 일반 자연어 의미 적합성, 전체 입력 1ms, 대규모 Main 메모리 제한은 별도 미완료다.

설치 검증: 기존 실행 파일 해시/실행 중 프로세스 확인 후 교체, 설정 보존.
실제 backend 초기화에서 도구 등록·동일 소유 조회 소켓·공유 I/O·cgroup·정상 종료를
확인했다. memory.max=3999997952, swap=0, CPU6-7, modelCalls=0.
사용자 GUI를 재시작하거나 실제 모델 대화를 실행하지 않았다.
