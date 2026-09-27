# 실제 파일 관측을 반환하는 C++ MCP 생산자

## 실행과 범위

`build/swegca-content-observer RAM_BYTES IO_BYTES_PER_SECOND MAX_FILE_BYTES`

표준 입력/출력 MCP 도구 `observe_file_content_equality`를 제공한다. 새 VRS Main이나
별도 판단 엔진을 생성하지 않는다. 내부 모델 호출, 명령 실행, 파일 쓰기는 없다.
메모리·I/O·파일별 읽기 한도를 실행 인자로 받고, 프레임 한도는 64KiB다.
현재 사용자의 데스크톱/MCP 설정에는 설치하거나 등록하지 않았다.

도구 인자는 다음 세 개다.

- `inputOriginal`: VRS가 현재 입력에 발급한 block/offset/bytes/digest 참조.
- `left`, `right`: 읽을 두 파일의 절대 경로.

생산자는 참조의 문법만 확인한다. 주소의 실재와 같은 native turn의 입력인지 여부는
기존 VRS ingress가 봉인 원경험을 기준으로 확인한다. 주소는 파일 접근 권한이 아니다.
도구 호출의 접근 권한은 호출 에이전트가 소유하며 이 프로그램은 권한을 발급하지 않는다.

## 실제 측정과 SWEGCA 연결

1. 두 경로를 검증한 후 읽기 전용·비차단 descriptor를 연다. FIFO 등은 일반 파일
   검사에서 미확인 관측이 되어 읽기를 기다리지 않는다.
2. 기존 `observe_file_equality`가 실제 바이트와 전후 파일 정보를 확인한다.
3. `observe_content_equality` 소자의 관측값을 반환한다. `scope`는 실제 술어
   `equal-file-bytes-v1`과 두 경로로 생성한다. 호출자가 scope, axis, outcome,
   confidence를 지정해 다른 검사를 했다고 바꿀 수 없다. 관측 축은 observational 0이다.
4. 결과 `structuredContent.swegcaObservation`은 기존 native 완료 도구 경로가 받는
   입력이다. 범위별 별도 연결→원문 저장→셔플→SWEGCA 3상 판정/강도 갱신으로 간다.
   생산자는 accept/reject/abstain 판정이나 연결 강도를 반환하지 않는다.

측정에는 읽은 길이, SHA-256, 전후 device/inode/mode/크기/mtime/ctime,
완전성·안정성·I/O 오류가 포함된다. 읽기가 불완전하면 digest는 null이며
취득하지 못한 stat도 null이다. 파일 없음, 크기 제한, 읽기 오류는
insufficient 관측으로 반환해 실패 경험도 보존할 수 있게 한다.

MCP text content는 짧은 설명이며 전체 관측은 structuredContent에 한 번 보존한다.
동일 JSON을 text로 다시 직렬화하면 native 봉투의 중복 이스케이프 때문에 기존
4KiB 테스트 프레임을 넘었다. 전체 관측을 줄이거나 프레임 한도를 올리는 대신
text의 중복 사본을 제거했다. 실제 native 원문에는 모든 structured 측정값이 남는다.

## 검증된 경로

CPU 6,7 / make -j2로 소스에서 빌드했다.

- MCP 생산자 **105 checks passed**: 빈/바이너리/일치/불일치 파일의 실제 읽기,
  파일 없음·FIFO·한도 초과, 결과 해시와 파일 비변경, 잘못된 참조/인자/프레임,
  오류 뒤 프로토콜 회복과 정상 종료.
- 실제 stdio **5080 checks passed**: C++ 생산자를 실행한 실제 결과를 native 도구
  이벤트로 주입했다. 일치→변경→삭제의 실제 support/refute/insufficient가 같은
  scope 연결에 기록되고, 부모 입력에는 합산되지 않으며, 재시작 후 전체 원문과
  입력 context가 복원되는지 확인했다. 단일 생산자/문맥의 반복으로 독립 증거를
  만들어 승인받는 일이 없도록 기권도 검사했다.

네이티브 이벤트 전달은 테스트 클라이언트가 수행한다. 실제 설치된 데스크톱에서
모델이 이 도구를 선택·호출했다는 검증은 아니다.

## 남은 경계

- 자연어 요구사항 추출, 이 비교가 해당 요구사항에 적합한지 검증,
  모든 요구사항과 전체 목적의 관계 판단은 미완료다. 파일 내용 일치를 파일 이동,
  권한 보존 또는 작업 전체의 성공으로 확대하지 않는다.
- 서로 다른 파일을 동시에 원자적으로 읽는 스냅샷 보증은 없다. 전후 메타데이터의
  관측된 변경은 탐지하지만 필요한 호출자는 불변 스냅샷을 제공해야 한다.
- RAM 인자는 이 프로세스의 PMR 예산이며 전체 RSS 보증이 아니다. I/O 예산도
  생산자 프로세스 내부에서 공유한다. 실제 설치 시 Main과 모든 생산자를 합친
  RAM/I/O 제한을 검증해야 한다. 이번 테스트는 전체 4GB/5Gbps 운용 게이트가 아니다.
- 아직 실제 데스크톱에 등록하지 않았다. 기존 앱과 서비스를 변경하지 않았다.
