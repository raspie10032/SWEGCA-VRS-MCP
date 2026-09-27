# 관련 경험의 연결별 계층 선택

2026-09-27

## 구현

ExperienceRouter::related와 Runtime::related는 선택적 connection을 받는다.
명시한 연결의 적격 input-key 관측만 Recall 결과에 넣은 후 기존 코어
recall_scope로 임시/Main 경로를 결정한다. 임시 A 연결이 있다는 이유로
Main B 연결을 숨기지 않는다. 같은 연결의 적격 임시 경험은 강도나
support/refute/insufficient 값과 무관하게 임시 우선 규칙을 유지한다.
일반 대화처럼 input-key 없는 항목은 기존 core eligibility에 따라 제외된다.

MCP related:true,connection 조회가 이 경로를 사용한다. 저장된 cognition을
복구하는 기존 경로는 이미 선택된 원경험을 보존한다. 자연 입력의 Déjà vu/
Recall 앞에 처리나 I/O를 추가하지 않는다. 원문 Replay 전에 메타데이터만
필터링하고, 원문 선택은 기존 core prefer_replay로 수행한다.

## 검증

runtime lifecycle 2117 checks, 실제 stdio subprocess 7232 checks 통과.
병합된 Main의 A/B/미검증 관측과 별도 임시 A 관측을 연결한 라우터에서:

- A 조회는 임시의 insufficient 관측을 선택.
- B 조회는 Main의 반박 관측을 선택.
- 없는 연결은 빈 결과.
- 세 조회는 추가 pread/pwrite 없이 선택되며 이후 Replay 주소가 일치.

초기 테스트에서 active Runtime의 observe_input_scope로 Main 원경험을 직접
관측하려 했지만 session provenance 경계가 거부했다. 이 경계를 완화하지
않았다. 새 혼합 계층 fixture는 SessionRuntime의 명시적 input-key 기록과
실제 PersistentMainGraph를 연결한 ExperienceRouter 단계의 증거다.
상위 실제 에이전트에서 이 혼합 계층 조건을 만든 검증은 아직 없다.

## 남은 연결

connection을 지정하지 않은 목록/기본 related 경로는 기존 계층 단위 선택이다.
자동 전달이 Main의 빠진 연결까지 발견하려면 연결 목록의 계층별 페이지를
조합해야 한다. 이 변경만으로 관련 요구 전체 전달이 완성됐다고 하지 않는다.
현재 설치본에는 이번 변경을 아직 반영하지 않았다.
