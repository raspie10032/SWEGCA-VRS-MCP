"""Reproduce the bounded first-party VRS2 port; never copy private data.

The source tree is an explicit argument. Selected definitions retain exact source
text except package-relative imports. The manifest records source and port hashes.
"""
import argparse
import ast
import hashlib
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
WHOLE = ('mosaic_vrs_address_index', 'mosaic_vrs_dependency_index',
         'mosaic_immutable_numeric', 'mosaic_vrs_event_kernel',
         'mosaic_vrs_event_signal', 'mosaic_vrs_region_arrays',
         'mosaic_vrs_connectivity_regions', 'mosaic_hot_evidence_pages')
SELECT = {
    'mosaic_memory_activation': (
        '_text _cue _freeze_json _plain_json MemoryStep MemoryEpisode HotMemoryIndex '
        'FullCurrentMemoryVrsSnapshot AtomicFullCurrentMemoryVrsOwner DejaVuSignal '
        'RuntimeCueSelection select_runtime_cues detect_deja_vu RecallCandidate RecallResult '
        'recall_memory ReplayedEpisode ReplayResult replay_memory CurrentEvidenceVerdict '
        'current_experience_verdict ReEvidenceResult re_evidence_memory '
        'MemoryActivationReceipt activate_memory').split(),
    'mosaic_memory_promotion': ['VRSExperiencePromotionDecision', 'assess_vrs_experience_promotion'],
    'mosaic_vrs_state_update': ['VRSConnectionStateUpdate', 'VRSStateUpdateReceipt', 'plan_vrs_state_update'],
    'mosaic_vrs_event_delta': ['array_binding', '_put', 'SparseEventVector', 'SparseEventEdges', 'prepare_event_delta'],
}
HEADERS = {
    'mosaic_memory_activation': '''from __future__ import annotations
import hashlib
import json
import re
from dataclasses import dataclass, field
from threading import Lock
from types import MappingProxyType
from typing import Any, Callable, Iterable, Mapping, Protocol, runtime_checkable
OUTCOMES = frozenset({'success', 'failure', 'negative', 'uncertain', 'conflict', 'pending'})
VERDICTS = frozenset({'support', 'refute', 'insufficient', 'conflict', 'available', 'retained'})
_FULL_CURRENT_MEMORY_VRS_SCHEMA = 'rozephine-full-current-memory-vrs-snapshot-v1'
EvidenceJudge = Callable[['ReplayedEpisode'], 'CurrentEvidenceVerdict']
''',
    'mosaic_memory_promotion': '''from __future__ import annotations
import math
from dataclasses import dataclass
VERIFIED_EXPERIENCE_PROMOTION_STRENGTH = 1.0
''',
    'mosaic_vrs_state_update': '''from __future__ import annotations
import math
from dataclasses import dataclass, replace
from .mosaic_memory_activation import MemoryActivationReceipt
from .mosaic_memory_promotion import VRSExperiencePromotionDecision, assess_vrs_experience_promotion
VRS_STABLE_REINFORCEMENT_FACTOR = 1.01
VRS_UNSTABLE_WEAKENING_FACTOR = 0.995
''',
    'mosaic_vrs_event_delta': '''from dataclasses import dataclass
from types import MappingProxyType
import operator
import re
import numpy as np
from .mosaic_immutable_numeric import immutable_numeric_array
from .mosaic_vrs_dependency_index import EndpointDependencyIndex, _segment, _merge
''',
}


def extract(source):
    target = ROOT / 'src/swegca_vrs2/engine'
    target.mkdir(parents=True, exist_ok=True)
    (target / '__init__.py').write_text('"""First-party native VRS2 components. See NATIVE_VRS2_PORT.json."""\n', encoding='utf-8')
    records = []
    for module in (*WHOLE, *SELECT):
        path = source / (module + '.py')
        raw = path.read_bytes()
        original = raw.decode('utf-8')
        if module in SELECT:
            lines = original.splitlines(keepends=True)
            definitions = {n.name: n for n in ast.parse(original).body if isinstance(n, (ast.ClassDef, ast.FunctionDef))}
            chunks = []
            for name in SELECT[module]:
                node = definitions[name]
                first = min([node.lineno] + [d.lineno for d in node.decorator_list])
                chunks.append(''.join(lines[first-1:node.end_lineno]))
            text = HEADERS[module] + '\n\n' + '\n\n'.join(chunks)
        else:
            text = original
        text = text.replace('from tinylm_slicer.', 'from .')
        text = text.rstrip() + '\n'
        (target / path.name).write_text(text, encoding='utf-8')
        records.append(dict(module=module, source_sha256=hashlib.sha256(raw).hexdigest(),
                            port_sha256=hashlib.sha256(text.encode()).hexdigest(),
                            definitions=SELECT.get(module, 'whole_module')))
    manifest = dict(schema='native-vrs2-first-party-port-v1', source_base_revision='3bddcb7adc8c07e21a57d8c921d312aed83270e5',
                    source_state='2026-09-14 working source; per-file hashes authoritative',
                    license='MIT', records=records,
                    scope='Memory activation, event-local VRS2 numeric settlement, overlapping connectivity regions, exact evidence pages. No model, Hermes or private experiences.',
                    modifications='Selected definitions with explicit dependency headers; absolute imports made relative. Standalone persistent main and ingestion are separate composition code.')
    (ROOT / 'NATIVE_VRS2_PORT.json').write_text(json.dumps(manifest, indent=2)+'\n', encoding='utf-8')


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('source', type=Path)
    extract(parser.parse_args().source)
