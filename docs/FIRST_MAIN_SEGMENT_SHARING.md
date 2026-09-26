# 첫 Main 연결의 봉인된 세그먼트 공유

기존에는 Main 안의 이전 경험 prefix만 공유했다. 처음 들어오는 연결은 종료된 source가
이미 보관한 모든 ExperienceEvidence를 다시 할당·복사했다. 같은 MemoryBudget을 쓰는
첫 연결도 기존 inherit_experiences 경로를 사용하도록 바꿨다.

공유 대상은 가득 찬 불변 세그먼트다. 덜 찬 꼬리는 기존 share_prefix 계약대로 복사한다.
각 경험은 Main의 core admission을 다시 통과하며 전체 실제 셔플·새 집계·3상 검증·강도
갱신을 그대로 수행한다. source의 강도나 revision을 Main 판정으로 설치하지 않는다.
초기 Main 강도에서 경험 수만큼의 append revision을 구성한 후 Main refinement를 수행한다.

서로 다른 MemoryBudget에서는 기존 append 경로로 복사한다. 다른 예산에 할당된 세그먼트를
잡아 두어 그 예산의 수명을 연장해야 하는 숨은 의존성을 만들지 않는다. 같은 예산에서는
source runtime cache를 해제해도 공유 소유권으로 봉인된 값이 유지된다. 원문 저장소의
수명 계약은 그대로이며 MainSources가 계속 소유한다.

검사에서는 같은 예산의 직렬/병렬 Main이 source의 꽉 찬 세그먼트와 같은 값 주소를
보관하고 꼬리는 다른 주소임을 확인한다. 별도 예산의 병렬 Main은 다른 주소에 복사한다.
그 모든 경로에서 strength/revision/전체 refinement digest/원경험 순서가 같아야 한다.
병렬 할당 실패와 워커 실패·spawn 실패·준비 결과 폐기 및 source 캐시 해제 후 Replay와
재시작 복구도 확인한다. 임의 크기 그래프의 제한 내 동작 증거로 확대하지 않는다.
