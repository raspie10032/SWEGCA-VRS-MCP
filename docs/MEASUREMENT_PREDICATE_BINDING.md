# 파일 관측값과 판정 대상의 결속

2026-09-27. 자연어 의미 적합성 전체가 아니라 실제 관측 경로의 명확한 결함 수정.

## 재현과 수정

기존 실제 파일 관측기에서 파일 읽기 실패 결과를 얻은 뒤, 기록될 native 결과의
outcome만 support로 바꾸어 제출했다. 기존 VRS는 유효 인용/scope가 있으면 이를
scoped 관측으로 받아들였다. 추가한 회귀검사의 parent-insufficient 기대가 실패했다.

이제 observe_file_content_equality 도구 또는 equal-file-bytes-v1 predicate를 선언한
보고는 measurement와 scope를 결속한다. 경로 일치, 전체 regular-file 읽기 크기,
전후 버전(장치/inode/mode/크기/mtime/ctime), 완료/안정/I/O 상태, 길이와 SHA256을
기존 core observe_content_equality 입력으로 구성하고 보고 outcome과 대조한다.
도구 이름을 바꿔도 선언한 predicate의 확인을 생략하지 않는다.

모순/누락/잘못된 관측은 기존 부족한 근거 경로로 원문 그대로 보존하며, 그 scoped
연결의 지지/반박으로 사용하지 않는다. 올바른 incomplete 관측은 기권 자료로 해당
scope에 계속 남는다. 코어 승인/거절/기권 판정식과 셔플/강도 업데이트는 변경하지 않았다.
새 파일 읽기나 현재 상태 재측정으로 과거 기록을 교체하지 않는다.

## 검증 및 한계

실제 stdio7153 checks 통과. 실제 관측기의 동일/상이/읽기 실패 결과는 계속 수용한다.
실패를 support로 바꾸기, 다른 digest/path/readBytes/전후 시간, measurement 삭제,
도구 이름 변경 사례는 scoped 지지로 수용하지 않는다. 잘못된 native 원문 보존 확인.

이 검사는 보고 안의 자기일관성과 명시적 predicate의 결속이다. 생산자가 모든
측정 필드를 함께 위조했는지 증명하지 못하며, 이 파일 비교가 사용자 자연어 요구의
충족에 충분한지도 증명하지 못한다. 출처와 불확실성을 보존한다. 인용 일치/바이트
일치만으로 전체 목적 성공을 선언하지 않는다. 일반 관계 해석은 여전히 미완료다.
현재 빌드에 수정했으며 설치본 반영은 아직 하지 않았다.

## 2026-09-27 명시적 불일치 조건 지원

기존 observe_file_content_equality에 선택적 boolean expectEqual 추가(default true).
false는 두 파일의 내용이 달라야 한다는 명시적 관측 조건이다. 별도 도구/외부
판정기를 추가하지 않았다. 실제 파일 읽기 결과를 SWEGCA observe_content_relation
소자로 전달하며, 이 소자는 기존 observe_content_equality에 equal==expect_equal을
합성한다. complete/stable이 false면 극성과 무관하게 insufficient다.
기존 증거 승인/거절/기권 및 셔플/연결 강도 판정식은 변경하지 않는다.

scope predicate는 true일 때 equal-file-bytes-v1, false일 때
different-file-bytes-v1이다. 같은 원문 requirement를 참조해도 서로 다른 조건은
별도 scoped 연결이다. VRS ingress는 두 predicate 모두 측정 메타데이터와
대조한다. producer tool 이름을 바꿔도 선언된 predicate의 일관성 검사를 우회할
수 없다. 원문은 보존하고 모순된 보고서를 scoped 지지 증거로 채택하지 않는다.

예: 원본 snapshot과 편집 결과를 left/right에 지정하고 expectEqual:false를
요청하면 완전하고 안정적으로 읽은 내용이 서로 다른지 관측할 수 있다.
이 결과는 편집의 목적 적합성/코딩 품질/전체 작업 완료를 증명하지 않는다.
사용자 자연어로부터 expectEqual을 자동 확정하는 기능은 구현하지 않았다.
immutable snapshot 사용과 파일 관측의 기존 비원자적 비교 한계는 동일하다.

검증: file observation52 / observer154 / stdio7286 checks 통과.
실제 파일 같음/다름/없음 각각에서 두 조건의 support/refute/insufficient,
기권 비반전, 별도 scoped 연결 및 재시작 원문 보존을 확인했다. 잘못된 결과를
보고한 renamed tool은 해당 scoped 연결에 반영되지 않는다. boolean 외
expectEqual은 파일 열기 전에 거부한다.

설치된 VRS/observer 해시 확인 및 원자 교체 완료. 기존 설정 byte 동일성을
유지했다. 설치본 실제 backend inventory에서 expectEqual schema 등록도
확인했다. 같은 cgroup/조회 endpoint/I/O owner, 약4GB/swap0/CPU6-7,
modelCalls0/exit0 확인. 앞선 e822e13 페이지 조회 최적화도 이번 VRS 설치에 포함된다.

## 완전한 측정의 해시 검증 누락 수정

추가 점검에서 equal=(leftBytes==rightBytes && hash(left)==hash(right))의 단락 평가로
크기가 다르면 양쪽 digest 형식/존재 검사가 실행되지 않는 결함을 재현했다.
complete/stable을 주장하면서 해시가 없거나 손상된 보고서가 equality의 refute,
difference의 support로 scoped 연결에 들어갈 수 있었다.

두 digest를 먼저 독립 검증한 뒤 크기/해시 일치를 계산하도록 수정했다.
core 판정이나 진짜 불일치 관측의 결과는 유지한다. 부족한 complete 보고서는
기존 ingress 실패 경로로 원문을 보존하며 해당 scoped 증거로 채택하지 않는다.

수정 전 실제 stdio 재현은 잘못된 보고서가 scoped 연결에 들어가 assertion 실패.
수정 후 같은 재현을 equality/difference × left/right × missing/malformed로
확장한 실제 stdio7339 checks 통과. VRS 설치본 원자 교체/전체 실행 파일 해시/
설정 보존 및 설치 backend smoke 정상 종료 확인. 자연어 의미 관련성 검증의
완료 증거로 사용하지 않는다.
