# 압축 유지력 A/B 채점 규격 — goal-consistency-under-compaction (2026-09-21)

무엇을 재는가: 코딩 과제는 **재료**(기계 채점이 되게 만든 것)이고, 잣대는 **시간 축**이다 — 컨텍스트 압축을
지나면서 과제의 목표·제약·위치가 유지되는가. 채점은 두 층으로 나뉘고 **섞지 않는다**:

| 층 | 단위 | 답하는 질문 |
| --- | --- | --- |
| ① 완료 품질 | 실행(run) 하나 | 끝났을 때 얼마나 정확히 다 뽑았나, 제약을 어겼나, 비용은 |
| ② 압축 유지력 | 압축 경계(compact_boundary) 하나 = 관측 하나 | 압축 직후 제자리에서 이어 갔나, 제약을 지켰나, 되물었나, 잃은 항목이 있나 |

**압축이 0회인 실행은 ②를 말할 수 없다.** 그 실행의 ① 수치를 ②의 근거로 쓰지 않는다(우리 1·2라운드가 그랬다:
A2 911k·B2 809k 토큰, 둘 다 압축 0 → 장부의 이점은 「긴 무압축 실행에서 위치를 안 잃는다」 쪽 관측이지 ②가 아니다).

조건 = **모델 × 장부(A: 없음 / B: `progress.txt`) × 압축 문턱(`--autocompact N` 또는 `autoCompactWindow`) × VRS(있음/없음)**.
같은 과제 카드·같은 입력(해시 동일)·같은 truth 로만 비교한다.

## 0. 대전제 관문 — 이것을 못 지키는 실행은 채점하지 않는다 (2026-09-21)

사용자 대전제: **모든 경험은 swegca-vrs 를 통해 경험으로 들어가고, 불러오는 것도 단순 로그가 아니라 경험을 통해
정확한 위치를 불러온다. 실시간이 아니면 연속성은 「유지 못 함」이 아니라 「없음」이다. 대전제를 무시한 테스트는 즉시 파기한다.**

채점기는 실행마다 먼저 이것을 판정하고, 통과한 실행만 ②를 계산한다:

| 검사 | 근거 | 통과 조건 |
| --- | --- | --- |
| `vrs_tail` | `~/.claude/hooks/vrs2_tail.log` — 그 실행의 전사(`path`)에 대한 영수증 | 경계 `b_i` 마다, `ts` 가 경계 시각보다 **앞**이고 `lines[1]` 이 경계 직전 레코드 줄 이상인 영수증(`trigger` = `precompact` 또는 `stop`/`subagent_stop`)이 있다. 마지막 경계 뒤에도 `stop` 영수증이 하나 이상 |
| `vrs_recall` | `~/.claude/hooks/recall_context.log` — 그 세션의 영수증 | 경계 뒤 첫 프롬프트에 회수 훅이 돌았다(`injected` 또는 `skip` 어느 쪽이든 **영수증이 있다**; 없으면 훅이 안 물린 실행) |
| `vrs_rows` | 데몬 `origins kinds=["transcript"]` | 그 세션 id 의 대화 행이 있고, 경계 직전 턴의 행(`part` = `partial` 또는 `whole`)이 있다 |

셋 다 통과 → `vrs = 1`. 하나라도 아니면 그 실행은 **「VRS 없음」** 으로 표기하고 ②(§5)는 계산하지 않는다 — ① 수치는 보고하되
「연속성」이라는 말을 그 실행에 붙이지 않는다. **회수 훅·유입 훅이 물리지 않는 실행 방식(도구를 Read/Write/Edit 로 제한한
서브에이전트, 훅 없는 헤드리스 실행)은 처음부터 「VRS 없음」 조건이다** — 그런 실행의 ② 는 층이 아니라 모델의 습관을 잰
것이고, 그것을 층의 결과로 적으면 대전제 위반이다. 우리 1·2라운드(A2·B2)가 그랬다(§10).

이 절은 §5 보다 먼저 구현한다. 관문 없이 ② 를 계산하는 채점기는 파기한다.

---

## 1. 입력물

