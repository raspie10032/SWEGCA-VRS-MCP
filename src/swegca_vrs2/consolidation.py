"""Non-destructive, main-owned derived navigation over exact source episodes.

Groups are structural pointers, never a factual summary, new observation, VRS
reinforcement or authority. Current conflicts and revisions are read afresh from
the full-current main snapshot whenever a group is returned.
"""
from __future__ import annotations

import hashlib
import json
SCHEMA = 'swegca-vrs2-derived-experience-v1'


def _checksum(body):
    return hashlib.sha256(body.encode('utf-8')).hexdigest()


def _successor_groups(main, record):
    from .store import freeze_view
    identifier = record['derived_experience_id']
    consolidations = main.consolidations.set(identifier, freeze_view(record))
    groups = main.parent_groups
    for parent in record['parent_episode_ids']:
        groups = groups.set(parent, groups.get(parent, frozenset()) | {identifier})
    return consolidations, groups


def _record(main, parents, pair):
    from .store import digest
    episodes = [main.memory.episode(parent) for parent in parents]
    shared = set(episodes[0].cues)
    for episode in episodes[1:]:
        shared.intersection_update(episode.cues)
    identifier = 'consolidated:' + digest((SCHEMA, pair, parents))
    return dict(schema_version=SCHEMA, derived_experience_id=identifier,
        creation_pair_snapshot_id=pair, parent_episode_ids=parents,
        shared_navigation_cues=sorted(shared),
        source_addresses=[list(episode.source_addresses) for episode in episodes],
        parent_revisions=[episode.revision for episode in episodes],
        parent_outcomes=[episode.steps[0].outcome for episode in episodes],
        factual_summary_claimed=False, independent_observation_added=False,
        grants_authority=False)


def restore_groups(main):
    """Validate durable pointers after the full journal/checkpoint is restored."""
    from .store import canonical
    valid_pairs = {main.pair.snapshot_id}
    valid_pairs.update(operation[2] for operation in main.operations.values())
    for identifier, body, checksum in main.db.execute(
            'SELECT id,body,checksum FROM consolidations ORDER BY id'):
        if _checksum(body) != checksum:
            raise ValueError('consolidation_checksum_integrity_failed')
        try:
            record = json.loads(body)
            parents = record['parent_episode_ids']
            if (type(record) is not dict or record['schema_version'] != SCHEMA
                    or type(parents) is not list or len(parents) < 2
                    or parents != sorted(set(parents))
                    or any(parent not in main.memory.records for parent in parents)
                    or record['creation_pair_snapshot_id'] not in valid_pairs
                    or _record(main, parents, record['creation_pair_snapshot_id']) != record
                    or record['derived_experience_id'] != identifier
                    or canonical(record) != body):
                raise ValueError('consolidation_integrity_failed')
        except (KeyError, TypeError, ValueError) as exc:
            raise ValueError('consolidation_integrity_failed') from exc
        main.consolidations, main.parent_groups = _successor_groups(main, record)


def consolidate(main, arguments):
    """Explicit maintenance write for one snapshot-bound parent set."""
    from .store import canonical, digest
    if (type(arguments) is not dict or set(arguments) !=
            {'parent_episode_ids', 'expected_pair_snapshot_id'}):
        raise ValueError('invalid_consolidation_fields')
    if arguments['expected_pair_snapshot_id'] != main.pair.snapshot_id:
        raise ValueError('snapshot_mismatch')
    parents = arguments['parent_episode_ids']
    if (type(parents) is not list or len(parents) < 2 or
            any(type(parent) is not str for parent in parents) or
            len(set(parents)) != len(parents)):
        raise ValueError('invalid_consolidation_parents')
    parents = sorted(parents)
    if any(parent not in main.memory.records for parent in parents):
        raise ValueError('unknown_consolidation_parent')
    identifier = 'consolidated:' + digest((SCHEMA, main.pair.snapshot_id, parents))
    if identifier in main.consolidations:
        return dict(status='consolidation_unchanged', derived_experience_id=identifier,
            parent_episode_ids=parents, distinct_source_episode_added=0,
            grants_authority=False)
    record = _record(main, parents, main.pair.snapshot_id)
    body = canonical(record)
    next_consolidations, next_parent_groups = _successor_groups(main, record)
    main.db.execute('BEGIN IMMEDIATE')
    try:
        main.db.execute('INSERT INTO consolidations VALUES (?,?,?)',
                        (identifier, body, _checksum(body)))
        main.db.execute('COMMIT')
    except BaseException:
        if main.db.in_transaction:
            main.db.execute('ROLLBACK')
        raise
    main.consolidations, main.parent_groups = next_consolidations, next_parent_groups
    return dict(status='consolidation_recorded', derived_experience_id=identifier,
        parent_episode_ids=parents, distinct_source_episode_added=0,
        grants_authority=False)


def navigation(main, candidate_ids):
    """Related derived objects with current revision/conflict state and parents."""
    groups = set()
    for parent in candidate_ids:
        groups.update(main.parent_groups.get(parent, ()))
    result = []
    for identifier in sorted(groups):
        record = main.consolidations[identifier]
        parents = []
        propositions = set()
        for parent in record['parent_episode_ids']:
            episode = main.memory.episode(parent)
            observed = episode.steps[0].observation
            proposition = observed.get('proposition_id')
            if proposition:
                propositions.add(proposition)
            parents.append(dict(episode_id=parent,
                source_addresses=episode.source_addresses, revision=episode.revision,
                historical_outcome=episode.steps[0].outcome,
                proposition_id=proposition,
                evidence_polarity=observed.get('evidence_polarity'),
                superseded_by=main.memory.superseded.get(parent)))
        conflicts = []
        for proposition in sorted(propositions):
            active = [main.memory.episode(parent) for parent in
                main.memory.propositions.get(proposition, ())
                if parent not in main.memory.superseded]
            if {episode.steps[0].observation.get('evidence_polarity') for episode in active} == {'support', 'refute'}:
                conflicts.append(proposition)
        result.append(dict(derived_experience_id=identifier,
            creation_pair_snapshot_id=record['creation_pair_snapshot_id'],
            current_pair_snapshot_id=main.pair.snapshot_id,
            parent_episode_ids=record['parent_episode_ids'],
            shared_navigation_cues=record['shared_navigation_cues'],
            parents=parents, current_conflicting_propositions=conflicts,
            source_expansion_required=True, factual_summary_claimed=False,
            independent_observation_added=False, grants_authority=False))
    return tuple(result)
