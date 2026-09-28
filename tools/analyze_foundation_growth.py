#!/usr/bin/env python3
"""Describe measured full-VRS growth; never promote concepts or change edges."""
import argparse
import csv
import json
from pathlib import Path


def lines(path):
    return [json.loads(line) for line in path.read_text().splitlines() if line.strip()]


def quantile(values, p):
    values = sorted(values)
    x = (len(values) - 1) * p
    i = int(x)
    return values[i] + (values[min(i + 1, len(values) - 1)] - values[i]) * (x - i)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('measurement', type=Path)
    args = parser.parse_args()
    root = args.measurement
    meta = json.loads((root / 'inputs.json').read_text())
    rounds = lines(root / 'rounds.jsonl')
    latest = rounds[-1]
    names = {r['tag']: r['name'] for r in lines(root / 'tag-dictionary.jsonl')}
    direct_rows = [r for r in lines(root / 'concept-growth.jsonl') if r['window'] == latest['round']]
    pair_rows = [r for r in lines(root / 'concept-pair-growth.jsonl') if r['round'] == latest['round']]
    direct = {r['concept']: r for r in direct_rows}
    pairs = {r['tag']: r for r in pair_rows}
    assert len(direct_rows) == len(direct) == len(pair_rows) == len(pairs) == meta['tags']
    assert set(direct) == set(pairs) == set(names)
    assert [sum(r['delta'][i] for r in direct.values()) for i in range(3)] == latest['tag_image']
    # Every undirected pair contributes to each of its two endpoint summaries.
    assert [sum(r['delta'][i] for r in pairs.values()) for i in range(3)] == [2 * n for n in latest['tag_tag']]
    rows = []
    for tag, d in direct.items():
        p = pairs[tag]
        a, r, u = p['delta']
        assert a + r + u == p['N']
        assert sum(d['delta']) == d['N'] == meta['images']
        rows.append(dict(tag=tag, name=names[tag], images=d['distinct_supported'],
                         coverage=d['supported_fraction'],
                         direct_accept=d['delta'][0], direct_reject=d['delta'][1], direct_abstain=d['delta'][2],
                         pair_accept=a, pair_reject=r, pair_abstain=u,
                         pair_n=p['N'], pair_accept_fraction=a / p['N'] if p['N'] else None,
                         pair_net=a-r, observed_partner_edges=p['observed_partner_edges'],
                         positive_net_edges=p['positive_net_edges'], active_edges=p['active_edges']))
    rows.sort(key=lambda r: r['coverage'], reverse=True)
    gaps = sorted([dict(after_rank=i+1, higher=rows[i]['name'], lower=rows[i+1]['name'],
                        gap=rows[i]['coverage']-rows[i+1]['coverage']) for i in range(len(rows)-1)],
                  key=lambda x: x['gap'], reverse=True)
    all_counts = [sum(latest[k][i] for k in ('tag_image', 'tag_tag')) for i in range(3)]
    result = dict(round=latest['round'], images=meta['images'], concepts=meta['tags'],
                  judgments=sum(all_counts), counts=all_counts, seconds=latest['seconds'],
                  loop_judgments_per_second=sum(all_counts)/latest['seconds'],
                  timing_scope='round verification, application and checkpoint; excludes initial loading',
                  positive_pair_net_concepts=sum(r['pair_net'] > 0 for r in rows),
                  zero_observation_concepts=sum(r['pair_n'] == 0 for r in rows),
                  abstention_policy_calibratable=all_counts[2] > 0,
                  promotion_applied=False, top_by_coverage=rows[:15], largest_coverage_gaps=gaps[:10],
                  coverage_sensitivity=[dict(cutoff=t, concepts=sum(r['coverage'] >= t for r in rows))
                                        for t in (.5, .67, .7, .75, .8, .9, .95)],
                  quantiles={key:{str(p):quantile([r[key] for r in rows],p)
                                  for p in (0,.25,.5,.75,.9,.95,.99,1)}
                             for key in ('coverage','pair_net','pair_n','pair_accept_fraction')})
    workers = [json.loads(p.read_text()) for p in sorted((root / 'dispatch').glob('worker-*.json'))]
    assert sum(w['judgments'] for w in workers) == result['judgments']
    result['worker_counts'] = workers
    with (root / 'concept-measurements.csv').open('w') as f:
        writer = csv.DictWriter(f, fieldnames=list(rows[0])); writer.writeheader(); writer.writerows(rows)
    (root / 'analysis.json').write_text(json.dumps(result, ensure_ascii=False, indent=2))
    text = ['# Full completed-integer-VRS growth measurement', '',
            f"Round {result['round']}: {meta['images']:,} originals, {meta['tags']:,} concepts; {result['judgments']:,} judgments.",
            f"Accept/reject/abstain: {all_counts}. No promotion or edge deletion applied.", '',
            '| Concept | Coverage | Images | Pair accept | Pair reject | Pair net growth |',
            '|---|---:|---:|---:|---:|---:|']
    for row in rows[:15]:
        text.append(f"| {row['name']} | {row['coverage']:.4%} | {row['images']} | {row['pair_accept']} | {row['pair_reject']} | {row['pair_net']:+d} |")
    text += ['', f"Largest coverage gap: rank {gaps[0]['after_rank']} to {gaps[0]['after_rank']+1}, {gaps[0]['gap']:.4%}.",
             f"Positive pair net growth occurs in {result['positive_pair_net_concepts']:,} concepts; positivity alone does not isolate ubiquitous concepts.",
             '', '## Evidence boundary', '',
             'Direct membership fractions and incident pair fractions are different propositions and denominators. They are not pooled.',
             'The pair endpoint summary counts each pair twice across concepts; the global judgment total does not.',
             'This corpus has zero abstention judgments, so no abstention-dependent cutoff can be calibrated.',
             'Candidate boundaries describe this image corpus; there is no labeled foundational-knowledge ground truth or held-out corpus here.',
             'Other retained raw whole-file records have no measured relation growth and are disclosed in measurement-scope.json. Inventory entries are not VRS experience observations.',
             'Prior interrupted/invalidated runs and duplicate snapshots were not counted as new independent experiences.',
             'The existing completed integer-collision runner was used, not a claim of full migration into the new block-ingress runtime.',
             'No numerical promotion policy has been installed.']
    (root / 'REPORT.md').write_text('\n'.join(text)+'\n')
    live = json.loads((root / 'live.json').read_text())
    live.update(phase='completed',checkpoint_complete=True,exit_code=0,analysis_complete=True)
    (root / 'live.json').write_text(json.dumps(live))
    print(json.dumps({k:result[k] for k in ('judgments','counts','positive_pair_net_concepts','quantiles','coverage_sensitivity','worker_counts')},ensure_ascii=False))


if __name__ == '__main__':
    main()
