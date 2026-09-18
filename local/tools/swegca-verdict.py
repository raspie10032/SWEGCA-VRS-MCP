"""결판난 것을 판정(`obs:` 기록)으로 남긴다. 규약을 도구가 강제한다.

    python swegca-verdict.py <판정.json> [<판정.json> ...]
    python swegca-verdict.py --list            # 있는 판정 목록 (가설·극성·asks 수)
    python swegca-verdict.py --check           # asks 없는 판정, 극성 섞인 가설

판정 JSON 의 모양:

    {"slug": "fts5-segments-must-be-merged",
     "claim": "an fts5 index keeps its query speed without periodic segment merging",
     "outcome": "failure",
     "context": "session-5e9f-2026-09-11-segments",
     "asks": ["색인이 왜 점점 느려지나", "FTS5 optimize 를 언제 거나"],
     "observation": {"claim": "…", "measured": "…", "numbers": {...}, "cause": "…",
                     "fix": "…", "why": "…"},
     "evidence": ["C:/…/session-log.md", "C:/…/stateful.py"]}

## 왜 도구인가

판정은 2026-09-10 부터 손으로 넣었다. 두 번은 즉석 스크립트로, 한 번은 MCP 도구로 —
그때마다 `asks` 를 빼먹거나 `producer_id`·`observed_at_ns` 를 빠뜨려 되돌아왔다.
그리고 회수 훅이 판정을 고를 때 **맞은 개념이 `asks` 에 있어야** 한다(실제 프롬프트
125개로 잡은 규칙). `asks` 없는 판정은 훅에 안 잡힌다 — 있어도 없는 것이다.

## 규약 (auto-memory `swegca-vrs-mcp-setup.md` 「판정 기록」)

- `claim` 은 **주장**을 영어 한 문장으로. `outcome` 은 그 주장이 버텼는지(`success`)
  아닌지(`failure`). 가설 하나에 극성 하나 — 섞이면 `unresolved` 로 강화가 막힌다.
- `asks` 는 **그 판정을 찾을 때 실제로 물을 말** 두어 개. 과업 어휘로, 한국어로.
  결과가 아니라 질문을 적는다(「출처 제한이 왜 안 걸리나」).
- `context` 는 콜론 없는 손기록 라벨. 콜론이 있으면 압축 도구의 스냅숏 정리에 걸린다.
- `observation` 에 숫자를 넣는다. 무엇을 쟀고 무엇이 나왔나.

이 도구는 코어를 직접 열어 쓴다(서버 서명과 같은 키). 떠 있는 MCP 서버와 동시에 써도
된다 — SQLite 가 직렬화하고 revision 재확인이 남의 커밋을 덮지 않는다.
"""
import json
import re
import sys
import time
from pathlib import Path

sys.path.insert(0, r"C:\Users\asm\mcp\SWEGCA-VRS-MCP\src")
from swegca_vrs_mcp.observations import Authenticator, Observation  # noqa: E402
from swegca_vrs_mcp.stateful import CoreError, StatefulCore  # noqa: E402

STORE = Path(r"C:\Users\asm\mcp\swegca-memory")
KEYS = Path(r"C:\Users\asm\mcp\swegca-keys\producer-keys.json")
PRODUCER = "asm-agent"
MIN_ASKS = 2
AXES = ("observational", "counterfactual", "intervention", "cross_context")   # SWEGCA accumulator required_axes


def existing(core):
    """event_id -> (hypothesis, outcome, asks 수). 판정만."""
    out = {}
    for eid, body in core._db.execute(
            "SELECT o.event_id, o.body FROM observations o JOIN record_meta m "
            "ON m.event_id = o.event_id WHERE o.event_id LIKE 'obs:%' AND m.superseded = 0 "
            "ORDER BY o.seq"):    # 덮인 옛 판본은 뺀다 — asks 판으로 덮인 것들이 있다
        event = json.loads(body)["event"]
        asks = (event.get("observation") or {}).get("asks") or []
        out[eid] = (event["hypothesis_id"], event["outcome"], len(asks))
    return out


def validate(spec, known):
    problems = []
    slug = spec.get("slug", "")
    if not re.fullmatch(r"[a-z0-9][a-z0-9-]{8,}(@[a-z0-9-]+)?", slug):
        problems.append("slug: 소문자·숫자·하이픈 9자 이상(옛 판정을 닫을 때만 같은 slug 에 @태그)")
    claim = spec.get("claim", "")
    if not (claim and re.fullmatch(r"[a-z][a-z0-9 ,'()/-]+", claim) and " " in claim):
        problems.append("claim: 영어 소문자 한 문장(주장)")
    if spec.get("outcome") not in ("success", "failure"):
        problems.append("outcome: success 또는 failure")
    context = spec.get("context", "")
    if not context or ":" in context:
        problems.append("context: 콜론 없는 라벨")
    asks = spec.get("asks") or []
    if len(asks) < MIN_ASKS or not all(isinstance(a, str) and len(a) >= 4 for a in asks):
        problems.append(f"asks: 찾을 때 물을 말 {MIN_ASKS}개 이상")
    # SWEGCA 증거 축(2026-09-15): 판정이 어떤 종류의 증거인지 작성자가 선언한다. observational = 관측했다,
    # counterfactual = 없었으면/반대면 어땠나를 대조했다, intervention = 손대서 결과를 봤다, cross_context = 다른
    # 맥락(프로젝트·세션)에서 재현했다. 없으면 observational. 넷이 다 차야 accumulator 가 accept 할 수 있다.
    axes = spec.get("axes", ["observational"])
    if not isinstance(axes, list) or not axes or any(a not in AXES for a in axes):
        problems.append(f"axes: {', '.join(AXES)} 중 하나 이상의 리스트")
    obs = spec.get("observation")
    if not isinstance(obs, dict) or not {"claim", "measured", "why"} <= set(obs):
        problems.append("observation: dict 에 claim·measured·why 는 있어야")
    if not spec.get("evidence"):
        problems.append("evidence: 경로 하나 이상")
    if f"obs:{slug}" in known:
        problems.append(f"obs:{slug} 이미 있음 — 다른 slug 로")
    supersedes = spec.get("supersedes") or []
    if not isinstance(supersedes, list) or any(not isinstance(x, str) or not x.startswith("obs:") for x in supersedes):
        problems.append("supersedes: 덮을 옛 event_id(obs:…) 리스트")
    for eid, (hyp, outcome, _) in known.items():
        if hyp == claim and outcome != spec.get("outcome") and eid not in supersedes:
            problems.append(f"같은 가설에 반대 극성 {eid} 가 있다 — 섞이면 unresolved. "
                            "정말 뒤집혔으면 supersedes 로 덮어야 한다")
    for eid in supersedes:
        if eid not in known:
            problems.append(f"supersedes: {eid} 는 살아 있는 판정이 아니다")
    return problems


