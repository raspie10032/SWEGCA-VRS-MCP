# 선택 원경험의 제한 메모리 구간 읽기

`read_evidence_slice`와 `SessionStore::read_payload_slice`는 하나의 선택된
원경험에서 지정한 원문 바이트 구간만 반환한다. 전체 레코드의 체크섬·주소·관측
인코딩과 SWEGCA admission을 검증한 뒤에만 결과가 반환된다. 미검증 조각을 외부
콜백에 내보내지 않으며, 검증 뒤 파일을 다시 읽는 경로도 없다.

반환 버퍼는 VRS MemoryBudget으로 할당하고 scratch는 기존 64KiB 고정 스택을
사용한다. 구간이 원문 범위를 벗어나면 거부한다. 원문이 비어 있거나 EOF를 가리키는
길이 0 구간도 전체 검증 후 반환한다. 범위 밖 손상·I/O 실패·예산 부족은 결과를
반환하지 않고 임시 할당을 해제한다. SessionStore는 소유 블록인지 먼저 확인한다.

이 변경은 반환 메모리를 줄인다. 현재 파일 형식의 전체 체크섬을 유지하므로 매 구간
요청마다 선택 레코드 전체를 읽고 해시한다. 디스크 읽기량 감소를 주장하지 않는다.
여러 구간을 순차 요청하면 반복 읽기 비용이 발생한다. 전체 Replay 완료 영수증으로
승격하거나 Re-evidence를 허가하는 API가 아니다.

Runtime::read_payload_slice는 기존 Replay와 같은 선택 검사 함수를 공유한다. 임시
경험은 append-only 계보와 원주소, Main은 현재 head와 원주소를 확인한다. 만료된
세션의 receipt는 과거 객체를 역참조하거나 디스크를 읽기 전에 거부한다. 부분 읽기는
continuation을 갱신하지 않는다.

MCP의 기존 vrs_replay는 offset/count 문자열을 함께 받으면 이 경로로 원문 구간을
반환한다. partial:true, offset, totalBytes, original, contentHex를 반환하며, 앞서
선택한 전체 Replay receipt도 폐기한다. 따라서 부분 읽기 직후 Re-evidence는
거부된다. offset/count 없이 호출하면 기존 전체 Replay가 동작한다. 여러 부분 읽기를
자동 합산하여 전체 Replay 완료로 바꾸지는 않는다. 이런 전체 완료 경로 및 실제 앱
연결은 별도 남은 작업이다.

검사: 2MiB 원문의 4KiB 구간, 64KiB 경계와 서로 다른 media 길이,
EOF/빈 원문/잘못된 범위/예산 초과/중간 I/O 실패/요청 범위 밖 끝부분 손상.
기존 연결 셔플의 고정 digest와 3상 결과도 유지되는지 확인한다.

Runtime 검사에서는 256KiB 추적 예산으로 2MiB 원경험을 임시/Main 양쪽에서 읽고,
4KiB 반환 버퍼만 추가됨을 확인한다. 같은 예산에서 기존 전체 Replay는 예산 부족으로
거부된다. 이 수치는 호출자 원문과 고정 스택을 포함한 전체 RSS가 아니다.
