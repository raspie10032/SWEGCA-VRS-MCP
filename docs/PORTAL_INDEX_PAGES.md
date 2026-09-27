# Main 포털 구간의 파생 페이지

## 저장 단위

`PortalPage`는 cue 또는 context 한 키의 연결 구간을 최대 256개 보관한다.
각 구간은 connection digest와 반개구간 `[begin,end)`이며 원경험 본문,
SWEGCA 판정값, 연결 강도를 복사하지 않는다. 페이지 header는 종류/키/count를
결속한다. 기존 ExperienceBlock의 체크섬과 VRS MemoryBudget/StorageBudget,
공유 TransferBudget 경로로 읽고 쓴다. 새 저장 엔진이나 외부 판정기는 없다.

기존 SWEGCA `region_partition_end`가 한 페이지의 크기를 확인한다.
`portal_range_valid`와 `portal_range_follows`가 비어 있지 않은 연결·구간,
연결 순서·동일 연결의 비중첩을 확인한다. 이는 주소 구조 검증이며 경험 진실성
판정이 아니다. 상위 Main은 실제 연결/관측 경계와 대조한 뒤 Recall 영수증을
발행해야 한다. 인접 구간도 유효하며 저장 과정에서 원경험 수를 바꾸지 않는다.

내용은 고정 56바이트 header와 구간당 48바이트다. 전체 페이지의 checksum과
header/구간을 검증한 뒤 PMR vector를 반환한다. 사용자 입력의 Recall 앞에
페이지 읽기를 추가하지 않았으며 아직 실제 Main 조회에 연결하지 않았다.

## 소유와 실패 처리

페이지는 원경험과 별개인 파생 블록이며 경로만으로 임의 reopen하는 API는 없다.
자신이 생성해 얻은 주소만 읽는다. 마지막 핸들 소멸 시 기존 core의
`discard_metadata_page`로 자신의 동일 inode/단일 링크인지 확인하고 삭제한다.
부분 쓰기 실패의 예약량은 자신이 소유한 파일을 제거한 뒤 원래 저장 예산으로
반환한다. 다른 파일로 교체된 이름, 이동된 원래 파일, 하드링크는 삭제하거나
예산에서 임의 차감하지 않는다. 원경험 저장소의 파일은 소유하지 않는다.

Main 색인 퇴거/복구에 통합하기 전에는 crash orphan 정리와 페이지 디렉터리의
Main 소유권도 연결해야 한다. 현재는 primitive 검사 경로에서만 사용한다.
이 구현만으로 전체 Main 색인이 퇴거하거나 4GB 운용이 완성된 것은 아니다.

## 검증

CPU 6,7 / make -j2 소스 빌드, `portal-page-tests` 566 checks 통과.

- cue/context 최대 페이지 왕복, move, PMR 계수와 해제
- 빈/과대/빈 연결/중첩/역순/잘못된 종류·키 거부
- 인접 구간과 uint64 최대 경계의 정확한 보존
- 읽기 메모리 부족, 저장 한도, 본문 0바이트/13바이트 후 쓰기 실패
- checksum 손상 거부와 실패 후 메모리 반환
- 기존 파일 덮어쓰기 거부, 하드링크/교체된 이름/이동된 inode 보존

`taskset -c 6 build/core-bench`의 portal_range_order: median block mean
3.85823ns, p95 block mean 4.55034ns. warmup 후 100000회×31블록이며 호출별
최악 지연 또는 디스크 페이지 적재 시간의 측정은 아니다. 다양한 별도 구간 객체의
연결/경계 값을 비인라인 함수에 전달한다. 핵심 증거 판정식은 변경하지 않았다.