def build(spec):
    observation = dict(spec["observation"])
    observation["asks"] = list(spec["asks"])
    axes = list(dict.fromkeys(spec.get("axes", ["observational"])))
    observation["axes"] = axes
    return Observation(
        event_id=f"obs:{spec['slug']}", hypothesis_id=spec["claim"], producer_id=PRODUCER,
        context_id=spec["context"], axis=axes[0],
        outcome=spec["outcome"], observation=observation, cues=[],
        evidence_refs=list(spec["evidence"]), observed_at_ns=time.time_ns(),
        supersedes=list(spec.get("supersedes") or []))     # 옛 판정을 닫는다(2026-09-17): 같은 slug 에 @태그, 극성 뒤집기 허용


def main(argv):
    if not argv:
        sys.exit(__doc__)
    auth = Authenticator.load(KEYS)
    if argv[0] == "--list":
        core = StatefulCore(STORE, writable=False, authenticator=auth)
        try:
            for eid, (hyp, outcome, asks) in existing(core).items():
                print(f"  {outcome:<8} asks{asks}  {hyp[:70]}")
        finally:
            core.close()
        return
    if argv[0] == "--check":
        core = StatefulCore(STORE, writable=False, authenticator=auth)
        try:
            known = existing(core)
        finally:
            core.close()
        bare = [eid for eid, (_, _, asks) in known.items() if asks < MIN_ASKS]
        by_claim = {}
        for eid, (hyp, outcome, _) in known.items():
            by_claim.setdefault(hyp, set()).add(outcome)
        mixed = [hyp for hyp, outcomes in by_claim.items() if len(outcomes) > 1]
        print(f"판정 {len(known)}건 · asks 없는 것 {len(bare)}건 · 극성 섞인 가설 {len(mixed)}건")
        for eid in bare:
            print(f"  asks 없음: {eid}")
        for hyp in mixed:
            print(f"  극성 섞임: {hyp[:70]}")
        return

    specs = [json.loads(Path(p).read_text(encoding="utf-8")) for p in argv]
    core = StatefulCore(STORE, writable=True, authenticator=auth)
    try:
        known = existing(core)
        rows = []
        for path, spec in zip(argv, specs):
            problems = validate(spec, known)
            if problems:
                sys.exit(f"{path}:\n  - " + "\n  - ".join(problems))
            rows.append(build(spec))
            known[f"obs:{spec['slug']}"] = (spec["claim"], spec["outcome"], len(spec["asks"]))
        sign = auth.signer_for(PRODUCER)
        receipt = core.ingest([sign(r) for r in rows],
                              request_id=f"verdict-{rows[0].event_id}-{len(rows)}",
                              expected_revision=core.status()["revision"])
        print(f"판정 {receipt['added']}건 기록 (authenticated {receipt['authenticated']}) "
              f"rev {receipt['revision']}")
        for r in rows:
            print(f"  {r.outcome:<8} {r.hypothesis_id[:70]}")
    except CoreError as failure:
        sys.exit(f"실패: {failure}")
    finally:
        core.close()
    mirror_to_vrs2()


def mirror_to_vrs2():
    """판정 이중 기록(2026-09-14): 회수 훅 v2 는 vrs2 스토어를 보므로 거기에도 넣는다.
    importer 가 v0.2 스토어의 살아 있는 판정을 읽어 없는 것만 데몬 ingest 로 넣는다(멱등).
    실패해도 판정 기록 자체는 끝난 뒤라 한 줄만 적고 넘어간다."""
    import subprocess
    cmd = [r"C:/Users/asm/mcp/vrs2-venv/Scripts/python.exe", r"C:/Users/asm/mcp/vrs2-import.py",
           "--only", "verdicts", "--daemon"]
    try:
        out = subprocess.run(cmd, capture_output=True, timeout=180)
        tail = (out.stdout or b"").decode("utf-8", "replace").strip().splitlines()[-1:] or ["(출력 없음)"]
        print(("vrs2 반영: " if out.returncode == 0 else "vrs2 반영 실패: ") + tail[0][:160])
    except Exception as failure:  # noqa: BLE001 — 이중 기록은 부수 작업
        print(f"vrs2 반영 실패: {failure!r}"[:200])


if __name__ == "__main__":
    main(sys.argv[1:])
