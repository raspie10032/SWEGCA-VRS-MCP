#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""위임 패킷 — 서브에이전트에 붙일 기억 영수증을 과제문에서 만든다 (2026-09-14).

    C:/Users/asm/mcp/vrs2-venv/Scripts/python.exe C:/Users/asm/mcp/vrs2-delegate.py "과제문" [옵션]

    --cwd PATH        과제가 속한 프로젝트 디렉터리(기본: 현재 디렉터리). 정션이면 실제 프로젝트로 푼다.
    --project SLUG    프로젝트 슬러그를 직접 지정(예: C--Users-asm-Desktop-SQLITE). --cwd 보다 우선.
    --records N       기록 최대 N 건(기본 4; 프롬프트 훅은 1)   --snippet N   토막 글자 수(기본 900)
    --packet-only     영수증 블록만 낸다(과제문·꼬리말 없이)     --out FILE    파일로도 쓴다
    --no-tail         답 형식 꼬리말(근거·연 파일·모름) 생략
    --code            코딩 과제용 꼬리말(범위·검증·yaml 반환: status/changed_files/tests/new_facts/state_corrections …)

낸 것(stdout, UTF-8)을 그대로 Agent 프롬프트로 쓴다: 과제문 + [첨부: 기억 영수증] + 꼬리말.

근거(2026-09-14 A/B, 소넷 12과제×3조건): 짧은 지시 + 이 패킷이 손 브리핑과 같은 정확도(23/24·25/26 vs
24/24·26/26)에 내가 쓰는 글자는 1/13, 서브에이전트 토큰·시간·연 파일은 셋 중 최저. 영수증이 빗나가도(6건 중 2건)
서브에이전트가 memory/ 를 스스로 뒤져 맞혔으므로 폴백 계층은 없다 — 꼬리말이 그 길을 가리킨다. 위험은 낡은
판본으로 끄는 것뿐이라 「후보일 뿐, 최신 로그로 확인」을 꼬리말에 둔다. 서브에이전트에는 swegca-vrs2 MCP 가
안 보이므로(swegca-vrs 만) 훅의 「memory_context 로 묻는다」 줄은 뺀다.

코딩 꼬리말(--code, 2026-09-15)은 Downloads 의 「Claude Code용 VRS 2.1 실행 규칙」(작성자 미확인, 사본
`mcp/docs-ref-CLAUDE_CODE_VRS_2.1.md`)에서 맞는 것만 가져왔다: 범위 지키기·최소 변경·실패마다 하나 바꾸기·테스트 로그는
추출만·범위 밖은 보고만·yaml 반환. 안 가져온 것: 탐색 예산(max_extra_files 2 — 실측 B 조건에서 영수증 빗나갈 때 3~6 파일을
스스로 뒤져 맞혔다), 「known_state 는 충돌 때만 재검증」(낡은 판본이 통과 — N1). 보탠 것: new_facts 마다 근거(파일:줄)와
찾을 때 묻는 말, `state_corrections`(영수증이 코드와 다를 때) — 이게 있어야 스토어가 배운다.

고르는 규칙은 회수 훅 v2(`~/.claude/hooks/recall_context_v2.py`)의 choose/render 를 그대로 쓴다 — 훅이 바뀌면
패킷도 같이 바뀐다. 영수증은 `~/.claude/hooks/delegate_packet.log`.
"""
import argparse
import importlib.util
import io
import json
import os
import sys
import time

HOME = os.path.expanduser("~")
HOOK = os.path.join(HOME, ".claude", "hooks", "recall_context_v2.py")
LOG = os.path.join(HOME, ".claude", "hooks", "delegate_packet.log")
PROJECTS = os.path.join(HOME, ".claude", "projects")
STATE = os.environ.get("VRS2_STATE") or r"C:\Users\asm\mcp\vrs2-memory"
LIMIT = 12          # 데몬에서 받아 오는 후보 수(훅은 10)
RECORDS = 4         # 위임 패킷은 프롬프트 훅(1건)보다 넉넉히 — A/B 에서 정답 포함 6/6·4/6 이던 값
SNIPPET = 900
HOOK_TAIL = "더 필요하면 swegca-vrs2 의 memory_context 로 같은 질문을 묻는다. 안 맞으면 무시해도 된다."

TAIL = """
답은 한 문단(필요하면 표 하나)으로, 마지막에 두 줄을 붙여라:
근거: <네가 실제로 읽은 파일 경로·판정 이름>
연 파일: <이번 답을 위해 직접 연 파일 수(첨부된 내용은 세지 않는다)>
확실하지 않은 부분은 「모름」이라 적어라. 지어내지 마라."""

CODE_TAIL = """
실행 규칙:
- 과제 범위 안의 파일만 고친다. 범위 밖 문제는 고치지 말고 out_of_scope_findings 에 적는다.
- 읽기는 필요한 만큼 자유롭게(영수증이 빗나가면 memory/ 와 코드를 직접 뒤진다). 고치기는 최소 변경, 요구되지 않은
  리팩터링·새 의존성·public API 변경은 하지 않는다. 원본은 사본을 만들어 고치고 검증 뒤 바꾼다.
- 고친 뒤 반드시 검증한다(지정 테스트 → 관련 테스트 → 타입/린트). 실패하면 전체 로그 대신 실패 테스트 이름·핵심 오류·
  원인 가설만 적는다. 같은 수정을 같은 테스트로 되풀이하지 않는다 — 실패마다 가설·코드·테스트·범위 중 하나는 바꾼다.
