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
