# Sizing — recommended bundle size (2026-09-19)

A **bundle** is one store directory served by one resident process (`swegca_vrs2.loopback`). Records
are never split (a source episode is one episode — paper contract); what is split, when a bundle
outgrows its machine, is the *store*: one bundle per project or per company system, addressed by name.

## Recommendation

| machine class | peak RSS budget for the daemon | settled RSS | records per bundle |
| --- | --- | --- | --- |
| 16 GB workstation shared with an IDE, browser and company programs (this PC) | **3.5 GB** | ~2 GB | **60,000** (default) |
| 16 GB with little else running | 5.5 GB | 3.2 GB | ~100,000 |
| 32 GB dedicated server | — | — | 400,000+ (extrapolated, not measured) |

The default `bundle_limit` is 60,000. It is **soft**: nothing is refused. `status` and every
`ingest`/`ingest_many` result carry `bundle = {records, limit, fill, over}`, and the Stop-hook
reindex prints `[기억] 뭉치 n/60,000 건 (x%)` at every stop from 90 % of the limit — the cue to plan a
split. Configure it in `~/.claude/vrs2.json` (`"bundle_limit": 60000`, written by
`vrs2-install.py --bundle-limit`), or `--bundle-limit` on the daemon.

## Where the numbers come from

The scale curve of 2026-09-18/19 (`docs/RESULT_LEDGER.md` §6b; `local/bench/vrs2-scale-curve.py`,
synthetic records mixed from live text, batch generations of 500) on an i5-13400 / 16 GB:

* memory: ~32 KB per record settled (after a checkpoint), ~55 KB at the peak between checkpoints
  (170k records → 5.7 GB peak, 2.2 GB settled at 200k). Linear in N; no wall inside 200k.
* time: ~300 edges per record; ingest, consolidation and full-store recall grow linearly with N
  (10k → 200k: ingest 17 → 279 ms/record in batches, consolidation 12 → 187 s, `all` recall
  0.7 → 17.7 s).
* the per-prompt line: the hook's recall must stay near 1 s. Region-scoped (`auto`) recall on this
  corpus crosses 1 s between 50k and 100k records (0.84 s at 50k, 1.39 s at 100k). The live corpus
  shares far fewer cues than the synthetic one (5.6k records → 69 ms), so this is the pessimistic side.

The memory budget and the latency line point at the same place — 60k — on this class of machine,
which is why it is the default rather than either number alone.

## When a bundle reaches the limit

1. Split by project/system: register a second bundle in `~/.claude/vrs2.json` and route the project's
   Stop hook to it (`vrs2-install.py --bundle <id>=<dir> --bundle-of <project slug>=<id>`, or edit
   `bundles` / `bundle_of` by hand); records keep their ids and sources. No second daemon: the primary
   daemon answers for every registered bundle (the G7 resident layer, `docs/VRS_REGIONS.md`
   「Resident layer」) — the primary hot, the others warm (index only, refreshed from their journals),
   hot while a Stop hook writes into them, `hot_bundles` at once (default 1).
2. A warm bundle costs its index, not its graph: on the live store (5.7k records) a warm view is 189 MB
   against 293 MB hot and loads in 0.4 s against 1.2 s; its rows come back in index order (BM25 without
   VRS strengths or regions) and are tagged 「뭉치 <id>·warm」 in the hook. The primary keeps the engine's
   full recall. Cross-bundle addressing: `lookup <episode_id>` names the bundle; the hook packet carries
   `bundle` on every row.
3. Before splitting, two software levers can move the line without a design change: the candidate loop
   in `recall_candidates` (still Python per candidate, ~0.2 ms each) and per-region candidate caps.
