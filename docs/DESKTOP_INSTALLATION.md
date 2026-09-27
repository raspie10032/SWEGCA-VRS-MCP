# 선택형 데스크톱 연결 설치

현재 로컬 설치는 기존 ChatGPT/Codex 실행 항목을 변경하지 않고 별도의
`Codex · SWEGCA VRS` 실행 항목을 추가한다. 현재 앱을 종료하거나 재시작하지 않았다.
이미 실행 중인 앱은 새 환경을 상속하지 않으므로 현재 대화의 수집을 의미하지 않는다.
기존 앱을 완전히 종료한 다음 새 실행 항목으로 시작해야 실제 GUI 연결이 활성화된다.

## 설치 내용

- 실행 항목: `/home/raspie/.local/share/applications/swegca-codex.desktop`
- 설치 디렉터리: `/home/raspie/.local/share/swegca-vrs/desktop`
- 독립 원경험 저장소: 설치 디렉터리의 `experience`
- `CODEX_CLI_PATH`: 설치한 `bin/swegca-codex-wrapper`
- `SWEGCA_DESKTOP_CONFIG`: 설치한 `desktop.json`
- 실제 백엔드: `/usr/lib/chatgpt/resources/codex`

네이티브 C++ 실행 파일 네 개를 복사하고 SHA-256 manifest를 저장한다. 설치 디렉터리
권한은 0700, 설정은 0600이다. 설치기는 기존 디렉터리나 실행 항목을 덮어쓰지 않는다.
배포 파일 작성만 Python이며 경험/검증/전송 로직은 기존 C++ 바이너리다.
로컬 앱 패키지의 CLI override 처리와 실제 버전 전달로 백엔드 연결을 확인했다.

자원 설정은 `limited-ensure`, 총 프로세스 그룹 4,000,000,000 bytes RAM, swap 0,
CPU 6·7, merge worker 2, 저장소 500,000,000,000 bytes, VRS 전송 예산
625,000,000 bytes/s다. 물리 SSD 전체 I/O의 하드웨어 속도 제한까지 입증한 것은 아니다.
관측 정책은 기존 `examples/stdio-config.json`을 유지한다. 프록시의 네이티브 프레임
한도는 16MiB, VRS RPC 프레임은 코드의 최악 JSON quoting 식 `6*N+65536`이다.
대형 프레임 지원을 입력→Recall 1ms 달성으로 해석하지 않는다.

## 실제 검증과 경계

- 설치한 wrapper의 `--version` → 실제 codex-cli 0.155.0-alpha.9.2.
- 설치한 설정 그대로 wrapper→host→proxy→VRS→실제 백엔드에 initialize/initialized
  두 프레임을 보내 정상 응답 및 종료 0을 확인했다. 모델/도구 호출은 하지 않았다.
- 최초 검사에서 host/proxy 프레임 예산 불일치로 시작이 거부됐다. 프록시 원문 한도를
  바꾸지 않고 호스트 예산을 실제 변환식에 맞춰 수정한 뒤 재검증했다.
- 설치 바이너리 hash, 프레임 예산 관계, 공백 경로 desktop 문법, 기존 설치 덮어쓰기
  거부를 확인했다. 기존 GUI·서비스·MIME 연결은 변경하지 않았다.
- 실제 GUI에서 새 사용자 입력을 보내 수집되는지, 모든 이벤트가 보존되는지,
  파일 관측 MCP가 실제 에이전트에 등록되어 사용되는지는 아직 검증하지 않았다.
- 이 설치가 일반 자연어 요구사항 해석 기능의 완성을 의미하지 않는다.

재설치가 필요한 경우 기존 원경험과 Main identity를 보존하는 별도 갱신 작업이 필요하다.
현재 설치기는 처음 설치만 지원하며 기존 데이터를 초기화하거나 호환 이관하지 않는다.

## 파일 관측 MCP의 백엔드 등록

설치기에 `swegca-content-observer`를 추가했다. wrapper의 선택 필드 `backendConfig`는
TOML `key=value` 문자열 배열이며 app-server 시작에만 각각 `-c`의 단일 argv로 전달한다.
셸을 실행하거나 문자열을 옵션으로 다시 분할하지 않는다. 원래 인수는 이어서 그대로
전달한다. help/version 등 비서버 호출에는 이 등록을 추가하지 않는다.

설치된 desktop.json에는 `mcp_servers.swegca_content_observer.command`와 `args`가
추가됐다. 관측기의 PMR 한도 128MiB, 전송 예산 625000000B/s, 한 파일 상한 16MiB다.
백엔드의 자식으로 실행되어 VRS와 동일한 프로세스 그룹 제한을 받는다. 전역 사용자
config.toml을 수정하지 않았다. 기존 Main identity, proxy instance, 원경험을 유지했다.

검증 결과:
- wrapper literal argv/잘못된 설정/비서버 전달 41 checks, 실제 파일 관측 105 checks.
- `tests/installed_observer_smoke.py INSTALLED_PREFIX`로 실제 설치 백엔드의
  `mcpServerStatus/list`에 `observe_file_content_equality`가 등록된 것을 확인했다.
