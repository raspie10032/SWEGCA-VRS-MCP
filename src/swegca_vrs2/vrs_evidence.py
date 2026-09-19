"""Evidence layer of the VRS core, per the SWEGCA architecture (vrs-regions, 2026-09-15).

Reads the current generation into the architecture's objects and runs its evidence accounting
unchanged (``engine.mosaic_evidence_accumulator``, vendored from SWEGCA-Architecture):

* hypotheses = explicit propositions (``proposition:<id>``) and cues (``cue:<term>``, a
  generalized hypothesis: the concept a record touches);
* evidence observations = the resolved records touching a hypothesis, one per record:
  ``(hypothesis, evidence_address = record source, source_family = source file, context =
  project, axis, outcome support/refute from the record's polarity, producer, confidence)``.
  The producer declares the axes (record ``metadata.axes``: observational / counterfactual /
  intervention / cross_context); a record without a declaration is observational. A record whose
  context differs from the hypothesis's first context is additionally cross-context evidence —
  that is a structural fact, not a producer claim;
* pending or superseded records are not evidence;
* per hypothesis the accumulator folds the observations in journal order — duplicate addresses
  and expired observations are discounted by the accumulator itself — and its decision
  (accept / reject / abstain with reason, Wilson bounds, effective samples, source and context
  diversity) is the architecture's decision, reported as such. With one producer the accumulator
  abstains on source diversity: that is the correct answer, not a defect to relax.

From that the numerical kernel gets continuous inputs:

* hypothesis ``direct`` = tanh(ratio x sufficiency) with ratio = (effective support - effective
  refute) / effective samples over the required axes (the original organizer's ratio, with the
  accumulator's diversity-weighted counts) and sufficiency = min(1, samples / minimum per axis):
  one observation moves a hypothesis a little, four from different sources move it fully;
* hypothesis ``unresolved`` = opposing evidence from different source families (the arbiter's
  directional conflict) or a suspected regime change;
* record weight ``w = confidence x (1 - contradiction) x (1 - uncertainty)`` (the arbiter's
  proposal weight): confidence 1 for an explicit claim, .5 for an outcome-only record;
  contradiction = the opposing share on the record's primary hypothesis; uncertainty =
  1 - sufficiency of that hypothesis. ``w`` is the record's direct (tanh) and the base strength
  of its edges, so only well-evidenced records can reach the promotion threshold at all
  (the kernel clamps to base x [.25, 4]).
"""
from __future__ import annotations

import math
from collections.abc import Mapping

import numpy as np

from .engine import mosaic_evidence_accumulator as acc

CONFIG = acc.EvidenceAccumulatorConfig()
AXES = CONFIG.required_axes
CONFIDENCE_CLAIM = 1.0
CONFIDENCE_OUTCOME = 0.5


def record_polarity(episode, superseded):
    """+1 / -1 for a resolved record (outcome, else explicit evidence polarity), 0 when unresolved."""
    step = episode.steps[0]
    if episode.episode_id in superseded:
        return 0
    if step.outcome == 'success':
        return 1
    if step.outcome == 'failure':
        return -1
    polarity = step.observation.get('evidence_polarity')
    return 1 if polarity == 'support' else -1 if polarity == 'refute' else 0


def _axes_of(metadata):
    # G11 (2026-09-19): the store hands metadata back frozen (mappingproxy); the old ``isinstance(dict)`` test
    # made every declared axis observational — no live hypothesis had an intervention or counterfactual sample
    declared = metadata.get('axes') if isinstance(metadata, Mapping) else None
    if isinstance(declared, (list, tuple)):
        axes = [a for a in declared if a in AXES]
        if axes:
            return tuple(dict.fromkeys(axes))
    return ('observational',)


def _source_family(source):
    return source.split('#', 1)[0] or source


