# JSON 문자열 검증·크기 계산 순회 결합 — 미채택

native 이벤트 RPC의 문자열 인코딩에서 UTF-8 검증과 escape 길이 계산을 결합하는
실험을 했다. 모든 UTF-8 검증을 유지하는 단일 순회였으나 대표 Unicode 입력이
느려져 production 변경을 되돌렸다. 현재 json.cpp는 실험 전 HEAD와 동일하다.

재현: make build/json-quote-bench 후 CPU 6,7에서 실행. 각각 약 1MiB인 ASCII
native JSON과 한글/이모지/escape 포함 JSON을 10회씩 31구간 측정한 구간 평균의
중앙값이다. SIMD 활성 네이티브 빌드, 모델/API 호출 없음.

| 입력 | 원래 | 결합 실험 | 복원 후 |
|---|---:|---:|---:|
| ASCII | 106,012ns | 106,368ns | 110,016ns |
| 한글·이모지 | 1,547,139ns | 1,851,804ns | 1,518,802ns |

출력 크기 합계는 세 실행 모두 동일했다. 원자료 json-quote-{before,fused,restored}.txt.
이 자료는 quote_json 단독 시간이며 전체 입력→Recall 측정이 아니다. 복원 후 JSON
스트림 13,291개 검사 통과. 순회 결합을 성능 개선이라고 주장하지 않는다.

다음 구조 개선 대상은 AgentEventCommit의 native 문자열 포장과 호스트의 외부 JSON
해제 뒤 native JSON 재파싱이다. 원문 공백/escape/unknown 필드까지 바이트 그대로
보존하고 모든 검증을 유지해야 한다. 이번 실험에서 새 전송 프로토콜을 도입하지 않았다.