| 것 | 어디 | 비고 |
| --- | --- | --- |
| 입력 로그 둘 | `input/SQLITE-session-log.md`(2,005줄), `input/ME-session-log.md`(778줄) | 읽기 전용. 실행 전에 sha256 을 적어 두고, 모든 실행이 같은 해시인지 확인 |
| 과제 카드 | `task_<cond>.md` | 조건마다 출력 폴더만 다르다. A 와 B 의 차이는 **B 의 「진행 장부」 단락뿐** |
| 실행 출력 | `run_<cond>/chunk_<SQLITE\|ME>_<시작줄>.csv`, `report.txt`, (B) `progress.txt` | 폴더 하나 = 실행 하나 |
| 전사 | 세션 `<id>.jsonl` 또는 `<id>/subagents/agent-*.jsonl` | 실행과 전사의 짝은 **출력 폴더 이름**으로 맞춘다(전사 앞 4 KB 에 과제 카드 본문이 실리므로 그 안의 폴더 이름) |
| truth | `truth.json` | §2 로 **실행 전에** 봉인. 실행 뒤에 손대지 않는다 |
| 조건 표 | `conditions.json` | 실행 id → {model, ledger, autocompact, effort, tools} |

경로는 **하드코딩하지 않는다**(윈도 `C:\...` 카드가 리눅스로 옮겨 왔다 — 카드의 경로는 그 기계의 것으로 바꿔 쓰되,
채점기는 `--inputs DIR --runs DIR --truth FILE --transcripts DIR` 인자로만 받는다).

## 2. truth 봉인 (한 번, 실행 전에)

```
항목  := 줄이 정규식 ^- (\d{4}-\d{2}-\d{2}) 에 맞는 줄. 줄 번호는 1부터(Read 가 보여 주는 번호와 같다).
       들여쓴 줄, 제목, 날짜로 시작하지 않는 불릿은 항목이 아니다.
date  := 그 날짜.
text30:= 날짜 뒤 텍스트에서 앞뒤 공백·콜론(':')을 떼고 처음 30자; 쉼표·큰따옴표·줄바꿈 → 공백.
제외  := date ∈ {2026-08-03, 2026-09-10} 인 항목은 truth 에 넣지 않고 "excluded" 수로만 센다.
```

파일을 **바이너리로 열어 UTF-8 로 디코드**(`errors="replace"`), BOM 제거, `\r\n` 은 한 줄. `truth.json` 에는
`exclude`, `per_file{items, excluded, lines}`, `items[{file, line, date, text30}]`, **입력 해시**를 넣는다.

같은 입력이라면 숫자는 이것이어야 한다 — 다르면 봉인이 틀린 것이다:
**SQLITE 521(제외 53, 2005줄) · ME 217(제외 25, 778줄) · 합 738, 제외 78.**

## 3. 출력 행 파싱 (chunk_*.csv)

- 파일마다 줄 단위, 빈 줄 무시, BOM 제거, UTF-8 `errors="replace"`.
- 쉼표로 나눠 **4 필드 이상**: `file, line, date, text30…`(text30 안의 쉼표는 공백으로 바꿨어야 하지만, 남아 있으면
  4번째 이후를 합친다). `file ∈ {SQLITE, ME}`, `line` 정수, `date` 가 `\d{4}-\d\d-\d\d`. 하나라도 어긋나면 **형식 오류**
  — 조용히 버리지 말고 세고 예 3개를 남긴다.
- 헤더 줄(`file,line,date,text30`)이 있으면 형식 오류 1 로 센다(카드가 「헤더 없음」이라 했다).
- 행의 열쇠 `key = (file, line)`.

## 4. ① 완료 품질 — 실행 하나당

| 지표 | 정의 | 점수에 들어가나 |
| --- | --- | --- |
| `rows` | 파싱된 행 수 | 참고 |
| `duplicates` | 같은 key 가 두 번 이상 나온 횟수(초과분) | **감점** |
| `hit` | `seen ∩ truth` (seen = key 집합) | — |
| **`recall`** | `|hit| / |truth|` | **주 지표** |
| `missing`, `gaps` | `truth − hit`; 파일별로 **연속 구간**으로 묶어 `SQLITE:1143-1145` 꼴로 | gaps 는 어디를 잃었는지 말해 준다 |
| `exclusion_violations` | `date ∈ exclude` 인 행 수(key 무관) | **감점** — 제약 위반 |
| `spurious` | `seen − truth − (제외 위반 key)` | **감점** — 없는 항목을 만들었거나 줄이 어긋남 |
| `off_by_one` | spurious 가운데 `(file, line±1) ∈ missing` 인 쌍 | spurious 의 부분집합. **따로 보고**(내용 손실이 아니라 위치 오류; 우리 B2 의 ME:644→643 이 이것) |
| `date_match` | hit 가운데 date 가 truth 와 같은 수 | 참고 |
| `text30_match` | hit 가운데 **정규화 뒤 앞 12자**가 같은 수 | **참고만. 점수에 절대 넣지 않는다**(§9) |
| `chunk_files` | 기대 집합 `{chunk_<F>_<s>.csv : s = 1, 201, …, ≤ lines}` = SQLITE 11 + ME 4 = **15** 대 실제; `chunk_missing`, `chunk_unexpected` | 누락은 **감점**(그 구간을 안 읽었다는 뜻) |
| `report_delta` | `report.txt` 가 스스로 적은 합계 − 채점기 `rows` | 참고(자기 보고의 정직성; B2 는 747 대 738) |
| `progress_final` | (B) `progress.txt` 마지막 내용이 「완료」이거나 다음 시작줄이 파일 끝을 넘는가 | 참고 |

