"""The larger affected-node backend must preserve native f32 settlement."""
import numpy as np
import pytest

from swegca_vrs2.numeric import settle
from swegca_vrs2.store import EDGE, frozen
from swegca_vrs2.engine.mosaic_vrs_dependency_index import EndpointDependencyIndex
from swegca_vrs2.engine.mosaic_vrs_event_kernel import EventVrsInputs
from swegca_vrs2.engine.mosaic_vrs_event_signal import settle_event_signal


@pytest.mark.parametrize('seed', range(20))
def test_large_affected_backend_matches_reference_bits(seed):
    random = np.random.default_rng(seed)
    count = 40
    edges = []
    for node in range(count - 1):
        edges.extend(((node, node + 1, 1, .5),
                      (node + 1, node, -1 if node % 3 == 0 else 1, .3)))
    for _ in range(35):
        source = int(random.integers(0, count))
        target = int(random.integers(0, count))
        edges.append((source, target, int(random.choice([-1, 1])), float(random.uniform(.1, .9))))
    edge = frozen(np.array(edges, dtype=EDGE))
    direct = frozen(random.uniform(-.6, .6, count).astype(np.float32))
    old = frozen(random.uniform(-.2, .2, count).astype(np.float32))
    inputs = EventVrsInputs('a' * 64, direct, old, edge,
        frozen(edge['vrs_strength'].copy()), frozen(np.ones(count, dtype=bool)),
        EndpointDependencyIndex.build(edge))
    expected = settle_event_signal(inputs, changed_nodes=(0, 19), maximum_rounds=512)
    actual = settle(inputs, changed_nodes=(0, 19), maximum_rounds=512)
    assert actual.pending_nodes == expected.pending_nodes
    assert actual.rounds == expected.rounds
    expected_values = np.array([expected.scores.get(i, old[i]) for i in range(count)], dtype=np.float32)
    actual_values = np.array([actual.scores.get(i, old[i]) for i in range(count)], dtype=np.float32)
    np.testing.assert_array_equal(actual_values.view(np.uint32), expected_values.view(np.uint32))
