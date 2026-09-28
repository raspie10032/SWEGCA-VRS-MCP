# Real experience-pair run, 2026-09-28

Code: 3ccc3faf9602fa35bfa4ef4ff264cec84a2e4739. One real-data pass after the user's explicit request to run the corrected implementation. No long-term goal, Main merge, or live service replacement.

- Input occurrences: 6,274; unique experiences: 6,274; exact duplicates: 0.
- All distinct experience pairs: 19,678,401.
- Accept: 19,675,480 (99.9852%); reject: 0; abstain: 2,921.
- Stored strengths: 1.01 for approved pairs, 1 for abstained pairs.
- Builder runtime: 102.062 seconds; peak process RSS 101,748 KiB (~99.4 MiB).
- Shuffle seed: 1704. Fresh experience-pair graph; the earlier incompatible tag-pair strengths were not reused.

## Independent artifact verification

Every 19,678,401 pair identity was checked using a triangular-index bitmap: no duplicate or missing pair. CSV status totals match summary.json. All previous strengths are 1. A separate tag-membership intersection and whole-input witness sum agreed for 4,889 sampled/non-accepting pairs, including every abstention. Verification took 17.13 seconds.

The large support_observations total is repeated witness occurrences across different pair/member checks, not a count of independent experiences.

## Same unseen-image query

- Query SHA: 45b177d6ee53b9dfb57829371709ce54e5eb70325c9405073379d613bd78444c; absent from experiences.
- No query tags supplied. DINO routes to source 688; its 49 recorded tags activate experience connections.
- Additional tags: 5,392, exactly the same set as the earlier tag graph. Added 0, removed 0.
- Traversed stored paths: 221,389.
- Recall consumer runtime: 21.8152 seconds (full graph scan; excludes feature extraction). This does NOT meet 5ms.
- Same-input disconnect control: same source/49 seed tags, zero additional tags and zero traversed paths.
- Separate WD14 comparison: 59 labels; 45 appear somewhere in the huge additional set; top-20 overlap is only white_hair (1/20). These model labels are NOT ground truth. The changed top ordering is not proof of semantic improvement because approved strengths remain tied.

Pair-local verification and native-link-dependent consumption are demonstrated. Selective association quality and convergence are NOT demonstrated. A concrete cause of broad connectivity: 1girl occurs in 6,256 experiences and alone supplies a common member to 19,565,640 pairs. No rarity filter or altered verdict policy was introduced during this run. The current observation path does not provide explicit negative evidence, so it cannot manufacture rejections.

Artifacts: /var/home/raspie/Documents/Codex/vrs-experience-pairs-real-20260928 (summary.json, links.csv, check.py/check.json, recall.json, recall-comparison.json, disconnected.json, resource logs and sealed native blocks). Large data stays local; only this report is committed.
