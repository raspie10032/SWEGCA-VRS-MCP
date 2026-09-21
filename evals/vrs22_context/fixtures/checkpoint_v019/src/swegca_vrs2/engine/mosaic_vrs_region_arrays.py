from dataclasses import dataclass


@dataclass(frozen=True)
class RegionTermArrays:
    offsets: object
    nodes: object
