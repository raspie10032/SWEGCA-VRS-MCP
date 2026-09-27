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

## 연결 목록 조합 구현 및 설치

후속 구현으로 Runtime/ExperienceRouter::related_connections가 임시와 Main의
적격 연결 목록을 조합한다. 같은 연결은 core recall_scope에 따라 임시 우선,
임시에 없는 연결은 Main 선택이다. 원경험 선택은 기존 prefer_replay를 사용한다.
connection을 지정하지 않은 단일 related Replay는 기존 동작을 유지하지만,
자동 다중 전달은 새 목록과 연결별 Replay를 사용한다.

출력 각 연결에 temporary를 추가했다. 최상위 temporary는 이 페이지의 모든
항목이 임시일 때 true이며, 두 계층이 섞였으면 mixedTiers=true다. 빈 페이지는
둘 다 false다. relatedCoverage에도 같은 메타데이터가 전달된다. 이는 경험이
어디서 왔는지의 표시이며 요구 완료나 실행 권한이 아니다.

두 계층의 Recall 메타데이터로 snapshot을 계산하고 모든 페이지에서 같은
snapshot을 유지한다. 변경된 목록의 기존 snapshot은 기존 MCP 검증이 거부한다.
정렬된 연결 ID는 페이지 순서뿐이며 의미 우선순위가 아니다. 추가 ID 집합은
페이지 limit 이내로 유지한다. Recall 메타데이터 자체와 스캔 시간은 전체 관련
경험 규모에 의존하므로 이 변경은 대규모 메모리/5ms 조건의 완료 증거가 아니다.

검증: runtime 2137 checks, stdio 7240 checks. 실제 Main과 임시를 함께 연결한
라우터에서 A 중복 제거/임시 선택, Main B/C 포함, 한 항목 페이지와 전체 페이지
일치, 원문 I/O 없음, 뒤에 온 임시 관측에 의한 snapshot 변경을 확인했다.
실제 transport/자동 전달 fixture는 각각 임시 또는 Main 단일 계층의 필드 전달을
확인했다. 상위 실제 에이전트의 혼합 계층 입력 생성 검증은 계속 남아 있다.

설치본 VRS 바이너리까지 원자 교체했다. 전체 executable manifest 해시 검증,
실행 중 설치 프로세스 부재, 기존 설정 byte 동일성을 확인했다. 설치된 실제
backend smoke에서 같은 소유자/소켓/I/O 그룹, memory.max=3999997952,
swap=0, CPU6-7, modelCalls=0, 정상 종료와 socket 정리를 확인했다.

## 페이지 후보 선택의 반복 탐색 제거

페이지의 각 연결마다 모든 Recall context를 재순회하던 경로를 변경했다.
첫 순회에서 기존 snapshot과 limit 이내 연결 ID 집합을 만들고, 두 번째 순회에서
연속 메모리의 해당 연결별 후보에 기존 core prefer_replay를 적용한다.
context 순서, sealed-page reduction, 동일 점수의 tie-breaking은 유지한다.
단일 연결 선택도 같은 context reduction 함수를 사용한다.

혼합 계층 목록은 각 계층의 첫 limit개 페이지를 조합한다. 각 prefix 밖의
ID는 합집합의 첫 limit개에 들어갈 수 없으므로 결과를 누락하지 않는다.
이제 snapshot을 얻기 위한 별도 후보 선택과 연결별 전체 재탐색이 없다.
페이지 크기 L, context 수 C에서 후보 연결 탐색은 C*L 반복 대신 C*log(L)의
bounded lookup이다. snapshot과 Recall 생성은 여전히 관련 메타데이터를 훑는다.

benchmarks/related_pages_bench.cpp 및 Makefile target related-pages-bench 추가.
1024개 단일 계층 연결, limit64, warmup8/측정128회, CPU7에서:

- 변경 전 a955320: 평균 434587ns.
- 최초 map 후보 누적 시도: 460541ns. 개선 확인 실패.
- 최종 bounded contiguous reduction: 412830ns.
- 세 경우 측정 전후 residentBytes=1380446, checksum8192 동일.

이는 시스템 부하를 통제한 반복 A/B나 대규모 성능 보장이 아니며, 약 5% 차이를
안정적 개선율로 주장하지 않는다. 원본 측정값은
benchmarks/results/related-pages-reduction-20260927.jsonl에 보존했다.
이 시간은 related 목록 전체이며 사용자 입력→Recall 5ms 또는 코어 ns 측정이 아니다.

검증: runtime2141/stdio7236 checks 통과. 페이지 후보가 단일 연결 core 선택과
동일한지, hard storage limit에서 31개 경험 중 같은 마지막 원경험을 선택하는지,
기존 혼합 계층/페이지/변경된 snapshot 검증을 포함한다. 이번 최적화 소스는
로컬 커밋으로 보존하며 설치본은 앞선 a955320 기능 버전을 유지한다.

## 실제 전달 원경험의 주소 보존

relatedCoverage.connections는 현재 목록 후보이고, 저장된 cognition을 복구한
related Replay는 이전에 선택한 원경험을 유지할 수 있다. 연결 ID가 같다고
현재 목록의 주소를 실제 전달 주소로 해석하면 안 된다.

replay_context는 검증된 actual Replay로부터 deliveredExperiences를 구성한다.
각 항목에 connection/original/matchesListedOriginal을 기록한다. 마지막 boolean은
원경험 주소의 일치만 나타내며 의미 적합성/요구 충족/신뢰 판정이 아니다.
목록과 Replay가 다를 때도 선택 계보를 바꾸거나 최신 후보로 대체하지 않는다.
전달 개수/선언 ID/목록에 없는 연결/중복 전달을 검증하고 일치하지 않으면 거부한다.
예산 때문에 전달하지 않은 항목은 실제 전달 배열에 포함되지 않는다.
원문 읽기나 후보 선택은 추가하지 않으며 기존 실제 Replay를 표현한다.

wire204/stdio7299 checks 통과. 실제 프로세스에서 후속 B 관측으로 목록 후보가
변경돼도 저장된 Replay는 이전 B 원경험을 유지하는 경우를 확인했다. 그 주소가
다른 경우의 표현은 wire 검증, 기본/개수 제한/바이트0 자동 전달은 실제
wrapper/host/proxy/VRS 경로에서 확인했다. 모든 자연어 요구의 충분성 검증은
계속 미완료이며 requirementsComplete=false 규칙은 유지된다.

proxy 설치본 원자 교체, 전체 manifest 해시 검증, 기존 설정 동일성 확인.
설치 backend smoke는 같은 소유자/조회 socket/자원 그룹/I/O owner와
modelCalls0/exit0을 확인했다. 실제 GUI에서 목적 유지 평가를 수행한 증거는 아니다.
