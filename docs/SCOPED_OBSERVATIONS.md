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

## 복구 조회의 실제 원경험 결속 검증

historical scoped Replay는 저장 JSON의 source/selectedOriginal만으로 원문을
내보내지 않는다. Runtime이 현재 세션의 봉인된 입력을 해독해 부모 가설을 얻고,
동일 `input_observation_scope`로 계산한 연결과 저장 scopeConnection을 비교한다.
선택 원경험도 Main 소유 저장소에서 읽어 SWEGCA 증거 디코더로 검증한 뒤,
가설·명시적 입력 키·cue가 모두 그 scoped 연결과 일치해야 내보낸다.
이는 출처 결속 검증이다. scope 이름의 의미 적합성이나 보고 내용의 사실성을
증명하지 않으며, 저장 verdict를 현재 판단 권한으로 복원하지 않는다.
관측 경계를 복원하여 재시작 후 새 반증을 자동 대조하는 기능은 아직 미완료다.

검증: CPU 6,7 / make -j2 빌드 성공. Runtime 1899 checks, stdio 5233 checks 통과.
재시작 후 정상 원문 조회와 잘못된 scope/연결/원경험/출처/빈 이름 거부를 확인했다.

## 과거 임시 연결의 관측 경계

`PersistentConnection::historical_snapshot`은 이미 SWEGCA로 검증되어 복구된
현재 연결에서 부모 기록을 따라 요청한 과거 head를 찾는다. append 기록을
거슬러 갈 때 관측 수를 되돌리고 refine 기록에서는 실제 이전 강도를 읽는다.
연결 identity, ordinal, revision, strength 일치를 확인하며 다른 계보의 주소는
거부한다. 새 증거 판정이나 현재 연결 변경은 하지 않는다. 원경험 본문 전체를
다시 Replay하지 않으며 하나의 read cursor로 연결 기록만 읽는다.

historical scoped Replay가 임시 연결을 사용했던 경우 이 경로로 rememberedHead를
확인하고 저장 observationBoundary와 관측 수를 대조한다. Main의 과거 head 복구와
새 router 소유 비교 receipt 발행은 아직 연결되지 않았다. 따라서 이 단계 역시
재시작 후 자동 live 대조 완성으로 해석하지 않는다.

검증: persistent connection 93 checks 및 stdio 5230 checks 통과.
과거 append/refine/origin/current 경계, 비계보 주소 거부, 재시작 후 경계와 현재 상태 불변을 확인했다.

## 임시 scoped 대조의 재시작 후 재개

이전 절의 임시 복구 제한을 해소했다. 새 개정 기록은 선택 원경험의 `originalIndex`도
보존한다. 재시작 후 과거 입력 receipt로 `vrs_replay(scope)`를 호출하면 임시 연결은
단순 historical 내보내기 대신 다음 경로로 실시간 대조를 재개한다.

1. 현재 세션의 봉인된 입력을 동일 코어 증거 디코더로 읽고 scope 연결을 확인한다.
2. 현재 소유 연결의 실제 부모 계보에서 rememberedHead와 관측 경계를 확인한다.
3. 인덱스가 당시 경계 안에 있고 원경험 주소·가설·입력 키가 일치하는지 확인한다.
4. 선택 원경험 하나만 Replay하고 현재 라우터 소유 receipt를 발행한다.
5. 현재 연결의 마지막 완료된 refine 기록에서 seed/step을 읽어, 당시 경계 이후의
   실제 관측만 대조한다. 코어가 충돌을 판정한 경우에만 Re-evidence를 수행한다.
6. scoped 개정을 저장하며 부모 cognition 기록은 덮어쓰지 않는다. 이후 새 반증은
   기존 실시간 이벤트 경로에서 응답 전에 대조·저장한다.

복구 결과에는 `restored:true, historical:false`가 붙는다. 이 필드는 저장 개정에
포함하지 않아 동일 비교의 저장 주소를 바꾸지 않는다. scoped 캐시가 자체 입력
주소를 소유하므로 다른 live 입력 또는 재시작으로 received 캐시가 없어도 정확한
입력에 결과를 저장한다. 관측·강도 갱신은 복구 과정에서 중복 수행하지 않는다.
Main에서 선택했던 과거 scoped Replay는 아직 historical 내보내기이며 실시간
receipt 복구가 미완료다. 새 자연 입력의 Main 조회는 기존 경로로 계속 처리된다.

검증: Runtime 1910 checks, stdio 5251 checks 통과. Runtime에서는 재시작·선택 원경험
유지·8개 새 반증·충돌 시 Re-evidence·강도/경험 미중복·잘못된 scope/head/index 거부를
확인했다. stdio에서는 복구 요청 전 도착한 반증의 포함, 복구 후 이벤트 응답 이전
대조 개정 저장, 부모 기록 불변, 명시적 종료/병합 후 해당 개정의 보존을 확인했다.
CPU 6,7 / make -j2만 사용했다. GUI 설치 및 Main 과거 receipt의 live 복구 증거는 아니다.