class Hypothesis:
    __slots__ = ('id', 'state', 'first_context', 'support_sources', 'refute_sources', 'decision')

    def __init__(self, hid):
        self.id = hid
        self.state = acc.EvidenceAccumulatorState.empty(hid, CONFIG)
        self.first_context = None
        self.support_sources = set()
        self.refute_sources = set()
        self.decision = None

    def observe(self, address, family, context, axes, outcome, producer, confidence, step):
        if self.first_context is None:
            self.first_context = context
        axes = list(axes)
        if context != self.first_context and 'cross_context' not in axes:
            axes.append('cross_context')
        for k, axis in enumerate(axes):
            observation = acc.EvidenceObservation(self.id, address if k == 0 else f'{address}@{axis}', family, context, axis,
                                                  outcome, step, producer_id=producer, producer_confidence=confidence)
            self.state = acc.update_accumulator(self.state, observation, CONFIG, current_step=step).state
        (self.support_sources if outcome == 'support' else self.refute_sources).add(family)

    def finish(self):
        self.decision = acc.assess_accumulator(self.state, CONFIG)
        return self.decision

    # numerical inputs for the kernel
    def counts(self):
        axes = {a.name: a for a in self.state.axes}
        required = [axes[n] for n in AXES]
        support = sum(a.effective_support for a in required)
        refute = sum(a.effective_refute for a in required)
        return support, refute, support + refute

    def direct(self):
        support, refute, samples = self.counts()
        if samples <= 0:
            return 0.0
        sufficiency = min(1.0, samples / CONFIG.minimum_effective_samples_per_axis)
        return math.tanh((support - refute) / samples * sufficiency)

    def unresolved(self):
        # the arbiter's directional conflict: opposing evidence from different source families
        conflict = bool(self.support_sources and self.refute_sources and len(self.support_sources | self.refute_sources) > 1)
        regime = self.decision is not None and self.decision.reason == 'regime_change_suspected'
        return conflict or regime

    def summary(self):
        d = self.decision
        support, refute, samples = self.counts()
        axes = {a.name: a for a in self.state.axes}
        return dict(status=d.status, reason=d.reason, posterior_mean=round(d.posterior_mean, 4),
                    causal_lower_bound=round(d.causal_lower_bound, 4), overall_upper_bound=round(d.overall_upper_bound, 4),
                    effective_samples=round(samples, 3), source_diversity=d.source_diversity,
                    context_diversity=d.context_diversity, regime_change_score=round(d.regime_change_score, 4),
                    support=round(support, 3), refute=round(refute, 3), unresolved=self.unresolved(),
                    # G11 (2026-09-19): what the accumulator has per axis and how many distinct producers,
                    # contexts and source families fed it — so a caller can name what is still missing
                    axes={n: round(axes[n].effective_samples, 3) for n in AXES},
                    producers=len(self.state.producer_ids), contexts=len(self.state.context_hashes),
                    families=len(self.state.source_families))


def gaps(summary):
    """G11: what a hypothesis still lacks before the accumulator's checks pass, from its ``summary()`` dict —
    per axis the effective samples short of the minimum (each distinct (source family, context) group counts
    once, whatever the number of runs), then producers / contexts / source families short of the diversity
    minimums. Empty when nothing is short (the decision is then accept, reject or 'uncertain' on the bounds).
    A summary from before G11 (no ``axes``) yields only what its diversity counts allow."""
    if not summary:
        return {}
    out = {}
    axes = summary.get('axes')
    if axes:
        short = {n: round(CONFIG.minimum_effective_samples_per_axis - axes.get(n, 0.0), 2) for n in AXES}
        short = {n: v for n, v in short.items() if v > 0}
        if short:
            out['axes'] = short
    producers = summary.get('producers')
    need_producers = max(CONFIG.minimum_source_diversity, CONFIG.minimum_context_diversity)
    if producers is not None and producers < need_producers:
        out['producers'] = need_producers - producers
    contexts = summary.get('contexts')
    if contexts is not None and contexts < CONFIG.minimum_context_diversity:
        out['contexts'] = CONFIG.minimum_context_diversity - contexts
    families = summary.get('families')
    if families is not None and families < CONFIG.minimum_source_diversity:
        out['families'] = CONFIG.minimum_source_diversity - families
    return out


AXIS_KO = {'observational': '관측', 'counterfactual': '반사실', 'intervention': '개입', 'cross_context': '교차맥락'}