- 막히면 탐색을 넓히기보다 blockers 에 무엇이 막는지 적고 멈춘다.

결과는 아래 yaml 로 반환한다(빈 항목은 []):
status: success | partial | blocked | failed     # success 는 목표·범위·테스트 셋 다 충족일 때만
changed_files:
  - path: <경로>
    summary: <한 줄>
tests:
  - command: <실행한 명령>
    result: pass | fail
    note: <필요할 때만>
new_facts:                                       # 오래 쓸 기술 사실만(함수의 실제 책임·숨은 의존·테스트 명령·문서와 코드의 불일치)
  - fact: <한 문장>
    evidence: <파일:줄 또는 명령 출력>
    asks: [<이걸 나중에 찾을 때 실제로 물을 말 둘 이상>]
state_corrections:                               # 첨부 영수증이 코드와 다른 곳 — 스토어를 고치는 근거가 된다
  - said: <영수증이 말한 것>
    actual: <실제>
    evidence: <파일:줄>
remaining_risks: [<미검증·회귀 위험>]
blockers: [<진행을 막은 것>]
out_of_scope_findings:
  - file: <경로>
    issue: <한 줄>
    recommendation: <별도 작업 등>
files_opened: <직접 연 파일 수(첨부는 세지 않는다)>
확실하지 않은 것은 「모름」이라 적는다. 지어내지 않는다."""


def load_hook():
    spec = importlib.util.spec_from_file_location("recall_context_v2", HOOK)
    hook = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(hook)
    return hook


def memory_dir(project):
    """프로젝트 슬러그의 memory/ 실제 경로(정션이면 원본)."""
    path = os.path.join(PROJECTS, project, "memory")
    return os.path.realpath(path) if os.path.isdir(path) else path


def subagent_tail(project, memory):
    return ("\n".join([
        "위 영수증은 스토어가 고른 후보일 뿐이다 — 판단은 네가 하되 먼저 읽어라. 낡은 판본일 수 있으니",
        f"세션 로그의 최신 항목으로 확인하라. 여기 없으면 {memory} 의 session-log.md 와 *.md 를",
        "직접 뒤져라(grep 가능). 첨부 항목의 경로는 Read 로 열 수 있다.",
    ]))


def build(task, project, records, snippet, packet_only, tail, code=False):
    hook = load_hook()
    hook.MAX_RECORDS = records
    hook.SNIPPET = snippet
    sys.path.insert(0, hook.SRC)
    from swegca_vrs2.loopback import ensure_daemon
    client = ensure_daemon(STATE, allow_ingest=True)
    try:
        stems = hook.prompt_stems(task)
        exclude = ["test"] if any(t.startswith(w) for w in hook.LOCATION_WORDS for t in stems) else ["test", "fs_listing"]   # test: 시험 기록은 언제나 제외(훅과 같은 규칙, 2026-09-15)
        packet = client.request("hook_recall", query=task[:4000], limit=LIMIT, snippet=snippet, exclude_kinds=exclude, region_scope="auto")
    finally:
        client.close()
    verdicts, rows = hook.choose(packet, project, hook.prompt_stems(task))
    memory = memory_dir(project)
    if verdicts or rows:
        body = hook.render(packet, verdicts, rows)
        if body.endswith(HOOK_TAIL):
            body = body[: -len(HOOK_TAIL)].rstrip("\n")
        block = "[첨부: 기억 영수증]\n" + body + "\n" + subagent_tail(project, memory)
    else:
        block = ("[첨부: 기억 영수증] 스토어에 맞닿는 기록이 없었다(후보 "
                 f"{packet.get('candidate_count', 0)}건 중 실을 것 없음). {memory} 의 session-log.md 와 *.md 를 직접 뒤져라.")
    chosen = [r["source"] for r in verdicts + rows]
    if packet_only:
        text = block
    else:
        text = task.strip() + "\n\n" + block + ("\n" + (CODE_TAIL if code else TAIL) if tail else "")
    return text, chosen, packet.get("candidate_count", 0)


def note(**fields):
    fields["ts"] = time.strftime("%Y-%m-%d %H:%M:%S")
    try:
        with open(LOG, "a", encoding="utf-8") as out:
            out.write(json.dumps(fields, ensure_ascii=False) + "\n")
    except OSError:
        pass


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("task")
    ap.add_argument("--cwd", default=os.getcwd())
    ap.add_argument("--project", default=None)
    ap.add_argument("--records", type=int, default=RECORDS)
    ap.add_argument("--snippet", type=int, default=SNIPPET)
    ap.add_argument("--packet-only", action="store_true")
    ap.add_argument("--no-tail", action="store_true")
    ap.add_argument("--code", action="store_true")
    ap.add_argument("--out", default=None)
    a = ap.parse_args()
    sys.stdout.reconfigure(encoding="utf-8")
    started = time.perf_counter()
    hook = load_hook()
    project = a.project or hook.project_of(a.cwd)
    text, chosen, candidates = build(a.task, project, a.records, a.snippet, a.packet_only, not a.no_tail, a.code)
    if a.out:
        io.open(a.out, "w", encoding="utf-8", newline="\n").write(text)
    sys.stdout.write(text + "\n")
    note(task=a.task[:120], project=project, chosen=chosen, candidates=candidates, chars=len(text),
         ms=round((time.perf_counter() - started) * 1000), out=a.out, code=a.code)


if __name__ == "__main__":
    main()
