"""Cold canonical snapshot hashing without an expanded body/JSON batch.

Matches the existing ensure_ascii=False, sort_keys=True, compact JSON recipe.
This changes physical computation only: no new schema, identity, or authority.
"""
from collections.abc import Mapping
import hashlib
import json
from types import MappingProxyType


_STREAM = object()


def _small_episode(row, budget):
    """Known header shape: normalize only bodies, not every header container."""
    texts = [row['episode_id'], row['revision'], row['verification_state']]
    sequences = [row['cues'], row['source_addresses']]
    if len(row['steps']) > 32: return _STREAM
    for step in row['steps']:
        texts.extend((step['phase'], step['judgment'], step['outcome']))
        sequences.extend((step['relations'], step['evidence_refs']))
    for values in sequences:
        if type(values) not in (tuple, list) or len(values) > 128:
            return _STREAM
        texts.extend(values)
    if any(type(text) is not str for text in texts): return _STREAM
    budget[0] -= len(texts) + len(sequences) + len(row['steps'])*8 + 8
    budget[1] -= sum(map(len, texts))
    if min(budget) < 0: return _STREAM
    for step in row['steps']:
        normalized = _small_plain(step['observation'], budget)
        if normalized is _STREAM: return _STREAM
        step['observation'] = normalized
    return row


def _small_plain(value, budget, depth=0):
    """Bounded native-container fast path; never probe lazy observation maps.

    Large strings/containers stay on the streaming path. The budget limits
    temporary serialization work, not experience access or hashed content.
    """
    budget[0] -= 1
    if budget[0] < 0 or depth > 8:
        return _STREAM
    kind = type(value)
    if kind is str:
        budget[1] -= len(value)
        return value if budget[1] >= 0 else _STREAM
    if value is None or kind in (bool, int, float):
        return value
    if kind in (dict, MappingProxyType):
        if len(value) > budget[0]: return _STREAM
        result = {}
        for key, item in value.items():
            # Let the general path preserve JSON's numeric-key/error behavior.
            if type(key) is not str: return _STREAM
            budget[1] -= len(key)
            if budget[1] < 0: return _STREAM
            normalized = _small_plain(item, budget, depth+1)
            if normalized is _STREAM: return _STREAM
            result[key] = normalized
        return result
    if kind in (tuple, list):
        if len(value) > budget[0]: return _STREAM
        result = []
        for item in value:
            normalized = _small_plain(item, budget, depth+1)
            if normalized is _STREAM: return _STREAM
            result.append(normalized)
        return result
    return _STREAM


class _CanonicalHash:
    def __init__(self):
        self.digest = hashlib.sha256()
        self.buffer = bytearray()
        self.active = set()

    def feed(self, value):
        if len(self.buffer)+len(value) > 65536:
            self.digest.update(self.buffer)
            self.buffer.clear()
        if len(value) >= 65536:
            self.digest.update(value)
        else:
            self.buffer.extend(value)

    def string(self, value):
        self.feed(b'"')
        # JSON escapes are code-point local; chunking does not change bytes.
        for offset in range(0, len(value), 16384):
            self.feed(json.encoder.encode_basestring(value[offset:offset+16384])[1:-1].encode('utf-8'))
        self.feed(b'"')

    def value(self, value):
        if type(value) in (dict, MappingProxyType, tuple, list):
            plain = _small_plain(value, [128, 8192])
            if plain is not _STREAM:
                self.feed(json.dumps(plain, ensure_ascii=False, sort_keys=True,
                                     separators=(',', ':')).encode('utf-8'))
                return
        if isinstance(value, str):
            self.string(value)
        elif value is None or isinstance(value, (bool, int, float)):
            self.feed(json.dumps(value, ensure_ascii=False, separators=(',', ':')).encode('utf-8'))
        elif isinstance(value, (Mapping, tuple, list)):
            identity = id(value)
            if identity in self.active:
                raise ValueError('Circular reference detected')
            self.active.add(identity)
            try:
                if isinstance(value, Mapping):
                    self.feed(b'{')
                    for ordinal, key in enumerate(sorted(value)):
                        if ordinal: self.feed(b',')
                        if isinstance(key, str): self.string(key)
                        elif key is None or isinstance(key, (bool, int, float)):
                            self.string(json.dumps(key, ensure_ascii=False))
                        else:
                            raise TypeError('JSON object keys must be str, int, float, bool or None')
                        self.feed(b':')
                        self.value(value[key])
                    self.feed(b'}')
                else:
                    self.feed(b'[')
                    for ordinal, item in enumerate(value):
                        if ordinal: self.feed(b',')
                        self.value(item)
                    self.feed(b']')
            finally:
                self.active.remove(identity)
        else:
            raise TypeError(f'Object of type {type(value).__name__} is not JSON serializable')

    def finish(self):
        self.digest.update(self.buffer)
        self.buffer.clear()
        return self.digest.hexdigest()


def snapshot_digest(episodes, postings):
    writer = _CanonicalHash()
    writer.feed(b'{"episodes":[')
    batch, batch_nodes, batch_chars = [], 0, 0
    wrote = False
    def flush():
        nonlocal wrote, batch_nodes, batch_chars
        if not batch: return
        if wrote: writer.feed(b',')
        writer.feed(json.dumps(batch, ensure_ascii=False, sort_keys=True,
                               separators=(',', ':'))[1:-1].encode('utf-8'))
        batch.clear()
        batch_nodes = batch_chars = 0
        wrote = True
    for key in sorted(episodes):
        episode = episodes[key]
        # Metadata dictionaries retain original observations by reference, not
        # _plain_json copies. At most one episode's small header is constructed.
        row = dict(episode_id=episode.episode_id, cues=episode.cues,
            steps=[dict(phase=step.phase, observation=step.observation,
                        relations=step.relations, judgment=step.judgment,
                        outcome=step.outcome, evidence_refs=step.evidence_refs)
                   for step in episode.steps],
            source_addresses=episode.source_addresses, revision=episode.revision,
            verification_state=episode.verification_state)
        budget = [128, 8192]
        plain = _small_episode(row, budget)
        if plain is _STREAM:
            flush()
            if wrote: writer.feed(b',')
            writer.value(row)
            wrote = True
        else:
            nodes, chars = 128-budget[0], 8192-budget[1]
            if batch_nodes+nodes > 1024 or batch_chars+chars > 32768:
                flush()
            batch.append(plain)
            batch_nodes += nodes
            batch_chars += chars
    flush()
    writer.feed(b'],"postings":')
    writer.value(postings)
    writer.feed(b'}')
    return writer.finish()
