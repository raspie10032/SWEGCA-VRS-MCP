# VRS와 관측기의 공유 전송 예산

## 구성

데스크톱 host는 자식을 시작하기 전에 `SharedTransferState`를 만든다. Linux memfd의
RAM 전용 객체이며 파일 크기를 seal한다. host의 생존 기간 동안 fd를 소유한다.
환경의 `SWEGCA_IO_OWNER`는 host pid/fd 위치를 전달한다. 자식이 상속 fd를 닫아도
/proc의 owner fd를 열어 동일 객체를 연결한다. 형식·크기·seal·요청 전송률을 확인하며
일치하지 않으면 시작을 거부한다. 디스크의 별도 DB나 지속 로그를 만들지 않는다.

`TransferBudget`의 기존 예약 산식은 그대로다. 공유 객체가 있으면 동일 next/requested
상태를 process-shared robust mutex 아래 갱신하고, 없으면 기존 프로세스 내부 예산을
사용한다. 한 번의 wait가 비용을 예약한 뒤 실제 I/O를 수행하는 순서는 바뀌지 않는다.
초기/유휴 credit은 여전히 최대 1MiB이며 대기자가 늦게 깨어나도 과거 예약을 몰아서
사용하지 않는다. 따라서 엄격한 순간 물리 속도보다 `rate*time + 최대 한 청크 credit`의
논리적 요청량 제한으로 해석해야 한다.

설치 관측기는 `--require-shared-io`를 사용하고 MCP의 `env_vars`에 owner 키를 명시한다.
owner 정보가 누락되거나 잘못됐으면 독립 예산으로 우회하지 않는다. 등록된 전송률이
Main과 다르면 거부한다. 새 host 시작 시만 새 예산을 만들며 자식 추가/재시작으로
초기 credit을 새로 만들지 않는다. 여러 독립 Main/host 사이의 전역 예산은 아니다.

프로세스가 mutex를 보유한 채 죽으면 미완료 예약을 추측해서 복구하지 않는다.
robust mutex를 unrecoverable 상태로 두어 나머지 요청도 실패한다. 기존 host의 자식
실패 처리가 종료·재시작을 맡으며 VRS 경험/강도/세션 종료 판정은 변경하지 않는다.
공유 예약은 자원 스케줄링이며 SWEGCA 증거 판정과 별개다. 코어 판정식·셔플·3상
의미와 입력 직후 Déjà vu/Recall 앞의 순서는 변경하지 않았다.

## 검증

- Transfer budget 38 checks: 프로세스 4개의 초기 credit 경쟁에서 총 허용량이 한
  청크임을 확인. rate 불일치, malformed owner, 보유자 사망 후 우회 없는 실패 확인.
- 실제 파일 관측 107 checks: 공유 모드의 누락/잘못된 owner도 시작 거부.
- stdio 5416 checks: 기존 desktop fixture와 기록/비교/복구 경로 통과.
- 설치한 실제 backend의 MCP 목록에서 관측기 등록 확인. /proc에서 VRS와 observer의
  owner 값이 동일하고 실제 host pid의 memfd를 가리키는 것을 실행 중 확인했다.
- host/proxy/VRS/observer 동일 cgroup, RAM 3999997952 bytes, swap 0, CPU 6-7도 재확인.
  실제 모델 호출 및 새 대화 생성은 0회다.

## 남은 제한

이 예산은 코드가 요청하는 read/write 바이트의 합산이다. 파일시스템 메타데이터,
writeback/읽기 증폭, OS 캐시와 다른 프로그램의 장치 I/O를 포함한 실제 SSD 전체를
하드웨어 수준에서 5Gbps로 제한한 증거는 아니다. GUI 전 이벤트 수집·일반 자연어
요구사항 검증·대용량 입력 지연의 완료로 취급하지 않는다.
