# SWEGCA 코어 보강 — 2026-09-26

사용자 지시: 코어부터 보강하고, 한 공정이 ns 시간대에 실행되는 특성을 유지한다.

비교 기준은 이 저장소의 보존된 코어 커밋 `f720a4f`다. 작업 브랜치는
`codex/core-hardening-20260926`이다. 폐기된 C++ VRS 구현은 사용하지 않았다.

## 변경한 계약

| 확인된 문제 | 보강 내용 |
| --- | --- |
| 특정 축의 출처 다양성이 전체 출처 다양성보다 큰 모순된 입력을 허용 | 활성 축마다 포함관계를 검사하고 `abstain / invalid_input` 반환 |
| 공개 필드로 승인 결과를 직접 만들어 후속 코어 함수에 전달 가능 | `EvidenceJudgment`를 읽기 전용 접근자를 가진 값 타입으로 변경. 유효한 판정은 `judge_evidence`만 생성 |
| 실패한 단일 호출의 출력에 이전 승격 권한 또는 완료 단계가 남음 | 승격 함수는 `none / invalid_input / semantic_read_allowed=false`, 단계 함수는 `invalid`로 초기화 후 판정 |
| 호출자 컴파일 옵션으로 인라인 함수의 NaN/Inf 처리와 산술 의미가 달라짐 | 위험한 옵션을 헤더에서 컴파일 오류로 차단하고 빌드에 `-ffp-contract=off` 적용 |

정상 입력의 3상 판정식, 비교 순서, 수치 연산 순서와 기존 여섯 개 영속 트랜잭션 단계는
유지했다. `MemoryTransactionStage::invalid`는 실패 출력용 값이며 저장할 일곱 번째 단계가 아니다.
판정 경로에 할당, 잠금, I/O, 해시 계산 또는 가변 전역 상태를 추가하지 않았다.

`EvidenceJudgment`의 비공개 필드는 일반 C++ API 사용 중 결과를 잘못 조립하는 것을 막는다.
프로세스 내 메모리 변조를 방어하는 인증 수단은 아니다. Main은 여전히 입력의 출처,
현재 개정과의 일치 여부, 반증 검증, 저장 및 실행 권한을 소유한다.

## 호출부 변경

- 결과 읽기: `judgment.status`에서 `judgment.status()`로 변경. 다른 필드도 같은 이름의 접근자를 사용한다.
- `decide_memory_promotion`에는 상태와 이유 두 값을 따로 전달하던 호출을 제거하고
  `const EvidenceJudgment&`를 전달한다. 판정 후 다음 코어 함수에 그대로 연결한다.
- 배치 출력에는 선택적 `std::span<EvidenceJudgment> judgments`를 추가했다.
  호출자가 배열을 제공하면 같은 판정에서 나온 전체 결과를 받는다. 수치 열에서 결과를
  재조립하거나 판정을 두 번 실행할 필요가 없다. 배열 할당은 코어가 하지 않는다.
- 배치의 출력 크기·범위·중첩이 잘못된 경우는 기존처럼 **아무것도 쓰지 않고 `false`**를
  반환한다. 그 경우 이전 배열 내용을 새 결과로 소비하면 안 된다. 유효한 배열 안에서
  특정 입력 항목만 잘못된 경우에는 그 항목에 새 `invalid_input` 결과를 쓴다.
- 이 저장소의 VRS 호출부 수정은 `cpp/vrs/verification.hpp`의 `status()` 접근자 적용 한 줄이다.

## 검증

| 검사 | 결과 |
| --- | --- |
| 회귀·경계·후속 코어 연결 검사 | 3,951개 확인 통과. 반복 확인을 포함한 수이며 독립 시나리오 수가 아님 |
| 컴파일 계약 검사 | 정상 설정 1개와 차단되어야 하는 설정·호출 9개, 모두 예상대로 동작 |
| 정상 입력 수치 동등성 | 1–8축 × 신뢰도 설정 4개 × 입력 1,024개 = 32,768건 일치 |
| 비교 항목 | 상태, 이유, 모든 수치 결과의 binary64 비트, 다양성, 개정 정보 |
| 코어 반복 호출 중 동적 할당 | 검사한 경로에서 `operator new` 호출 0회. 소스에도 새 할당 경로 없음 |
| UBSan | `-fsanitize=undefined -fsanitize-undefined-trap-on-error` 빌드로 3,951개 확인 통과 |
| ASan | 미실행. 호스트의 `libasan.so.8.0.0`이 없어 링크 불가 |

