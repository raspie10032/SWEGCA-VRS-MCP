#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""규모 곡선 — 합성 스토어를 N 건까지 키우며 적재·consolidation·회수·메모리·디스크를 잰다 (2026-09-18).

모델 토큰 0. 실제 라이브 스토어의 기록 본문을 표본으로 삼아(회사 데이터는 밖으로 안 나감 — 사본은 `vrs2-scale-lab/` 안에만)
낱말을 섞고 소금을 쳐 N 건을 만든다(cue 분포는 실제와 비슷, 내용은 다름). 단계마다 기록:
  N, 적재 ms/건(단계 평균), consolidation 1세대 초(16사이클)·영역 수·간선 수, 회수 중앙값/최대 ms(20 질의, auto/all),
  RSS MB, 디스크 MB. RSS 가 상한을 넘거나 MemoryError 면 거기서 멈추고 그 사실을 적는다 — G7(상주 계층)이 필수가 되는 지점.

    vrs2-venv python vrs2-scale-curve.py [--steps 10000,20000,50000,100000,200000] [--rss-cap-mb 4096] [--seed 7] [--batch 500]
결과: vrs2-scale-lab/curve.json + curve.log (단계마다 append — 중간에 죽어도 지금까지가 남는다)
"""
import argparse
import io
import json
import os
import random
import shutil
import statistics
import sys
import time

from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'src'))
STATE = os.environ.get('VRS2_STATE', str(Path.home() / '.local/share/swegca-vrs2-codex'))
LAB = os.environ.get('VRS2_SCALE_LAB', str(Path(STATE).parent / 'vrs2-scale-lab'))
CURVE = os.path.join(LAB, "curve.json")
LOG = os.path.join(LAB, "curve.log")


def log(msg):
    line = time.strftime("%H:%M:%S ") + msg
    print(line, flush=True)
    with open(LOG, "a", encoding="utf-8") as out:
        out.write(line + "\n")


def rss_mb():
    try:
        import psutil
        return psutil.Process().memory_info().rss / 1048576
    except Exception:
        pass
    try:
        import ctypes, ctypes.wintypes as w
        class C(ctypes.Structure):
            _fields_ = [("cb", w.DWORD), ("PageFaultCount", w.DWORD), ("PeakWorkingSetSize", ctypes.c_size_t),
                        ("WorkingSetSize", ctypes.c_size_t), ("QuotaPeakPagedPoolUsage", ctypes.c_size_t),
                        ("QuotaPagedPoolUsage", ctypes.c_size_t), ("QuotaPeakNonPagedPoolUsage", ctypes.c_size_t),
                        ("QuotaNonPagedPoolUsage", ctypes.c_size_t), ("PagefileUsage", ctypes.c_size_t), ("PeakPagefileUsage", ctypes.c_size_t)]
        c = C(); c.cb = ctypes.sizeof(C)
        ctypes.windll.psapi.GetProcessMemoryInfo(ctypes.windll.kernel32.GetCurrentProcess(), ctypes.byref(c), c.cb)
        return c.WorkingSetSize / 1048576
    except Exception:
        return -1.0


def sample_texts(limit=6000):
    """Read samples through the VRS owner API; never copy or inspect storage."""
    from swegca_vrs2.loopback import ensure_daemon
    client = ensure_daemon(STATE, allow_ingest=False)
    texts = []
    try:
        cursor = 0
        while len(texts) < limit:
            page = client.request('export_experiences', after_sequence=cursor,
                                  max_records=min(512, limit - len(texts)))
            for item in page.get('rows', []):
                text = item['observation'].get('text') or ''
                if 80 <= len(text) <= 4000:
                    texts.append(text)
            cursor = int(page.get('next_sequence', cursor))
            if page.get('complete'):
                break
    finally:
        client.close()
    if not texts:
        raise ValueError('no_vrs_experience_samples')
    return texts


def synth(texts, i, rng):
    """실제 본문 둘을 섞고 소금을 쳐 새 기록 하나."""
    a, b = rng.choice(texts), rng.choice(texts)
    words = (a + " " + b).split()
    rng.shuffle(words)
    body = " ".join(words[: rng.randint(40, 260)])
    return f"합성 {i} {rng.randint(0, 99999)} " + body


def queries_from(texts, rng, n=20):
    out = []
    for _ in range(n):
        words = [w for w in rng.choice(texts).split() if 2 <= len(w) <= 12]
        rng.shuffle(words)
        out.append(" ".join(words[:5]))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--steps", default="10000,20000,50000,100000,200000")
    ap.add_argument("--rss-cap-mb", type=float, default=4096)
    ap.add_argument("--seed", type=int, default=7)
    ap.add_argument("--batch", type=int, default=500, help="한 세대에 넣는 건수(묶음 세대, 2026-09-18); 1 = 옛 경로")
    ap.add_argument("--fresh", action="store_true", help="기존 랩 스토어를 지우고 처음부터")
    a = ap.parse_args()
    if not 0 < a.rss_cap_mb <= 4096:
        ap.error("--rss-cap-mb must be within the 4096 MB VRS limit")
    sys.stdout.reconfigure(encoding="utf-8")
    os.makedirs(LAB, exist_ok=True)
    steps = [int(x) for x in a.steps.split(",")]
    store_dir = os.path.join(LAB, "store")
    if a.fresh:
        shutil.rmtree(store_dir, ignore_errors=True)
    os.makedirs(store_dir, exist_ok=True)
    rng = random.Random(a.seed)
    texts = sample_texts()
    log(f"표본 본문 {len(texts)}건 | 단계 {steps} | 묶음 {a.batch} | RSS 상한 {a.rss_cap_mb:.0f} MB | 랩 {store_dir}")
    from swegca_vrs2.store import Main
    m = Main(store_dir, allow_ingest=True)
    curve = json.load(io.open(CURVE, encoding="utf-8")) if os.path.exists(CURVE) else []
    n = m.memory.episode_count
    queries = queries_from(texts, rng)
    try:
        for target in steps:
            if target <= n:
                continue
            t0 = time.perf_counter(); made = 0
            while n < target:
                # 묶음 세대(2026-09-18): --batch 건을 한 세대로(그래프 재구성 한 번). 1 이면 옛 한 건 = 한 세대.
                rows = [dict(request_id=f"synth:{k}", text=synth(texts, k, rng), source=f"synth:{k}", revision="1",
                             metadata=dict(kind="synthetic_experience", project="synthetic")) for k in range(n, min(target, n + a.batch))]
                m.ingest_many(rows) if a.batch > 1 else m.ingest(rows[0])
                n += len(rows); made += len(rows)
                if made % 2000 < len(rows):
                    log(f"  … {n} 건, {(time.perf_counter() - t0) / made * 1000:.1f} ms/건, RSS {rss_mb():.0f} MB")
                    if rss_mb() > a.rss_cap_mb:
                        raise MemoryError(f"RSS {rss_mb():.0f} MB > cap at {n} records during ingest")
            ingest_ms = (time.perf_counter() - t0) / max(1, made) * 1000
            t1 = time.perf_counter(); r = m.consolidate(); cons_s = time.perf_counter() - t1
            lat = {}
            for scope in ("auto", "all"):
                ms = []
                for q in queries:
                    t = time.perf_counter(); m.recall(q, None, exclude_kinds=("fs_listing", "test"), region_scope=scope); ms.append((time.perf_counter() - t) * 1000)
                lat[scope] = dict(median=round(statistics.median(ms), 1), max=round(max(ms), 1))
            m.checkpoint()
            disk = sum(path.stat().st_size for path in Path(store_dir).rglob('*')
                       if path.is_file()) / 1048576
            row = dict(records=n, batch=a.batch, ingest_ms_per_record=round(ingest_ms, 1), consolidation_s=round(cons_s, 1),
                       regions=(r or {}).get("regions"), edges=(r or {}).get("edges"), promoted=(r or {}).get("promoted"),
                       recall_ms=lat, rss_mb=round(rss_mb()), disk_mb=round(disk, 1), ts=time.strftime("%Y-%m-%d %H:%M:%S"))
            curve.append(row)
            io.open(CURVE, "w", encoding="utf-8").write(json.dumps(curve, ensure_ascii=False, indent=1))
            log(f"N={n}: 적재 {ingest_ms:.0f} ms/건 | consolidation {cons_s:.1f} s ({row['regions']} 영역, {row['edges']} 간선) | "
                f"회수 auto {lat['auto']['median']}/{lat['auto']['max']} ms, all {lat['all']['median']}/{lat['all']['max']} ms | RSS {row['rss_mb']} MB | 디스크 {row['disk_mb']} MB")
            if rss_mb() > a.rss_cap_mb:
                log(f"RSS 상한 초과({rss_mb():.0f} MB) — 여기서 멈춤: 상주 계층(G7) 없이는 이 규모가 한계"); break
    except MemoryError as failure:
        log(f"MemoryError: {failure} — 상주 계층(G7) 없이는 이 규모가 한계")
        curve.append(dict(records=n, error=str(failure)[:200], ts=time.strftime("%Y-%m-%d %H:%M:%S")))
        io.open(CURVE, "w", encoding="utf-8").write(json.dumps(curve, ensure_ascii=False, indent=1))
    finally:
        try:
            m.close()
        except Exception:
            pass
    log("끝")


if __name__ == "__main__":
    main()
