# 봉인 경험 메타데이터 페이지 — 저장 단위 구현

지역 적재/퇴거에 사용할 제한 크기 저장 단위를 구현했다. 현재 ExperienceSequence와 MainGraph의 명시적 소유자 호출로 퇴거/재적재에
연결했다. 서비스의 자동 예산 대응·재시작 주소 복구는 아직 연결하지
않았다. 페이지 마지막 소유자 해제 시의 공간 회수는 아래에 기술한다. 이 구현을 전체 4GB 운용 완료로 해석하지 않는다.

## 검증 경계

ExperiencePage::create는 이미 원문 검증을 거친 ExperienceEvidence만 입력받는다.
경로만 지정해 임의 페이지를 다시 여는 공개 API는 없다. 생성한 핸들은 실제
ExperienceBlock 쓰기가 반환한 정확한 레코드 주소와 체크섬을 보유한다.
디스크 읽기는 이 주소로 전체 페이지를 인증하고 기존 SWEGCA admit_observation을
통과한 값만 반환한다. 외부에서 임의 관측값/강도/판정을 설치하는 API가 아니다.

페이지는 최대 256개 경험의 메타데이터를 보존한다. 원경험 블록/오프셋/크기/digest,
hypothesis/source/context/producer, 관측·만료 시각, confidence, axis, outcome,
expiry/input-key 플래그, 정확 cue를 고정 길이 little-endian 필드로 저장한다.
관측 address는 원경험 digest에서 복원한다. C++ 구조체의 padding/ABI를 저장하지 않는다.
원문과 연결 강도를 복제하거나 메타데이터 페이지를 새 경험으로 세지 않는다.

기존 ExperienceBlock의 체크섬·동기화·I/O 계수·StorageBudget 제한을 사용한다.
단일 값 읽기는 제한 크기 페이지 전체를 검증한다. load는 한 번의 페이지 읽기로
전체 구간을 복원하여 향후 구간 캐시가 항목마다 반복 I/O를 할 필요가 없도록 한다.
출력은 완전한 검증 후에만 반환되며 임시 버퍼도 VRS MemoryBudget을 사용한다.

## 검사와 다음 연결 작업

make build/experience-page-tests / build/experience-page-tests로 검증한다.
256개 값의 모든 필드와 3종 관측 결과, explicit/derived cue, expiry, axis, confidence,
페이지 이동, 인덱스 범위, 예산 부족, 다른 admission 조건, 선택 밖의 손상을 검사한다.
단일 읽기는 80,000바이트 예산, 전체 load는 200,000바이트 예산에서 수행했다.
페이지 손상 시 부분 결과가 반환되지 않고 추적 메모리가 해제됨을 확인했다.

다음 구현은 ExperienceSequence의 봉인 구간과 이 페이지를 연결하고, 구간 핀의
수명을 보존하면서 Main 소유자가 상주 구간을 퇴거·적재하도록 만드는 것이다.
원문 checksum/admission을 우회하거나 입력→Recall 앞에 디스크 적재를 넣어서는 안 된다.
재시작 시 페이지 주소의 영속 소유권과 정리/회수도 이 연결 작업에 포함된다.


## 봉인 구간의 실제 퇴거·적재

MainGraph::page_out은 선택한 연결/원경험 번호가 속한 구간을 대상으로 한다.
기존 SWEGCA 코어와 같은 순수 함수 계층의 metadata_release가 완전 봉인 여부,
구간 소유자 수, 검증된 페이지 존재 여부로 유지/저장 후 해제/재해제를 결정한다.
연결 강도나 참/거짓을 판단하는 함수가 아니다.

부분 tail, 공유 연결, Recall 핀/스냅샷이 잡힌 구간은 해제하지 않는다. 완전히
봉인된 단독 구간은 저장 성공 후에만 RAM을 해제한다. 메타데이터 접근 시
페이지 전체 인증·기존 admission을 거쳐 복원한다. 동시에 복원하는 독자는
한 번만 RAM을 발행하도록 mutex와 acquire/release 포인터를 사용한다.
페이지 관리 객체는 첫 퇴거 때 VRS 예산으로 할당하며 일반 구간에 정책/파일 핸들을
상주시켜 두지 않는다. 적재 실패 시 구간은 cold 상태로 남아 재시도가 가능하다.