- host/proxy/VRS/observer의 /proc 및 cgroup을 실행 중 읽어 동일 그룹을 확인했다.
  memory.max=3999997952(4GB를 페이지 단위 내림), memory.swap.max=0, CPU=6-7.
- 이 opt-in 검사는 해당 프로세스에서만 MCP 목록을 이 로컬 관측기로 한정했다.
  다른 사용자 커넥터는 시작하지 않았으며 영속 설정은 변경하지 않았다.
- 모델 호출·새 대화 생성 없이 정상 종료했다. 실제 GUI에서 모델이 도구를 선택하거나
  도구 결과가 사용자 입력의 의미에 적합한지는 이 검사가 입증하지 않는다.
- 관측기와 VRS의 개별 전송 예산 합계가 물리 SSD 5Gbps를 넘지 않도록 하는
  시스템 전체 I/O 강제는 여전히 미완료다. RAM/CPU 그룹 확인과 구분한다.

## 합산 논리 전송 예산 후속

설치 host/VRS/observer를 공유 예산 버전으로 갱신했다. MCP 설정은
`env_vars=["SWEGCA_IO_OWNER"]`와 관측기의 `--require-shared-io`를 포함한다.
실제 backend에서 VRS와 관측기가 같은 host의 memfd를 연결함을 확인했다.
이는 앞 절의 개별 예산 합산 문제를 논리 요청량 범위에서 해결한 것이다.
물리 SSD 전체 제한과 구분하며 SHARED_IO_BUDGET.md의 한계를 따른다.


## 일반 회상 복구·원문 인용 버전 갱신

설치 실행 파일의 기존 manifest 해시와 실행 프로세스 부재를 확인한 뒤,
검증된 `swegca-vrs-mcp`와 `swegca-content-observer`를 갱신했다.
설정/identity/원경험은 유지했고 GUI를 재시작하지 않았다. 파일별 교체는 임시
파일 fsync→rename으로 수행했으며 manifest와 디렉터리도 동기화했다.

실제 설치 backend의 MCP inventory에서 `requirement`의 textIndex/byteOffset/quote
스키마와 원래 필수 인자를 확인했다. host/proxy/VRS/observer의 동일 cgroup,
memory.max=3999997952, swap.max=0, CPU 6-7, VRS/observer의 같은 memfd 전송
예산을 재확인했다. 모델 호출 0, 정상 종료 0, 검사 프로세스 잔류 없음.
설치 해시와 소스 빌드 바이너리 일치도 확인했다. 현재 실행 GUI의 실제 사용자
대화 수집이나 도구의 의미 적합성을 검증한 것은 아니다.


## 소스 후속: 조회 참조 전달과 남은 에이전트 연결

현재 소스의 프록시는 첫 입력 참조와 Replay 문맥 모두에 해당 입력 acknowledgment의
`receipt`를 추가한다. 회상 원문에 들어 있는 핸들을 현재 입력 핸들로 사용하지 않는다.
자동 Replay 요청은 `receipt`와 `inputOriginal`을 함께 보내며 호스트가 실제 입력 주소를
대조한다. 복구된 입력에도 같은 검사를 수행한다. 영수증은 실행 중 해당 입력의 조회
참조이며 프로세스 재시작 뒤 재사용할 영속 주소나 실행 권한이 아니다.

wire148 / 실제 stdio5850 checks 통과. 실제 사용자 GUI/모델 도구 호출 검증은 아니다.
설치된 desktop.json을 확인한 결과 SWEGCA MCP 등록은 내용 관측기뿐이다. 별도 에이전트
조회 도구가 호스트 소유 VRS로 요청을 전달하는 연결은 아직 구현되지 않았다. 이 문맥
필드 추가만으로 에이전트의 요구별 추가 조회를 완료했다고 보지 않는다. 설치본도 이번
소스 변경을 아직 포함하지 않는다. 연결 시 같은 Main을 소유하는 기존 호스트에 입력
주소와 세션을 검증하여 위임해야 하며 별도 Main이나 외부 판정기를 생성하지 않는다.


## 후속 완료: 소유 VRS 조회 MCP 연결

이전 절의 미구현 조회 연결을 구현하고 설치했다. 관측 MCP는 호스트가 전달한
SWEGCA_QUERY_SOCKET이 있을 때 vrs_replay를 제공한다. 설치 설정의 env_vars에
해당 변수 전달을 추가했다. 조회 요청은 기존 소유 VRS로 가며 inputOriginal/receipt
확인 뒤 기존 Replay만 수행한다. 자세한 제한과 검사 결과는 AGENT_QUERY_BRIDGE.md.

실제 backend 등록, 동일 private 소켓, 4프로세스 동일 자원 그룹/공유 I/O와 종료 정리를
검사했다. 모델/새 대화 호출은 없었으며 현재 실행 GUI의 사용자 turn 검증은 아니다.