text30 정규화(비교에만 씀): NFC → `**`·백틱 제거 → 앞의 공백·콜론 제거 → 연속 공백 하나로 → 앞 12자.

### 4-1. 제약 위반 (전사에서)

카드의 「하지 않는 것」을 전사의 `tool_use` 로 센다. 전사 파싱은 §6.

| 위반 | 판정 |
| --- | --- |
| 훑기 도구 사용 | `Grep`, `Glob`, `Bash`, `PowerShell` tool_use 가 **하나라도** 있으면 위반. 횟수와 첫 예를 남긴다 |
| 출력 폴더 밖 쓰기 | `Write`/`Edit` 의 `input.file_path` 가 출력 폴더 밖이면 위반 (경로 정규화 뒤 접두 비교). 셸로 썼을 수도 있으니 실행 전후 샌드박스 스냅샷(파일 목록+mtime) 차이도 같이 본다 |
| 되묻기 | `AskUserQuestion` tool_use, 또는 tool_use 없는 어시스턴트 턴의 본문이 사용자에게 묻는 문장(`?` 로 끝나는 줄)으로 끝남 — 완료 보고 턴은 제외 |
| 입력 수정 | 입력 두 파일의 해시가 실행 뒤에도 같아야 한다 |

### 4-2. Read 규율 (위치 유지의 직접 신호)

`Read(file_path, offset, limit)` 호출을 순서대로 뽑아:

- `reads_total`, 파일별 offset 열(예: SQLITE 1,201,…,2001 / ME 1,…,601).
- `rereads`: 같은 (file, offset) 을 두 번 이상 읽은 횟수 — **압축 뒤 되읽기가 「위치를 잃었다」의 신호**(§5 에서 경계와 짝짓는다).
- `out_of_order`: offset 이 앞선 것보다 작아진 횟수(파일 안에서).
- `split_reads`: limit ≠ 200 인 읽기(카드가 허용 — 위반 아님, 참고).
- `skipped`: 기대 offset 가운데 한 번도 안 읽은 것(청크 누락과 같이 나타난다).

## 5. ② 압축 유지력 — 경계 하나당 관측 하나

### 5-1. 경계 찾기

전사에서 **JSON 으로 파싱해** `record["type"] == "system" and record["subtype"] == "compact_boundary"` 인 레코드만.
`compactMetadata` 에 `trigger`(auto/manual), `preTokens`, `postTokens`, `durationMs` 가 있다.
**문자열 grep 으로 세지 않는다** — Read 결과 안에 「compact_boundary」 라는 낱말이 들어 있어 거짓 양성이 났다(우리 실측).

경계 `b_i` 의 **직전 상태**: 그 앞에서 마지막으로 완료된 청크(Write 된 `chunk_*.csv` 가운데 마지막)와 그때 진행 중이던
청크(Read 했으나 아직 Write 안 한 것). **기대 재개점** `expected_next` = 마지막 완료 청크 다음 offset
(B 는 `progress.txt` 의 「다음: <파일> <시작줄>」 이 곧 기대 재개점 — 둘이 다르면 장부가 틀린 것도 적는다).

### 5-2. 경계마다 재는 것

