# 셔플 그룹 키 중복 제거

실제 셔플 순회가 처음 만난 (source, context, axis) 순서를 유지한다. 이전 구현은
해시 맵에 GroupKey→vector index를 두고 vector에 같은 GroupKey와 집계를 다시
저장했다. 현재 구현은 맵의 GroupKey→집계 노드 주소를 순서 vector에 저장한다.
표준 unordered_map 노드는 rehash 때 주소가 유지되며 삭제는 집계가 끝난 뒤에만
일어난다. 순서 vector가 먼저 파괴되고 맵은 뒤에 파괴된다.

그룹의 최초 관측 순서, support/refute 누적, normalize_evidence_group 호출 및
축별 부동소수점 합산 순서를 유지한다. shuffle, 관측 admission, 3상 판정과
연결 강도 연산은 변경하지 않았다. 외부 집계/판정 로직을 추가하지 않았다.

connection-tests의 512개 그룹/1,536개 실제 경험, seed 991 측정으로 전후 VRS
MemoryBudget 최대 추가 예약량과 refinement digest를 기록한다. 전후 자료는
benchmarks/results/group-memory-{before,after}.txt. RSS/전체 프로세스 메모리나
속도 측정은 아니다. sample/dedup/group의 선형 크기 자체를 없앤 구현은 아니다.

검증 결과: 상주 예약량은 전후 559,944바이트로 같고, 집계 중 최대 추가 예약량은
255,310→220,494바이트(34,816바이트, 약 13.6% 감소)였다. 결과 digest는 전후
c9360c43c9471740ad940712378edf1dc615987479e492782fbb5bb6d81d8332로 같다.
연결 검사 15,967개(실패 주입 246지점), Main 4,719개(실패 주입 914지점) 통과.

## 표본 열 분리

ConnectionRefinement는 셔플된 uint32 경험 번호와 ObservationUse를 별도 PMR 배열로
보존한다. 기존 {uint32, uint8} 구조체 배열의 정렬 여백을 없앴다. samples()는
동일한 두 값을 반환하는 읽기 뷰이며 size/index/순회 인터페이스를 제공한다.
연속 구조체 span은 더 이상 노출하지 않는다. 실제 Fisher–Yates는 같은 번호를
같은 순서로 교환하고 admission은 해당 순번에 기록한다.

canonical digest의 표본당 두 little-endian uint64 인코딩은 변경하지 않았다.
모든 기존 digest 벡터와 512그룹/1,536표본의 digest가 유지됐다. 해당 측정의 최대
추가 예약량은 직전 220,494→215,886바이트로 4,608바이트 감소했다. 표본당 3바이트의
여백 제거와 일치한다. 결과는 sample-columns-memory.txt에 보존했다.
연결 15,971개(실패 주입 247지점), 영속 연결 77개 검사 통과.
표본 저장은 여전히 O(n)이며 디스크 기반 표본 또는 전체 4GB 운용 완성은 아니다.

## 다양성 집계 작업 공간의 수명

첫 admission 순회는 주소 중복 집합과 producer 집합만 유지한다. source/context는
실제로 applied된 그룹 키에 이미 정확히 보존되어 있다. admission을 끝낸 뒤 주소
중복 집합을 파괴하고, 하나의 작업 집합으로 source 고유 수, context 고유 수 및
이후 축별 다양성을 계산한다. 근사 집계나 이전 누적값을 쓰지 않는다. producer와의
최솟값 규칙 및 축별 새 관측 순회는 그대로다.

동일 측정의 최대 추가 예약량은 215,886→166,270바이트로 49,616바이트(약 23.0%)
감소했다. 상주 예약량과 전체 digest는 같다. 다양성/중복/만료/원본 digest 벡터를
포함하여 연결 15,967개 및 Main 4,699개 검사 통과. 할당 실패 주입 지점은 각각
246개와 910개다. 실패 지점 감소에 따라 반복 검사 총수도 달라진다. 검사 사례를
삭제하지 않았다. 원자료 diversity-workspace-memory.txt.

## 축별 원경험 재읽기 제거

각 applied 관측을 처음 집계할 때 producer가 등장한 축을 8비트 마스크로 기록하고,
각 축의 고유 producer 수를 정확히 계산한다. admission이 허용한 축만 사용하며
SWEGCA max_axes가 8을 넘으면 컴파일 시 재검토를 요구한다. global producer 수는
맵의 고유 키 수와 같고, source의 축별 고유 수는 applied 그룹 키에서 계산한다.
기존 source/producer 최솟값 규칙은 유지한다.

이로써 각 축마다 모든 applied 경험을 다시 읽어 집합을 만드는 순회를 제거한다.
관측 순서, 중복·만료 admission, 그룹 정규화, 최근 창, 3상 판정은 변경하지 않는다.
producer 축 마스크의 저장 비용이 추가되므로 메모리 감소로 단정하지 않는다.

두 cold 구간/1축/같은 seed의 전체 평가에서 pread 호출은 90→46회로 감소했다.
1,536표본 메모리 측정은 166,270→170,366바이트로 4,096바이트 증가했고 digest는
그대로다. 디스크 재읽기 감소와 producer 축 정보의 공간 비용을 함께 보고한다.
파일시스템/장치별 경과시간 개선율로 환산하지 않는다. 자료는
axis-cold-reads-{before,after}.txt 및 axis-producer-memory.txt다.