명시적인 퇴거는 Main 소유자가 입력/병합 준비/빌린 참조 사용과 겹치지 않게 호출해야
한다. 구간 스냅샷 자체는 읽기를 수행하지 않으며 실제 메타데이터 적재는 Recall 진입
뒤의 접근에서 발생한다. 원경험의 디스크 주소와 순서, Main 세대와 강도는 바뀌지 않는다.

검사: 페이지/구간 1,595개, Main 그래프 4,713개, 세션 런타임 1,151개, 비동기 91개.
Main 구간 퇴거의 추적 메모리 감소, 동일 원주소 Replay, 재퇴거 시 저장 증가 없음,
핀이 잡힌 구간 보호, 공유 prefix, 동시 복원, 예산 부족 후 재시도를 확인했다.
CPU 6,7 코어 벤치에서 metadata_release 중앙값 1.78751ns였다. 함수 호출·루프를
포함한 단독 측정이며 디스크 I/O나 전체 입력→Recall 지연의 증거는 아니다.

서비스 자동화, 보존된 페이지 주소의 재시작 복구, 파생 파일 회수, 전체 상주 예산 및
단일 큰 연결 셔플 중의 적재/퇴거 조절은 아직 미완료다. 아래의 마지막 소유자 회수 외에
파일을 일괄 삭제하는 동작은 없다.

## Runtime 소유자 연결

Runtime::page_out_main(connection, original_index)은 서비스 소유자의 명시적 유지관리
호출이다. 진행 중인 Main 작업은 준비 완료 여부와 무관하게 poll_work로 정리될 때까지
퇴거를 금지한다. 구간 해제 여부는 기존 metadata_release가 결정하며, 자연 입력 앞에
이 호출을 끼워 넣지 않는다. 저장 파일은 Runtime 루트의 metadata-pages 아래에
고유 이름으로 만들고 기존 파일을 덮어쓰지 않는다. 페이지 쓰기와 읽기는 같은
StorageBudget/TransferBudget을 사용한다.

저장 실패는 resident 원경험을 유지한다. 실패한 파일의 시도 용량은 기존 규칙대로
보수적으로 계수하며, 재시작 시 실제 파일 길이로 조정한다. 재시작은 원경험 저장소와
병합 저널로 Main을 복구한다. 남아 있는 파생 페이지도 저장 용량에는 포함되지만,
그 페이지의 주소를 다시 채택하는 캐시 복구는 아직 구현하지 않았다.

Runtime 검사는 실제 종료·병합 후 핀 보호, 쓰기 실패/재시도, 메모리 감소,
저장·I/O 계수, cold 구간의 무읽기 Recall, 동일 원주소 Replay, backing 재사용 및
재시작 원경험 복구를 확인한다. 비동기 검사는 준비 워커를 실제로 정지시킨 상태에서
퇴거가 파일 생성 전에 거부되는지 확인한다. 자동 압력 대응·페이지 회수는 다음 범위다.


## 마지막 페이지 소유자 해제 시 회수

ExperiencePage는 파생 파일만 소유하며, 마지막 핸들의 소멸/이동 대입 때 회수를
시도한다. 공유 구간과 Recall 스냅샷의 수명은 그대로 적용된다. SWEGCA 코어의
`discard_metadata_page`는 원래 소유 inode 일치와 단독 링크 조건으로 허용한다.
VRS는 열린 파일과 현재 경로를 대조하고 기록 완료 길이도 일치해야 삭제한다.
이름이 바뀌었거나 symlink/하드 링크/크기 변경/시스템 호출 실패가 있으면 보존한다.
성공한 unlink 뒤 파일을 닫고 해당 바이트만 StorageBudget에 반환한다.
원경험 ExperienceBlock의 일반 소멸 동작은 그대로 파일을 보존한다.

실패한 create/append와 비정상 종료의 잔여 파일은 이 소유자 회수 대상이 아니다.
해당 파일은 재시작 시 용량에 포함되며 별도의 검증된 정리 경로가 필요하다.
파생 페이지의 재시작 재사용도 아직 구현하지 않았다.

