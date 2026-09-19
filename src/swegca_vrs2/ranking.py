"""Order already-recalled evidence without changing its membership or verdicts."""
from __future__ import annotations

from dataclasses import replace
import math
import re

from .store import keys


def _query_terms(memory, query):
    result = {}
    for word in re.findall(r'\w+', query.casefold()):
        if memory.episode_ids_for_cue(word):
            result[word] = None
            continue
        runs = re.findall(r'[가-힣]+|[a-z]+|[0-9]+', word)
        if len(runs) > 1:
            for run in runs:
                if memory.episode_ids_for_cue(run):
                    result[run] = None
            continue
        available = [key for key in keys(word) if memory.episode_ids_for_cue(key)]
        if available:
            longest = max(len(key) for key in available)
            for key in available:
                if len(key) == longest:
                    result[key] = None
    return tuple(result)


def rank(memory, receipt, query):
    """Return one reordered four-stage receipt and auditable ordering metadata.

    Every candidate, replay and current verdict remains present exactly once.
    Candidate membership and actual conflict closure were decided by main before
    this ordering step. Scores never confer truth, promotion or action authority.
    """
    candidates = receipt.recall.candidates
    if len(candidates) < 2:
        return receipt, dict(ranking_method='query_word_coverage_bm25_v1',
                             ranked_candidate_count=len(candidates))
    terms = _query_terms(memory, query)
    count = max(1, memory.episode_count)
    lengths = {row.episode_id: len(memory.episode(row.episode_id).cues) for row in candidates}
    average = max(1., sum(lengths.values()) / len(lengths))
    evidence = {row.episode_id: [0, 0.] for row in candidates}
    for term in terms:
        posting = memory.episode_ids_for_cue(term)
        frequency = len(posting)
        information = math.log1p((count - frequency + .5) / (frequency + .5))
        for identifier in posting:
            if identifier not in evidence:
                continue
            coverage, score = evidence[identifier]
            normalized = 1.2 * (.7 + .3 * lengths[identifier] / average)
            evidence[identifier] = [coverage + 1, score + information * 2.2 / (1 + normalized)]
    direct = query.strip().casefold()
    ordered = tuple(sorted(candidates, key=lambda row: (
        -int(row.episode_id == direct), -evidence[row.episode_id][0],
        -evidence[row.episode_id][1], -row.cue_overlap, row.episode_id)))
    if ordered == candidates:
        return receipt, dict(ranking_method='query_word_coverage_bm25_v1',
                             ranked_candidate_count=len(candidates))
    ids = tuple(row.episode_id for row in ordered)
    replays = {row.episode_id: row for row in receipt.replay.episodes}
    judgments = {row.episode_id: row for row in receipt.re_evidence.judgments}
    updated = replace(receipt, recall=replace(receipt.recall, candidates=ordered),
        replay=replace(receipt.replay, episodes=tuple(replays[i] for i in ids)),
        re_evidence=replace(receipt.re_evidence,
            judgments=tuple(judgments[i] for i in ids),
            selected_support=tuple(i for i in ids if judgments[i].verdict == 'support'),
            selected_refutation=tuple(i for i in ids if judgments[i].verdict == 'refute')))
    return updated, dict(ranking_method='query_word_coverage_bm25_v1',
                         ranked_candidate_count=len(candidates))
