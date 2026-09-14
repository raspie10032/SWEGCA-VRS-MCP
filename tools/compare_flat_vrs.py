# -*- coding: utf-8 -*-
"""Bit-for-bit comparison: ported engine settlement vs ``flat_vrs.settle`` on real records.

  python tools/compare_flat_vrs.py <records> [<log path>]

Drives the ported ``store.Main`` (reference) on N real session-log chunks, materializes
each settled generation, and replays the same node/edge construction through
``flat_vrs`` from an empty graph. Reports, per generation, the number of nodes whose
float32 score differs and the maximum absolute difference. Zero is the goal; anything
else is reported, not hidden.
"""
import io
import os
import re
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'src'))
import numpy as np                                           # noqa: E402
import importlib.util                                        # noqa: E402
import subprocess                                            # noqa: E402


def reference_main():
    """The ported composition exactly as tagged v2.1.0 (before the flat adapter)."""
    root = Path(__file__).resolve().parents[1]
    source = subprocess.run(['git', '-C', str(root), 'show', 'v2.1.0:src/swegca_vrs2/store.py'],
                            capture_output=True, check=True).stdout.decode('utf-8')
    spec = importlib.util.spec_from_loader('swegca_vrs2.store_v210', loader=None)
    module = importlib.util.module_from_spec(spec)
    module.__package__ = 'swegca_vrs2'
    sys.modules['swegca_vrs2.store_v210'] = module
    exec(compile(source, 'store_v210.py', 'exec'), module.__dict__)
    return module.Main


Main = reference_main()                                      # noqa: E402
from swegca_vrs2 import flat_vrs                             # noqa: E402

LOG = sys.argv[2] if len(sys.argv) > 2 else r"C:\Users\asm\.claude\projects\C--Users-asm-Desktop-----\memory\session-log.md"
N = int(sys.argv[1]) if len(sys.argv) > 1 else 12


def chunks(path):
    text = io.open(path, encoding='utf-8', errors='replace').read()
    return [p.strip()[:1500] for p in re.split(r"\n(?=- 20\d\d-)", text) if len(p.strip()) >= 400]


def materialize(inputs):
    n, m = len(inputs.score), len(inputs.edges)
    score = np.array([inputs.score[i] for i in range(n)], dtype=np.float32)
    direct = np.array([inputs.direct[i] for i in range(n)], dtype=np.float32)
    src = np.array([inputs.edges['source'][e] for e in range(m)], dtype=np.uint32)
    dst = np.array([inputs.edges['target'][e] for e in range(m)], dtype=np.uint32)
    sign = np.array([inputs.edges['sign'][e] for e in range(m)], dtype=np.int8)
    strength = np.array([inputs.strength[e] for e in range(m)], dtype=np.float32)
    return score, direct, src, dst, sign, strength


def main():
    sys.stdout.reconfigure(encoding='utf-8')
    rows = chunks(LOG)[:N]
    state = Path(tempfile.mkdtemp()) / 'ref'
    ref = Main(state, allow_ingest=True)
    flat = flat_vrs.FlatGraph.empty()
    nodes = {}                         # name -> index, mirrors Graph.nodes construction order
    worst = 0
    print(f"{'k':>3} {'nodes':>7} {'edges':>8} {'ref ms':>8} {'flat ms':>8} {'rounds':>6} {'diff nodes':>10} {'max |Δ|':>10}")
    for k, text in enumerate(rows):
        t = time.perf_counter()
        ref.ingest(dict(request_id=f'c{k}', text=text, source=f'cmp://{k}', revision='1', outcome='pending'))
        ref_ms = (time.perf_counter() - t) * 1000
        episode = ref.memory.episode(ref.operations[f'c{k}'][1])
        # same construction as store.Graph.append
        new_names = [episode.episode_id] + [c for c in episode.cues if 'cue:' + c not in nodes]
        names = [episode.episode_id] + ['cue:' + c for c in new_names[1:]]
        for name in names:
            nodes[name] = len(nodes)
        center = nodes[episode.episode_id]
        endpoints = [nodes['cue:' + c] for c in episode.cues]
        src = [x for n in endpoints for x in (center, n)]
        dst = [x for n in endpoints for x in (n, center)]
        direct = np.zeros(len(names), np.float32); direct[0] = .1
        t = time.perf_counter()
        grown = flat.extend(new_direct=direct, new_src=src, new_dst=dst, new_sign=[1] * len(src),
                            new_strength=[.5] * len(src))
        settled, receipt = flat_vrs.settle(grown, [center, *endpoints], maximum_rounds=512)
        flat = grown.with_score(settled)
        flat_ms = (time.perf_counter() - t) * 1000
        r_score, r_direct, r_src, r_dst, r_sign, r_strength = materialize(ref.graph.inputs)
        assert np.array_equal(r_src, flat.src) and np.array_equal(r_dst, flat.dst), 'edge layout differs'
        assert np.array_equal(r_direct, flat.direct), 'direct differs'
        diff = flat.score.view(np.uint32) != r_score.view(np.uint32)
        delta = float(np.abs(flat.score.astype(np.float64) - r_score.astype(np.float64)).max()) if len(r_score) else 0.0
        worst = max(worst, int(diff.sum()))
        print(f"{k:>3} {len(flat.score):>7} {len(flat.src):>8} {ref_ms:>8.0f} {flat_ms:>8.1f} {receipt['rounds']:>6} {int(diff.sum()):>10} {delta:>10.2e}")
    ref.close()
    print(f"\n최대 불일치 노드 수: {worst}  ({'비트 동일' if worst == 0 else '차이 있음 — 위 표'})")


if __name__ == '__main__':
    main()
