# 회상한 입력 후보와 전달 관측의 위치 연결

2026-09-27, NATURAL_INPUT_PLAN 1~2단계의 연결·전달 보강.

## 소유권과 의미

프록시가 소유 VRS에서 이미 받은 원경험과 관련 관측을 전달할 때
`recalledRequirementCoverage`를 추가한다. VRS 조회, 원경험 선택, 평가나
경험 쓰기를 새로 수행하지 않는다. 기존 코어의 원문 일치 검증과
`input_spans_overlap` 위치 소자를 조합한다. SWEGCA 숫자 판정식과
승인/거절/기권·연결 강도는 변경하지 않는다.

이 표의 `inputOriginal`은 **회상한 입력 원경험**이다. 기존 `inputCandidates`
안의 현재 입력 원경험과 별개이며, 두 입력의 글자가 같아도 주소를 혼용하지
않는다. 회상 원문이 native turn/start 또는 turn/steer JSON인 경우에만 구성한다.
현재 사용자 입력 배열과 회상·관측 원문은 모두 그대로 전달한다.

각 후보는 원문 requirement(textIndex, byteOffset, quote)와 실제 전달된
`relatedExperiences` 배열의 `relatedExperienceIndices`를 가진다. 이전 단일
relatedExperience 형식에서는 index 0이 그 객체를 가리킨다. 원래 배열의
첨부물 위치와 UTF-8 바이트 위치를 유지한다.

연결은 다음을 모두 확인한 경우에만 만든다.

1. 전달된 관측 원문이 완료된 MCP tool item이다.
2. 선언한 inputOriginal이 회상한 입력 주소와 같다.
3. 선언한 requirement가 scope에 들어 있는 requirement와 같다.
4. 인용이 그 원문 위치에서 코어 원문 일치 검증을 통과한다.
5. 코어 위치 겹침 소자가 해당 후보와의 겹침을 확인한다.

일부 인용과 겹친다는 사실은 후보 전체의 의미·목적을 검증한 것이 아니다.
항상 `semanticVerified=false`, `boundariesVerified=false`,
`requirementsComplete=false`, `grantsAuthority=false`다. 지지·반박·불충분은
기존 관측과 각 SWEGCA assessment에 그대로 남으며 이 표가 재판정하지 않는다.

연결된 관측이 없는 후보도 빈 배열로 남긴다. 빈 배열은 **이번에 전달된
원경험 중 인증된 인용 연결이 없음**을 뜻하며 저장소 전체에 관측이 없다는
뜻이 아니다. 위치를 인증할 수 없는 관측은 unboundObservationIndices에
표시하고 원문을 보존한다. 문자열 검색으로 다른 요구에 임의 연결하지 않는다.

## 제한

자동 표는 최대 후보 8개, 전달 관측 64개를 검사한다. 인코딩 예산은 프록시에서
min(65536, frameBytes/8)이며 절반을 기존 후보 구성에 사용한다. 후보의 next는
회상 원문의 다음 구조 위치이고, byteLimited 및 observationsLimited를 명시한다.
최종 표가 예산을 넘으면 표를 잘라서 완전한 것처럼 전달하지 않고 빈 후보와
byteLimited=true를 전달한다. 제한을 알리는 고정 메타데이터는 극소 예산보다
클 수 있으며 전체 프레임·PMR 제한은 계속 적용된다.

회상 원문 자체는 기존 Replay packet에 이미 전달돼 있다. 이 next를 현재 입력용
inputCandidates 조회에 넣어서는 안 된다. 회상 후보 후속 페이지의 전용 조회는
아직 연결하지 않았다. 큰 회상 원문·후속 페이지·범용 의미 관련성/충분성 검증은
이 표만으로 완성되지 않는다.

## 검증

* Wire 249 checks: 한글 바이트 위치, 첨부물 인덱스, 관측 순서가 요구 순서와
  다른 경우, 다른 입력 주소, scope 불일치, 잘못된 인용, 미전달/극소 예산,
  경계 인접·항목 차이·빈 범위·정수 끝 위치를 확인했다.
* 실제 wrapper/host/proxy/VRS와 테스트 backend 사이에서 네 요구 중 세 개의
  support/refute/insufficient 관측이 각 원문에 연결되고 네 번째가 빈 배열로
  유지됨을 확인했다. 원경험은 명시적 세션 종료 후 Main에 병합해 다시 읽었다.
* 관측 전달 예산 0에서도 네 후보가 보존되고 연결 배열만 비는 것을 확인했다.
  인용 없는 기존 관측들은 임의 매칭 없이 unbound로 남는다.
* 현재 입력 주소와 회상 입력 주소 분리, 원래 사용자 input 배열 보존,
  기존 relatedCoverage의 next/제한/원주소 보존을 확인했다.
* 전체 stdio subprocess 회귀검증 7,582 checks 통과. 실제 모델 호출이나 GUI
  목적 유지 평가를 수행한 것은 아니다.

## 함께 발견한 Main 유지관리 예약 수정

전송 회귀검증에서 빈 Main의 페이지 검사가 끝나기 직전에 새 Main 병합이
완료되면 과거 pass-complete 상태 때문에 idle 호스트가 다음 입력까지 대기하는
문제를 재현했다. Runtime이 검사 중인 Main generation을 보관하고 새 세대가
발행되면 검사 커서·완료 상태를 초기화한다. 이미 쓰는 페이지 작업은 기존대로
join/게시한 뒤 처리하고, 입력 경로에 작업을 추가하지 않는다.

활성 세션의 메모리 압박 + 빈 Main 검사 두 단계 + 명시적 종료/병합 순서를
직접 구성한 회귀검증을 추가했다. 런타임 2,493 checks 및 위 실제 idle stdio
검증을 통과했다. 세션 도중 Main 병합을 허용하는 변경이 아니다.

## 설치본 반영

기존 desktop prefix의 모든 실행 파일 manifest 해시와 설치 프로세스 부재를
확인한 뒤 VRS/proxy를 교체했다. 설정 파일은 바이트 동일성을 보존했다.
실제 설치 backend inventory에서 도구·소유 조회 소켓·공유 I/O·동일 실행 그룹,
memory.max=3999997952, swap=0, CPU6–7, modelCalls=0, exitCode=0을 확인했다.
검증 프로세스와 소켓은 종료/정리됐다. 현재 사용자 GUI를 재시작하거나 기존
대화에 VRS를 활성화한 것은 아니다.

SHA-256:

* VRS: `304cd9e2cf8fa18a0a93e537a8aab4c69b717dda764992a058c1a6121ad8d188`
* Proxy: `e554dd2d24709aab1625ace15c8bf9e3f0c5bf57443adb407a564c92ccded40d`
