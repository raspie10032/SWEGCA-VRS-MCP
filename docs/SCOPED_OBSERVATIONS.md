# 입력의 부분 요구사항과 관측 연결

## 네이티브 경로

완료된 MCP 도구의 `structuredContent.swegcaObservation`에 선택 필드
`scope`를 추가했다. 값은 생산자가 관측 대상으로 선언한 비어 있지 않은 문자열이다.
기존 `inputOriginal`은 같은 turn에 확인된 입력 원경험이어야 한다.

예를 들어 `scope: "byte content unchanged"`와
`scope: "permissions unchanged"`는 같은 복합 요청에 붙어도 다른 연결이다.
첫 번째의 지지 관측과 두 번째의 반박 관측을 입력 전체의 경험값으로 합산하지 않는다.
생산자가 scope를 생략한 기존 보고는 여전히 입력 전체에 대한 보고다. 보고 내용의
사실성이나 의미 적합성을 이 인터페이스가 인증하지는 않는다.

## SWEGCA 구성과 보존

- 코어의 `input_observation_scope(parent, scope)`가 부모 가설과 정확한 scope
  바이트를 길이 구분하여 해시한다. 이름 정규화나 자연어 유사성 추정을 하지 않는다.
  동일 부모/동일 이름은 동일 연결, 다른 부모/다른 이름은 다른 연결이다.
- Runtime이 봉인된 입력에서 부모 가설을 읽고 연결 identity를 구성한다.
  생산자는 hypothesis/context를 덮어쓸 수 없다.
- 관측 context는 실제 입력 원경험 digest다. scope 이름과 관측값을 담은 native
  이벤트 전체는 기존대로 한 원경험에 바이트 그대로 보존한다.
- scoped 연결을 기존 SessionRuntime에 만들고 동일 기록→셔플→SWEGCA
  3상 판정→같은 연결의 강도 갱신 경로로 처리한다. 별도 판단기를 두지 않는다.
- scoped 관측의 cue도 해당 연결을 가리킨다. 입력 전체의 정확 cue에 부분 관측을
  등록하지 않으므로 전체 요청 Recall 후보/판정에 부분 결과가 섞이지 않는다.
  원래 입력과의 context 연결은 보존한다.
- `refinement.connection`은 판정 대상 연결을 명시한다. scoped 판정을 부모
  입력의 판정으로 표시하지 않도록 실제 ConnectionRefinement의 identity를 반환한다.
- 새 연결 해시와 관측 처리는 도구 결과 수신 후에 수행한다. 사용자 입력의
  Déjà vu→Recall 앞에는 작업을 추가하지 않았다.

잘못된 scope 타입이나 빈 문자열은 유효한 관측으로 수용하지 않는다. 원문은
기존 미판정 경험 경로로 보존한다. 네이티브 재전송/복구/명시적 종료 규칙은 유지한다.
일반 `swegca/observe` 봉투의 scope는 아직 지원하지 않으므로 오류로 반환한다.
scope를 무시하고 전체 입력 관측으로 처리하지 않는다.
이 변경은 scope 이름을 전제로 한 구조적 연결이며 의미적 관련성/함의의 증거가 아니다.

## 남아 있는 작업

후속 scoped Recall/Replay와 실시간 대조 연결은 아래 절에 있다. 파생 scoped 대조는
이제 범위별 불변 개정으로 저장·조회한다(COGNITION_REVISIONS.md). 과거 기록을
읽는 것과 재시작 후 실시간 대조를 자동 재개하는 것은 구분하며 후자는 아직 남았다.

파일 관측 함수는 이제 C++ MCP 생산자에 연결됐으며 실제 실행 결과의 native 저장/복구를
검증했다(CONTENT_OBSERVER_MCP.md). 설치된 데스크톱 등록과 자연어 요청에서
검증할 요구사항을 생성하고 그 적합성을 확인하는 기능, 설치된 에이전트의 도구 호출,
여러 부분 요구사항과 전체 목적의 관계 검증은 미완료다. 모든 부분 판정이 승인돼도
자동으로 전체 목적을 승인하는 합산 규칙을 만들지 않았다.

