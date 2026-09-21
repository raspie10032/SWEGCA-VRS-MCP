#!/usr/bin/env bash
# 2.2 evaluation runs on Antigravity, sequential: run -> dump/score, one line per run in eval-seq.log.
# usage: run_eval_seq.sh "agB:gemini-3.8-flash-medium agBV:gemini-3.8-flash-medium agBV-sonnet:claude-sonnet-4-6"
PY="C:/Users/asm/mcp/vrs2-venv/Scripts/python.exe"
BENCH="C:/Users/asm/mcp/ab-compaction"
STATE="C:/Users/asm/mcp/vrs2-eval-100k"
RECEIPTS="C:/Users/asm/mcp/vrs2-eval-100k/receipts"
SCORE="C:/Users/asm/mcp/SWEGCA-VRS-MCP-v2/local/bench/compaction/antigravity/score_ag.py"
SCRATCH="C:/Users/asm/AppData/Local/Temp/claude/C--Users-asm-Desktop-----/5e9f04f8-ba16-4daa-a148-2bac0eaf94e3/scratchpad"
cd /c/Users/asm/mcp || exit 1
for spec in $1; do
  run="${spec%%:*}"; model="${spec##*:}"
  echo "=== $(date '+%H:%M:%S') start $run ($model)" >> "$SCRATCH/eval-seq.log"
  mkdir -p "$BENCH/$run"
  "$PY" ag_eval.py run --model "$model" --card "$BENCH/task_$run.md" --workspace "$BENCH" --out "$BENCH/$run" \
      --limit 100000 --threshold 10000 --max-min 75 --nudge 2 --vrs-state "$STATE" --vrs-receipts "$RECEIPTS" \
      > "$SCRATCH/run-$run.log" 2>&1
  echo "=== $(date '+%H:%M:%S') finished $run exit $?" >> "$SCRATCH/eval-seq.log"
  "$PY" "$SCORE" --bench "$BENCH" --run "$run" --receipts "$RECEIPTS" --state-dir "$STATE" \
      --observations "$BENCH/observations.jsonl" > "$BENCH/$run/score.json" 2> "$SCRATCH/score-$run.err"
  "$PY" -c "import json,io; r=json.load(io.open(r'$BENCH/$run/score.json',encoding='utf-8')); q=r['quality']; a=r['aggregate']; print('$run', r['model'], 'done', r['done'], 'compactions', r['compactions'], 'recall', q['recall'], 'missing', q['missing'], 'chunks', q['chunk_files'], 'resume', a['resume_kinds'], 'kept', a['constraint_kept_rate'], 'lost', a['items_lost_total'], 'vrs', r['vrs'], 'tokens', r['cost']['prompt_tokens'])" >> "$SCRATCH/eval-seq.log" 2>&1
done
echo "=== $(date '+%H:%M:%S') all done" >> "$SCRATCH/eval-seq.log"
