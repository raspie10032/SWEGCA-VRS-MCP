# -*- coding: utf-8 -*-
"""The evaluation table (GRADING §8) from the runs' score.json files: one row per run, ① / ② / gate / cost.

    vrs2-venv python report_ag.py --bench C:/Users/asm/mcp/ab-compaction --runs agB,agBV,agBV-sonnet [--md out.md]
"""
import argparse, io, json, os, sys


def row(bench, run):
    p = os.path.join(bench, run, "score.json")
    if not os.path.isfile(p):
        return f"| {run} | (no score.json) |"
    r = json.load(io.open(p, encoding="utf-8"))
    q, c, d, a, v, cost = r["quality"], r["constraints"], r["read_discipline"], r["aggregate"], r.get("vrs") or {}, r["cost"]
    ledger = "B" if "B" in run.split("-")[0] else "A"
    vrs_cond = "on" if run.split("-")[0].endswith("V") else "off"
    def flag(x):
        return "—" if x is None else ("1" if x else "0")
    gate = "—" if not v else ("1" if v.get("vrs") else f"0 (tail {flag(v.get('vrs_tail'))} · recall {flag(v.get('vrs_recall'))} · rows {flag(v.get('vrs_rows'))})")
    kinds = a.get("resume_kinds") or {}
    n = a.get("n_boundaries") or 0
    def rate(x):
        return "—" if x is None else f"{x:.2f}"
    return (f"| {run} | {r.get('model')} | {ledger} | {vrs_cond} | {'done' if r.get('done') else 'not done'}{' (timed out)' if r.get('timed_out') else ''}, nudges {r.get('nudges')} | "
            f"{q['recall']:.4f} ({q['truth'] - q['missing']}/{q['truth']}) | {q['missing']} / {', '.join(q['gaps'][:4])}{'…' if len(q['gaps']) > 4 else ''} | "
            f"{q['spurious']} | {q['exclusion_violations']} | {q['duplicates']} | {q['format_errors']} | {q['chunk_files']} | "
            f"{c['scan_tools']} | {c['outside_writes']} | {c['reasked']} | {d['rereads']} / {d['out_of_order']} / {len(d['skipped'])} | "
            f"{r['compactions']} | {kinds.get('correct', 0)}/{n} | {rate(a.get('mean_steps_to_resume'))} | {rate(a.get('within_chunk_reread_rate'))} | "
            f"{rate(a.get('constraint_kept_rate'))} | {a.get('items_lost_total')} | {a.get('recall_boundaries', 0)}/{n} | {gate} | "
            f"{cost.get('prompt_tokens'):,} / {cost.get('output_tokens'):,} | {cost.get('context_peak'):,} | {cost.get('duration_s')} s |")


HEAD = ("| run | model | ledger | VRS | end | recall | missing / gaps | spurious | excl | dup | fmt | chunks | scan tools | outside writes | reasked | "
        "rereads / out-of-order / skipped chunks | compactions | resume correct/n | steps | within-chunk reread | constraint kept | items lost | recall called/n | VRS gate | "
        "tokens in / out | context peak | duration |\n"
        "|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bench", required=True); ap.add_argument("--runs", required=True); ap.add_argument("--md", default=None)
    a = ap.parse_args()
    sys.stdout.reconfigure(encoding="utf-8")
    lines = [HEAD] + [row(a.bench, r) for r in a.runs.split(",")]
    text = "\n".join(lines) + "\n"
    print(text)
    if a.md:
        io.open(a.md, "w", encoding="utf-8", newline="\n").write(text)


if __name__ == "__main__":
    main()