## Main 과거 연결 상태의 재구성

`PersistentMainGraph::historical_snapshot(connection, root)`는 현재 소유 Main의
병합 계보와 해당 연결의 출처 범위를 사용한다. 요청한 과거 루트까지 병합 기록의
generation/parent/source root를 확인하며, 해당 연결이 참여한 각 병합의 실제
seed/step으로 기존 Connection의 원경험 admission·shuffle·refine을 수행한다.
현재 루트이면 이미 검증된 현재 상태를 반환한다. 다른 계보나 연결 생성 이전
루트는 거부한다. 외부에서 제공한 과거 강도나 저장 verdict는 입력받지 않는다.

두 개의 교대 연결 상태가 기존 Main 병합과 동일하게 불변 메타데이터 prefix를
공유한다. 관련 없는 연결을 재구축하거나 원경험 본문들을 Replay하지 않는다.
현재 Main의 강도·그래프·루트는 바꾸지 않는다. 복구 비용은 해당 연결의 경험량과
요청 시점까지의 병합 횟수에 따라 늘어나며, 이 연산의 상수 시간 보장은 없다.
새 사용자 입력의 Déjà vu→Recall 경로에 이 복구 연산을 삽입하지 않는다.

원경험이 0개인 정의만 있는 연결은 공개 카탈로그에 포함되지 않아 Main 병합 대상이
아니다. 빈 범위를 추가하는 변경은 적용하지 않았다. Main scoped 비교 receipt 복구에
이 과거 상태를 연결하는 작업은 다음 단계이며 아직 완료된 것으로 간주하지 않는다.

검증: Persistent Main 1097 checks, stdio 5250 checks 통과. 후속 병합 및 재시작 뒤
과거 관측 수/리비전/강도의 일치, 연결 생성 이전/미등록 연결/잘못된 루트 거부,
현재 그래프 유지 확인. 테스트 링크의 누락된 fsync 전달 래퍼도 복구했다.

## Main scoped Replay의 실시간 복구 연결

Main 과거 상태 재구성을 `ExperienceRouter::restore_main_replay` 및 Runtime의
코어 비교 구성에 연결했다. 새 scoped 개정은 `observationHead`를 보존한다.
Main Recall 시 현재 임시 연결이 있었다면 그 시점의 head, 없었다면 빈 주소다.
이는 Main의 rememberedHead와 별개이며 이후 새 로컬 관측의 시작 경계를 확정한다.

복구는 봉인된 입력→동일 scope 연결→검증된 과거 Main 루트→그 루트 안의 원경험
인덱스/주소/키→현재 임시 연결의 과거 경계를 확인한 뒤 선택 원경험 하나만 읽는다.
발행하는 receipt는 현재 라우터 소유이고, 동일 기존 compare/Re-evidence 코어를
사용한다. Main에 후속 세션이 병합됐더라도 선택 경험과 기억 당시 head는 유지한다.
현재 세션의 관측만 새 증거이며, 후속 Main 병합을 새 로컬 증거로 재계산하지 않는다.

복구 transport는 임시/Main 모두 `restored:true, historical:false`로 반환한다.
단계 파라미터는 저장된 비교 시점보다 뒤로 가지 않으며, 더 최신인 실제 연결
refine의 seed/step을 사용한다. 새 이벤트는 기존 경로에서 응답 전에 비교와 개정을
저장한다. 부모 cognition과 Main/임시 연결 강도는 복구로 중복 변경하지 않는다.
이 구현은 scoped 조회의 복구이며 일반 자연어 요구사항 추출 완성이나 앱 설치,
대용량 지연 달성의 증거가 아니다.

검증: Runtime 1926 checks, stdio 5418 checks 통과. Main의 후속 병합 뒤 과거
원경험 유지, 잘못된 scope/root/index 거부, 0 및 비제로 임시 관측 경계 검증,
새 로컬 반증의 충돌/Re-evidence, 부모 기록 불변, 반복 재시작 후 동일 비교를 확인했다.
같은 입력 하나에 8개 보고를 붙인 경우 코어는 맥락 다양성 부족으로 기권했다.
충돌 검사에는 서로 다른 실제 입력 원경험 8개를 사용했으며 판정 기준은 바꾸지 않았다.

추가 경계 검사: Session Runtime 1356 checks, Persistent Main 1097 checks 통과.
관측 head 보존으로 8-original Main receipt는 408 bytes, 194-original mixed receipt는
3040 bytes로 증가했다. 고정된 연결 그룹 메타데이터 증가이며 원문 복제는 없다.
