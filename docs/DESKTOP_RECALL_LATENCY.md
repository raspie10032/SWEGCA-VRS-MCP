# Desktop stdio to Recall entry diagnosis

## Boundary and reproduction

`benchmarks/desktop_recall_latency.py` launches the actual C++ wrapper, desktop
host, proxy and VRS, with a synthetic Python app-server backend. No installed app,
account, model, Claude bridge or existing experience store is used. Each run uses
an empty temporary Main and a new temporary session, initialized before timing.
Each size is sent five times; the first encounter is not assumed to be a hit.

The start is CLOCK_MONOTONIC immediately before the first native stdin write;
JSON construction is outside timing. The end is CLOCK_MONOTONIC at the existing
core Recall entry probe, before diagnostic marker output. Transport, framing,
JSON decoding and Déjà vu preceding Recall are included. This is NOT GUI keyboard
latency, Replay completion, a populated Main benchmark, or concurrent merge load.
The probe is linked only in the diagnostic executable; production has no marker.

```
taskset -c 6,7 make -j2 build/swegca-vrs-ingress-probe build/swegca-codex-wrapper build/swegca-desktop-host build/swegca-app-server-proxy
taskset -c 6,7 python3 benchmarks/desktop_recall_latency.py build 5
```

## Changes and observations

1. JSON string parsing and quoting append ordinary contiguous text in one range
   instead of one PMR string append per byte. UTF-8, control characters, escapes,
   surrogate pairing, duplicate member checks and nesting limits are preserved.
2. VRS stdio disables C stream synchronization and detaches cin from cout. Every
   result/error already flushes at its message boundary, so input no longer asks
   cout to flush once per byte. No SWEGCA decision or evidence check is removed.

Raw samples are in `benchmarks/results/desktop-recall-json-before.jsonl`,
`desktop-recall-json-after.jsonl` (change 1 only), and
`desktop-recall-untied-input.jsonl` (both changes). The baseline executable is
commit 6247971 with the diagnostic target linked. Runs used CPUs 6 and 7.

| Prompt bytes | Before median ms | Both changes median ms | Final maximum ms | Final samples >=1ms |
| --- | ---: | ---: | ---: | ---: |
| 128 | 0.018630 | 0.016940 | 0.033541 | 0/5 |
| 4096 | 0.099131 | 0.068690 | 0.073990 | 0/5 |
| 65536 | 1.333553 | 0.760727 | 0.999230 | 0/5 |
| 1048576 | 20.886566 | 12.623809 | 19.933958 | 5/5 |

These small sequential samples diagnose removable transport cost, not a latency
bound or statistically controlled attribution. The 64KiB maximum is very close
to 1ms. 1MiB still fails, and prior concurrent 3.507ms/1.071ms failures remain
unresolved. Input still performs bytewise frame extraction and several complete
JSON traversals before Recall; their separate costs have not yet been measured.
The overall input-to-Recall requirement remains unfulfilled.

## Regression evidence

JSON stream/parser tests: 38 checks, including long ordinary strings, all escaped
controls, Unicode, surrogate handling, malformed strings and exhausted PMR.
Actual stdio subprocess tests: 2,560 checks passed after both changes, including
request/response delivery, persistence/recovery and Replay/Re-evidence behavior.