회귀 검사에는 축별 입력 모순, NaN/Inf, 3상 결과, 성공 후 실패 출력, 여섯 단계의
모든 전이 조합, 스칼라/배치 결과, 배치 버퍼 중첩·범위, 기존 VRS 매핑을 포함한다.
컴파일 검사는 fast-math, finite-only, reciprocal, signed-zero 제거, 재결합 옵션과
결과 필드 수정·직접 승인 생성·원시 상태 전달을 검사한다.

정상 입력 비교 결과의 SHA-256:

```text
b520864f93b92f52fb6ea275e6832f14256df985893602774d16f654d97cac8d
```

32,768건의 동등성은 해당 입력 집합의 검증 결과다. 모든 입력 또는 다른 CPU/GPU/NPU의
수치 동등성을 증명한 것으로 확대하지 않는다.

## 공정별 시간

조건: AMD Ryzen 7 9800X3D, GCC 16.2.1 20260819, C++20,
`-O3 -march=native -ffp-contract=off`. 단일 스레드를 논리 CPU 6에 고정했다.
Palworld 서버는 실행 상태로 유지했다.

각 실행에서 100,000회 예열 후 100,000회 호출 묶음을 31번 측정했다.
실행 순서는 이전/이후/이후/이전/이전/이후다. 아래 값은 각 버전의 실행 3회에서
얻은 중앙값들의 중앙값이다. 측정 함수는 `noinline`이며 결과를 소비해 제거를 막았다.
입력 64개를 바꿔가며 승인·반려·기권이 포함되도록 했다.

| 공정 1회 | 보강 전 ns | 보강 후 ns |
| --- | ---: | ---: |
| SWEGCA 증거 판정, 1축 | 22.89 | 24.80 |
| SWEGCA 증거 판정, 4축 | 57.84 | 62.33 |
| SWEGCA 증거 판정, 8축 | 117.45 | 132.92 |
| 의미기억 승격 조건 비교 | 1.11 | 1.14 |
| 기억 승격 결정 | 1.42 | 1.73 |
| 트랜잭션 단계 전이 | 1.68 | 1.65 |

확인한 여섯 공정은 보강 후에도 ns 시간대를 유지했다. 이 측정에서 증거 판정 중앙값은
약 8–13% 증가했다. 추가 검사의 비용과 함께 서버 부하·CPU 주파수·스케줄링의 편차가
포함되므로 차이 전체를 코드 비용으로 단정하지 않는다. 속도 향상을 주장하지 않는다.

표의 값은 이미 메모리에 준비된 입력에 대한 **호출 묶음의 평균을 집계한 시간**이다.
반복문·호출·결과 소비 비용도 포함한다. 개별 호출의 최댓값이나 실시간 지연 보장은 아니다.
규칙 생성, 데이터 읽기, 입력 훅, MCP, 디스크 접근과 전체 VRS 흐름은 이 측정에 포함하지 않았다.
원본 측정의 `p95_block_mean_ns` 역시 개별 호출 지연의 p95가 아니다.

측정 원본과 요약은 `measurements/core-hardening-20260926.json`에 보관한다.
추가 로컬 비교 산출물은 다음 경로에 있다.

```text
/var/home/raspie/Documents/Codex/reviews/swegca-core-20260926/hardening/
```

## 재실행

저장소 루트에서 다음 명령을 실행한다. 컴파일에 필요한 것은 C++20 컴파일러와 make,
컴파일 계약 검사에 필요한 것은 표준 Python 3이다.

```sh
make -j2 check
make build/core-bench CXXFLAGS='-O3 -march=native'
taskset -c 6 build/core-bench
```

이 Makefile은 플래그 변경만으로 기존 바이너리를 자동 재빌드하지 않는다. 빌드 옵션을
변경했다면 `make clean` 후 다시 빌드한다. CPU 6은 이번 측정 환경의 선택이다.

UBSan 검사:

```sh
mkdir -p build/sanitized
g++ -std=c++20 -O1 -g -ffp-contract=off \
  -fsanitize=undefined -fsanitize-undefined-trap-on-error \
  -fno-omit-frame-pointer -Icpp tests/core_tests.cpp \
  cpp/swegca_architecture/evidence_rules.cpp \
  cpp/swegca_architecture/sha256.cpp \
  cpp/swegca_architecture/strong_types.cpp \
  -o build/sanitized/core-tests-ubsan-trap
build/sanitized/core-tests-ubsan-trap
```

헤더는 확인 가능한 위험 옵션을 컴파일 시 차단한다. 런타임의 반올림 모드와
FTZ/DAZ 설정을 호출마다 검사하지는 않는다. 호출자는 round-to-nearest-even과
서브노멀 보존을 유지해야 한다. 검증한 도구 체인은 위 GCC 환경이며 다른 도구 체인의
동일 동작을 이번 결과만으로 보장하지 않는다.
