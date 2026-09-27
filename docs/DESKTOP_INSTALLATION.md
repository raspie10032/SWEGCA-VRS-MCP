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
