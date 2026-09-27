# 봉인 경험 메타데이터 페이지 — 저장 단위 구현

지역 적재/퇴거에 사용할 제한 크기 저장 단위를 구현했다. 현재 MainGraph 및
ExperienceSequence의 실제 퇴거/재적재 경로에는 아직 연결하지 않았다. 이 문서와
페이지 단위 검사 통과를 Main 페이징이나 4GB 운용 완료로 해석하지 않는다.

## 검증 경계

ExperiencePage::create는 이미 원문 검증을 거친 ExperienceEvidence만 입력받는다.
경로만 지정해 임의 페이지를 다시 여는 공개 API는 없다. 생성한 핸들은 실제
ExperienceBlock 쓰기가 반환한 정확한 레코드 주소와 체크섬을 보유한다.
디스크 읽기는 이 주소로 전체 페이지를 인증하고 기존 SWEGCA admit_observation을
통과한 값만 반환한다. 외부에서 임의 관측값/강도/판정을 설치하는 API가 아니다.

페이지는 최대 256개 경험의 메타데이터를 보존한다. 원경험 블록/오프셋/크기/digest,
hypothesis/source/context/producer, 관측·만료 시각, confidence, axis, outcome,
expiry/input-key 플래그, 정확 cue를 고정 길이 little-endian 필드로 저장한다.
관측 address는 원경험 digest에서 복원한다. C++ 구조체의 padding/ABI를 저장하지 않는다.
원문과 연결 강도를 복제하거나 메타데이터 페이지를 새 경험으로 세지 않는다.

기존 ExperienceBlock의 체크섬·동기화·I/O 계수·StorageBudget 제한을 사용한다.
단일 값 읽기는 제한 크기 페이지 전체를 검증한다. load는 한 번의 페이지 읽기로
전체 구간을 복원하여 향후 구간 캐시가 항목마다 반복 I/O를 할 필요가 없도록 한다.
출력은 완전한 검증 후에만 반환되며 임시 버퍼도 VRS MemoryBudget을 사용한다.

## 검사와 다음 연결 작업

make build/experience-page-tests / build/experience-page-tests로 검증한다.
256개 값의 모든 필드와 3종 관측 결과, explicit/derived cue, expiry, axis, confidence,
페이지 이동, 인덱스 범위, 예산 부족, 다른 admission 조건, 선택 밖의 손상을 검사한다.
단일 읽기는 80,000바이트 예산, 전체 load는 200,000바이트 예산에서 수행했다.
페이지 손상 시 부분 결과가 반환되지 않고 추적 메모리가 해제됨을 확인했다.

다음 구현은 ExperienceSequence의 봉인 구간과 이 페이지를 연결하고, 구간 핀의
수명을 보존하면서 Main 소유자가 상주 구간을 퇴거·적재하도록 만드는 것이다.
원문 checksum/admission을 우회하거나 입력→Recall 앞에 디스크 적재를 넣어서는 안 된다.
재시작 시 페이지 주소의 영속 소유권과 정리/회수도 이 연결 작업에 포함된다.