def gaps_text(summary):
    """One short Korean tag for a hook line: ``abstain: 개입 0/4·반사실 0/4·프로듀서 2/4`` (``accept(causal_lower_bound)`` once nothing is short)."""
    if not summary:
        return ''
    g = gaps(summary)
    parts = []
    axes = summary.get('axes') or {}
    for n, short in (g.get('axes') or {}).items():
        parts.append(f"{AXIS_KO.get(n, n)} {axes.get(n, 0):g}/{CONFIG.minimum_effective_samples_per_axis}")
    if 'producers' in g:
        parts.append(f"프로듀서 {summary.get('producers')}/{summary.get('producers') + g['producers']}")
    if 'contexts' in g:
        parts.append(f"맥락 {summary.get('contexts')}/{CONFIG.minimum_context_diversity}")
    if 'families' in g:
        parts.append(f"출처 {summary.get('families')}/{CONFIG.minimum_source_diversity}")
    # the named gaps *are* the reason while any are open; the accumulator's own reason is shown once none is
    if parts:
        return f"{summary.get('status')}: " + '·'.join(parts)
    return f"{summary.get('status')}({summary.get('reason')})"


class Evidence:
    """Per-generation evidence view: hypotheses keyed by id, record weights keyed by episode id."""
    __slots__ = ('hypotheses', 'record_weight', 'record_primary', 'record_polarity', 'resolved_count', 'observation_count')

    def __init__(self):
        self.hypotheses = {}
        self.record_weight = {}
        self.record_primary = {}
        self.record_polarity = {}
        self.resolved_count = 0
        self.observation_count = 0

    def hypothesis(self, hid):
        h = self.hypotheses.get(hid)
        if h is None:
            h = self.hypotheses[hid] = Hypothesis(hid)
        return h

    def decision_of(self, hid):
        h = self.hypotheses.get(hid)
        return None if h is None or h.decision is None else h.summary()


def build(graph, memory):
    """Fold every resolved record's evidence into the hypotheses it touches (journal order)."""
    store = memory._store
    ids = store['ids']
    superseded = memory.superseded
    ev = Evidence()
    aliases = getattr(graph, 'aliases', None) or {}     # hypothesis registry: alias proposition -> canonical
    touched = {}                      # episode id -> [hypothesis ids] for the weight pass
    for row in range(memory.count):
        eid = ids[row]
        episode = memory.episode(eid)
        pol = record_polarity(episode, superseded)
        ev.record_polarity[eid] = pol
        if not pol:
            continue
        ev.resolved_count += 1
        obs = episode.steps[0].observation
        metadata = obs.get('metadata') or {}
        source = episode.source_addresses[0]
        family = _source_family(source)
        context = str(metadata.get('project') or metadata.get('scope') or 'global')
        producer = str(metadata.get('producer') or 'main')
        axes = _axes_of(metadata)
        proposition = obs.get('proposition_id')
        confidence = CONFIDENCE_CLAIM if proposition else CONFIDENCE_OUTCOME
        outcome = 'support' if pol > 0 else 'refute'
        # "an experience record becomes evidence only when it is addressable, relevant to a declared
        # hypothesis, and admitted under the evidence policy" — a resolved record without a declared
        # proposition is an outcome, not evidence for anything: undefined stays insufficient (I04), w = 0
        if not proposition:
            ev.record_weight[eid] = 0.0
            continue
        hid = 'proposition:' + aliases.get(proposition, proposition)
        ev.hypothesis(hid).observe(source, family, context, axes, outcome, producer, confidence, row)
        ev.observation_count += 1
        touched[eid] = (hid, confidence, pol)
    for h in ev.hypotheses.values():
        h.finish()
    # record weight = the arbiter's proposal weight against its proposition's accumulated evidence
    for eid, (primary, confidence, pol) in touched.items():
        support, refute, samples = ev.hypotheses[primary].counts()
        opposing = (refute if pol > 0 else support) / samples if samples > 0 else 0.0
        uncertainty = 1.0 - min(1.0, samples / CONFIG.minimum_effective_samples_per_axis)
        ev.record_weight[eid] = confidence * (1.0 - opposing) * (1.0 - uncertainty)
        ev.record_primary[eid] = primary
    return ev