| 항목 | 값 | 정의 |
| --- | --- | --- |
| `resume` | `correct` / `reread` / `skipped` / `wrong_file` / `none` | 경계 뒤 **첫 Read** 의 (file, offset): 기대 재개점이면 correct; 이미 Write 한 청크를 다시 읽으면 reread(위치 상실); 기대점을 건너뛰면 skipped; (B) `progress.txt` 를 먼저 읽고 기대점을 읽으면 correct. Read 없이 끝나면 none |
| `steps_to_resume` | 정수 | 경계 뒤 기대 재개점을 읽기까지 든 tool_use 수(0 = 첫 호출). reread 마다 커진다 |
| `constraint_kept` | 0/1 | 경계 뒤 창(다음 경계 전까지, 없으면 끝까지)에서 §4-1 위반 0 이고, 그 창에서 Write 된 청크 행에 제외 위반·형식 오류 0 |
| `reasked` | 0/1 | 창 안에 되묻기(§4-1) |
| `scope_drift` | 0/1 | 창 안에 카드에 없는 산출물(보조 스크립트·임시 파일·다른 형식) 을 썼나 |
| `items_lost` | 정수 + 구간 | truth 의 missing 가운데 **경계에 걸친 청크**(직전에 Read 하고 그 뒤에야 Write 했거나 끝내 안 쓴 청크) 구간에 속한 것. 다른 곳의 missing 은 ①에만 센다 |
| `goal_restated` | 0/1 | 경계 뒤 첫 어시스턴트 본문이 과제(항목 뽑기·CSV·청크·제외 날짜)를 언급하나 — **참고만** |
| `pre_tokens`, `post_tokens`, `trigger` | 그대로 | 경계 메타데이터 |
| `vrs_tail_before` | 0/1 | §0 `vrs_tail` — 이 경계 앞까지의 턴이 경계 **전에** 스토어에 들어갔나(영수증 `ts` < 경계 시각, `lines[1]` ≥ 경계 직전 줄) |
| `vrs_recall_after` | 정수 | 경계 뒤 창에서 회수 훅이 주입한 횟수(`recall_context.log` `injected`), 그중 대화 행(`대화 —`)이 든 횟수를 괄호로 |
| `vrs_turn_opened` | 0/1 | 경계 뒤 창에서 주입된 대화 행의 「원문 위치」 Read 를 실제로 불렀나(`memory_use_log`) — 참고 |

관측 한 줄(`observations.jsonl`):
```
{"run": "...", "model": "...", "ledger": "A|B", "boundary": i, "trigger": "auto", "pre_tokens": 512345,
 "expected_next": ["SQLITE", 1401], "first_read": ["SQLITE", 1201], "resume": "reread", "steps_to_resume": 3,
 "constraint_kept": 1, "reasked": 0, "scope_drift": 0, "items_lost": 0, "lost_ranges": [], "goal_restated": 1,
 "vrs": 1, "vrs_tail_before": 1, "vrs_recall_after": 2, "vrs_turn_opened": 1}
```

### 5-3. 집계

조건(모델 × 장부 × 문턱)마다: `n_boundaries`, `resume_correct_rate`, `mean_steps_to_resume`, `constraint_kept_rate`,
`reasked_rate`, `scope_drift_rate`, `items_lost_total`. **표본 수를 항상 같이 적는다**; `n_boundaries = 0` 인 조건은 「미측정」.

## 6. 전사 파싱 규칙

- jsonl 한 줄 = 레코드 하나. `json.loads` 실패 줄은 세고 건너뛴다(잘린 마지막 줄 등).
- `tool_use`: `record["type"] == "assistant"` 의 `message.content[]` 가운데 `{"type": "tool_use", "name": ..., "input": {...}}`.
  순서는 파일 순서(`timestamp` 로도 확인). 서브에이전트 전사(`isSidechain: true`)는 그 에이전트의 실행이다.
- 토큰: 어시스턴트 레코드마다 `message.usage` — `input_tokens + cache_creation_input_tokens + cache_read_input_tokens`
  = 그 호출의 **문맥 크기**, `output_tokens` = 출력. `tokens_total` = 모든 호출의 (문맥 + 출력) 합(비용), `context_peak`
  = 문맥 크기 최댓값(경계의 `preTokens` 와 맞아야 한다 — 안 맞으면 짝이 틀린 전사).
- 시간: 첫 레코드 `timestamp` ~ 마지막 레코드 `timestamp`.
- 도구 호출 수: 이름별(`Read/Write/Edit/Grep/Glob/Bash/AskUserQuestion/…`).
- 모델: 어시스턴트 레코드의 `message.model`(조건 표와 대조 — 다르면 그 실행은 무효).

## 7. 비용 지표 (실행 하나당, 참고)

`tokens_total`, `context_peak`, `duration_s`, `tool_calls{name: n}`, `compactions[{trigger, pre, post, duration_ms}]`.
①·② 에 넣지 않고 표의 오른쪽 열로만 둔다(잣대는 「덜 잃었나」이고 「덜 썼나」는 부수).

## 8. 실행 하나의 최종 표

```
run  model  ledger  autocompact  vrs | recall  missing  gaps  spurious(off1)  excl_viol  dup  fmt  chunks | scan_tools  outside_writes  reasked
     | compactions  resume_correct/n  steps  constraint_kept/n  items_lost  vrs_tail_before/n  vrs_recall_after | tokens_total  context_peak  duration
```

`vrs` 열이 0 이면 ② 열은 비워 두고 「VRS 없음」이라 적는다(§0).