## 복원 중 중복 배열 제거

cold 구간 복원은 최종 ExperienceEvidence 배열을 먼저 예약하고, 인증된 페이지에서
각 값을 해당 배열에 직접 생성한다. 이전의 임시 decoded vector와 최종 배열 사이
복사를 제거했다. 모든 값의 admission이 성공한 뒤에만 atomic 포인터를 공개한다.
읽기/할당/디코딩 실패는 생성된 prefix와 최종 배열을 해제하고 cold 상태를 유지한다.

128개 구간 검사는 `최종 배열 + 인코딩된 레코드`만 남긴 공유 예산에서 복원하며,
이 예산이 두 decoded 배열보다 작음을 확인한다. 최종 배열만 예약할 수 있어 입력
버퍼가 부족한 경우에는 실패 후 예산이 정확히 반환되고 다음 복원이 가능해야 한다.
전체 페이지 인증과 원주소·관측 검증을 생략하지 않는다. 자동 퇴거 스케줄링은 별도다.

## 유휴 시간의 자동 메모리 압력 대응

Runtime::maintain_memory는 한 호출에서 최대 한 봉인 구간만 검사한다. 주소 지역의
lower_bound와 저장된 연결/구간 커서로 다음 위치를 찾으며 매번 전체 그래프를
복사하거나 처음부터 순회하지 않는다. SWEGCA metadata_pressure가 사용량과 목표,
Main 준비 상태로 실행 여부를 결정한다. metadata_release와 metadata_page_beneficial로
핀/공유/미봉인 구간 및 관리 객체보다 작은 구간을 유지한다. 작은 구간의 무익한
페이지화로 RAM 사용이 늘어나지 않게 한다.

기본 목표는 호출자가 정한 MemoryBudget의 75%다. 설정의 memoryTargetBytes로
바꿀 수 있으며 0은 기본값이다. 코어가 4GB 같은 자원 상수를 소유하지 않는다.
목표 이하 또는 한 순회가 끝나면 멈춘다. 페이지 생성 중 OOM은 원래 구간을 유지하고
그 유휴 작업을 끝낸다. 저장 예산 부족도 원래 구간을 유지하고 해당 유휴 작업을 끝낸다. 다른 저장 오류는
기존 호스트 background 오류 처리로 보고된다. 강제 메모리 확보나 임의의 경험 삭제를 수행하지 않는다.

stdio 호스트는 초기화 뒤 완전한 프레임 사이, staging과 읽을 입력이 없을 때만
호출한다. 계속할 작업이 있을 때만 10ms 간격으로 다시 확인한다. automaticWork
설정 없이도 메모리 관리는 작동하지만, 수동으로 예약된 Main 작업을 자동 발행하지
않는다. 명시적 세션 종료 조건은 변하지 않는다.

페이지 쓰기는 아래의 워커 준비/소유자 공개 경로로 분리했다. 보장된 1ms, 자동 대규모 그래프 4GB 운용,
단일 대형 연결의 셔플 상한 달성으로 해석하지 않는다. 색인/연결 디렉터리와 임시
세션 메모리는 이 구간 퇴거의 대상이 아니며 사용 중인 핀이 많으면 목표에 못 미친다.


## 비동기 페이지 쓰기

ExperienceSequence::PagePreparation은 완전 봉인 구간의 공유 소유권을 잡는다.
워커는 이 고정된 resident 값으로 새 페이지와 비공개 Backing만 만든다. 구간의
data/backing을 바꾸지 않으므로 기존 Recall/Replay와 Main 병합 준비가 계속 읽을
수 있다. 단일 Runtime은 페이지 워커를 최대 하나만 유지한다.

유휴 소유자가 완료 flag를 acquire로 확인하고 join한 뒤, Main 준비 작업이 없는
상태에서 backing을 공개한다. 그동안 새 핀이 생겼으면 metadata_release 판정으로
RAM을 유지한다. 핀이 없으면 해제한다. 작업 시작 실패/worker OOM은 원래 구간을
유지하며 준비 객체는 미공개 페이지와 메모리를 회수한다. Runtime 소멸은 워커를
join하고 준비를 폐기하며, 세션 종료나 Main 병합으로 해석하지 않는다.