이름이 다른 동일 의미 요구사항의 자동 결합도 구현하지 않았다. scope는 현재
생산자가 선언한 범위이며, 사용자 원문에서 SWEGCA가 의미를 추출했다는 뜻이 아니다.

## 검증 대상

CPU 6,7 / make -j2 재빌드 후 실제 stdio 프로세스 **5034 checks passed**.

실제 stdio 프로세스에서 하나의 복합 입력에 서로 다른 두 scope를 넣어 각각
승인/반려에 도달시키고 부모 입력이 기권·초기 강도로 남는지 검사한다. 다른 부모의
같은 이름은 누적값을 재사용하지 않아야 한다. 재시작 후 scoped 연결의 identity와
개정이 이어지고 native sequence가 복원되는지 검사한다. 명시적 종료→Main 병합→
재시작→새 소비 세션의 부모 Recall도 기권이어야 한다. 보고한 support/refute 자체는
합성 fixture이므로 자연어 해석이나 실제 측정의 사실성을 검증한 시험은 아니다.

## 부분 관측의 Recall → Replay → 대조

기존 `vrs_replay` 도구에 선택 인자 `scope`를 연결했다. 현재 입력의 live receipt와
생산자가 기록한 정확한 scope 문자열을 사용한다. 부모 입력 cue와 scope로 키를
구성하고 정확한 친숙성 신호를 확인한 뒤 기존 Recall을 호출한다. 임시에서 해당 cue가
없을 때만 Main으로 간다. 지정 scope가 없으면 다른 대화나 연속 기억으로 대체하지 않는다.

scope 조회는 candidate/offset/count와 함께 사용할 수 없다. 기존 코어 선택기로
원경험 하나를 고른 다음 기존 Replay/대조 및 충돌 시 Re-evidence를 수행한다.
응답은 scope, temporary, parentCognitionUnchanged와 원경험 및 assessment를 포함한다.
실제 Replay가 기존 대화 연속 키를 갱신하는 동작은 유지하되 부모의 자동 cognition과
부모 Replay 및 해당 영속 개정을 덮어쓰지 않는다. `vrs_re_evidence`는 부모 Replay에
대한 기존 의미를 유지하며 scope 인자를 받으면 오류를 반환한다. scoped 대조 갱신은
같은 `vrs_replay(scope=...)`로 조회한다.

활성 입력마다 scope 하나의 선택 원경험과 대조 경계를 보유한다. 같은 scope를 다시
조회할 때 최근 관측을 새로운 과거 기억으로 재선택하지 않는다. 이후 해당 연결에
기록된 native 관측은 기존 선택 원경험과 대조하고 코어가 충돌로 판정할 때만
Re-evidence를 호출한다. 이 갱신은 관측 기록 후 ACK 전에 수행하며 실패한 갱신은
같은 delivery 재전송 또는 다음 scoped 조회에서 재시도한다. 셔플/경험 기록은 중복하지 않는다.

다른 scope 선택, 새 입력 또는 세션 종료 시 그 메모리상의 선택은 교체/해제된다.
새 입력에서는 기존 scoped 캐시 해제를 Déjà vu/Recall 이전에 추가하지 않고
receive 이후에 수행한다. 원경험과 연결은 계속 영속 보존된다. 후속 영속 개정 구현으로
재시작 후 historical receipt에서는 저장된 scoped 대조와 선택 원경험을 정확히 내보낸다.
새 live 입력의 scoped 조회는 별도 새 조회다. 과거 대조 기록을 실시간 비교 권한으로
복원했다고 주장하지 않는다.

CPU 6,7 / make -j2 재빌드 후 stdio **5176 checks passed**. 임시 scoped 조회,
잘못된/없는 scope의 비대체, 늦은 8개 반증에 따른 기존 선택 유지·충돌·Re-evidence,
부모 결과 불변, Main scoped 조회, 실제 파일 상태를 변경해도 원래 측정 원문을 반환,
다른 세션의 오래된 receipt 거부를 확인했다. 이 검증은 큰 Main의 시간/메모리
성능이나 자연어에서 scope를 자동 해석하는 기능을 증명하지 않는다.