같은 셀(조건)에 실행이 하나뿐이면 「1회」라고 적고 결론을 내리지 않는다. recall 차이 **1 % 미만(≈7 항목)은 소음**이다.
A 대 B 비교는 같은 모델·같은 문턱에서만.

## 9. 채점기가 하지 말 것 — 우리가 실제로 틀렸던 것

1. `compact_boundary` 를 **문자열 grep** 으로 세기 → Read 결과 속 낱말이 잡혀 1 이 나왔다. JSON 파싱 + type/subtype.
2. `text30` 을 점수에 넣기 → `**`·백틱·공백 접기 규칙 차이로 293/738 이 「불일치」로 나왔지만 항목 누락이 아니었다. 참고 열로만.
3. `report.txt` 의 자기 셈을 믿기 → B2 는 747 이라 적었고 채점기 행은 738. 채점기가 센다.
4. 경로·기대 숫자 하드코딩(`C:\Users\asm\...`, 738) → 인자로 받고, truth 는 입력에서 다시 봉인해 해시로 대조.
5. 줄 어긋남(off-by-one)을 missing 1 + spurious 1 로 두 번 벌주고 말 안 하기 → 쌍으로 따로 보고.
6. 압축 0회 실행을 ② 표본으로 넣기 → ② 는 경계가 있어야 한다.
7. 「목표 복창」 같은 말을 주 지표로 삼기 → 말은 참고, 행동(재개점·제약·잃은 항목)이 점수.
8. 다른 카드 판본의 출력 폴더를 섞기(`runA` 와 `runA2`) → 실행 id 로 짝.
9. BOM·CRLF·인코딩 오류를 예외로 죽이기 → `errors="replace"`, BOM 제거, 형식 오류로 세기.
10. 형식 오류 행을 조용히 버리기 → 세고 예를 남긴다.

## 10. 회귀 앵커 — 채점기가 맞는지 먼저 확인

같은 입력·같은 truth 로 우리 2라운드 출력(`runA2/`, `runB2/`)을 채점하면 **이 숫자가 나와야 한다**:

| run | recall | missing / gaps | spurious (off-by-one) | 제외 위반 | 중복 | 형식 | 청크 | date_match | text30(참고) | report 자기 셈 | 압축 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| A2 (장부 없음, 소넷) | **0.9851** (727/738) | 11 / `ME:203-248`(통째), `SQLITE:1143-1145` | 19 (SQLITE 561, 572-575 …: 줄 어긋남) | 0 | 0 | 0 | 15/15 | 726/727 | 501/727 | 747 | 0 |
| B2 (장부, 소넷) | **0.9986** (737/738) | 1 / `ME:644` | 1 (ME:643 — 644 와 off-by-one 1쌍) | 0 | 0 | 0 | 15/15 | 736/737 | 403/737 | 747 | 0 |

(2026-09-21 에 기존 `score.py` 로 다시 돌려 확인한 값. text30 열이 낮은 것이 §9-2 의 이유다.)

**A2·B2 는 둘 다 「VRS 없음」 실행이다** — `sonnet-medium` 서브에이전트에 Read/Write/Edit 만 주어 회수 훅도 유입 훅도 물리지
않았고, 압축도 0회였다. 앵커로 쓰는 것은 ① 채점기의 정확성뿐이다. 이 두 실행에서 연속성·압축 유지력에 대해 말할 수 있는
것은 없다(§0). 로그·메모리에 남은 A2/B2 수치를 층의 결과로 읽지 않는다.

앵커가 안 맞으면 채점기부터 고친다. 그 다음에야 새 모델·새 조건을 돌린다.

## 11. 판정 스토어에 넣을 때 (선택)

경계 하나 = 관측 하나. 주장은 영어 소문자 한 문장, 예:
`the progress ledger keeps the task position across a compaction` — B 의 경계는 `intervention`(장부 있음),
A 의 경계는 `counterfactual`(없음), outcome = `resume == correct and items_lost == 0` 이면 success.
프로듀서는 채점기 id(`compaction-grader`), 출처는 `ab:<run>#<boundary>`, 맥락은 모델 이름. 실행 단위의 ① 수치는
같은 주장의 관측이 아니다 — 넣으려면 다른 주장(`… without a compaction`)으로.

---

기존 `score.py` 는 §3·§4 의 대부분과 §6 의 일부(경계 수·도구 수)를 이미 한다. **새로 짜지 말고** 거기에 §4-1·§4-2·§5 를
붙이고 인자화(§1)·JSON 파싱(§9-1)·앵커(§10)를 맞춘다.
