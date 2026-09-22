"""Main-controller-owned navigation publication, separate from belief writes.

Cold preparation takes place outside the controller lock. Publication uses the
existing controller's generation lock; workers receive pinned views, not this
controller. The original owner, runtime, commit count and memory are untouched.
"""
from dataclasses import dataclass


@dataclass(frozen=True)
class RegionBinding:
    pair: object
    topology: object


@dataclass(frozen=True)
class RegionGeneration:
    pair: object
    runtime: object
    binding: RegionBinding | None

    @property
    def topology(self):
        # A new experience generation must not silently use the old binding.
        if self.binding is None or self.binding.pair is not self.pair:
            return None
        return self.binding.topology


def read_region_generation(controller):
    """Pin one generation. None topology means ordinary full-memory cognition."""
    with controller._lock:
        return RegionGeneration(controller._owner.snapshot(), controller._runtime,
                                getattr(controller, '_vrs_region_binding', None))


def publish_region_topology(controller, *, expected: RegionGeneration, topology):
    """Main-only CAS of a prepared derived topology; no CognitiveState commit.

The caller supplies a trusted, already-validated build/load result. This API is
not an untrusted worker proposal endpoint and is not a semantic authority gate.
"""
    if not topology.converged or expected.pair.memory.lookup_requires_io:
        raise ValueError('region publication requires converged hot preparation')
    topology.require_pair(expected.pair)
    replacement = RegionBinding(expected.pair, topology)
    with controller._lock:
        if (controller._owner.snapshot() is not expected.pair
                or controller._runtime is not expected.runtime
                or getattr(controller, '_vrs_region_binding', None) is not expected.binding):
            raise ValueError('main generation or navigation changed during preparation')
        controller._vrs_region_binding = replacement
        return RegionGeneration(expected.pair, expected.runtime, replacement)


def withdraw_region_topology(controller, *, expected_binding):
    """Drop only the caller's still-current derived binding, never new work."""
    if expected_binding is None:
        raise ValueError('an installed binding is required for withdrawal')
    with controller._lock:
        if getattr(controller, '_vrs_region_binding', None) is not expected_binding:
            raise ValueError('navigation changed before withdrawal')
        controller._vrs_region_binding = None