페이지 경로 선택의 디렉터리 상태 확인 및 이름 충돌 확인도 워커에서 한다. 소유자는
Main head와 연결/인덱스/시도 번호를 묶은 식별자를 확정해 전달한다. 이름이 이미
존재하면 같은 seed에서 충돌 번호를 파생하여 기존 파일을 보존한다. 선택 Replay의
cold 페이지 읽기는 동기식이며 전체 입력 1ms 달성을 입증한 것은 아니다.

## 셔플·병합의 제한 크기 읽기 버퍼

ExperienceSequence::Reader는 worker가 소유하는 최대 한 페이지의 decoded 버퍼다.
이미 상주한 구간은 그대로 읽고 cold 구간은 전체 인증/admission 뒤 읽기 버퍼로
복원한다. 다음 cold 페이지로 바꾸기 전에 이전 버퍼를 반환한다. 읽기 결과를 공유
구간 data로 공개하지 않으므로 검증 한 번이 모든 cold 경험을 다시 상주시킬 필요가 없다.
소유자가 자료를 바꾸지 않는 기존 병합 준비/직렬화 계약을 따른다.

Connection의 상속 검증, 실제 셔플의 두 집계 순회 및 Main 결과의 원주소 digest
계산에 연결했다. Fisher–Yates 표본 순서, 새 집계, 그룹 합산 순서와 3상 판정은
유지한다. 축 다양성 순회는 applied가 아닌 표본의 이미 확인된 사용 결과를 먼저
확인하여 불필요한 페이지 읽기를 생략한다.

캐시가 작은 만큼 무작위 셔플이 여러 cold 페이지 사이를 오가면 반복 디스크 읽기가
늘 수 있다. 속도 향상을 주장하지 않는다. 전체 shuffle sample 배열·dedup 집합·그룹
집합은 여전히 경험 수에 따라 커지므로 전체 대형 연결의 4GB 완료도 아니다.

## Runtime보다 늦게 해제되는 페이지의 계수 수명

Recall 스냅샷은 구간 소유권을 유지할 수 있으므로 Runtime 소멸 뒤 스냅샷을
파괴하는 경로도 안전해야 한다. 페이지 회수에서 StorageBudget의 raw 주소를
다시 사용하던 오류를 재현했다. 같은 주소에 새 Budget을 생성하면 이전 페이지의
해제가 새 예산 사용량을 차감했다.

StorageBudget의 VRS 사용량 계수 상태를 공유 소유하고, 페이지는 생성 시의 해당
계수 상태만 보유한다. 마지막 페이지 해제는 소멸한 Runtime이나 이후 Budget을
참조하지 않는다. Budget당 계수/제어 블록 하나, 페이지당 공유 핸들 비용이 추가된다.
이 변경은 늦은 파괴를 안전하게 하며, 만료된 Recall에 새 읽기/판정 권한을 부여하지
않는다. 경험/디렉터리 MemoryBudget의 기존 수명 계약도 그대로다.

## Failed append ownership

A newly created derived page now owns its ExperienceBlock immediately after the
header is created, before reserving/appending the record. If record reservation
fails, stack unwinding runs the same core `discard_metadata_page` predicate and
same-inode, sole-link, exact-extent checks as normal page destruction. Successful
unlink closes the descriptor and releases only that page's original counter.
No original block is removed, no session ends, and no Main merge is scheduled.

Regression: with a storage budget of exactly one block header, the header write
succeeds and the page record reservation throws StorageLimit. Previously the
header file and its charge remained. Three failures now leave no page file and
zero page-budget usage while preserving original storage. The pre-fix test failed
at this condition; page suite after the fix passes 1,658 checks.

This only covers failures after a valid header has been returned and before an
unexpected physical extent exists. Header-creation I/O failure, partial record
writes, process termination and restart orphan reconciliation remain unfinished.
Unexpected extents, shared links and replaced names remain conservatively kept.
