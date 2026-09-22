"""Cold/delta explicit-proposition addresses; claims are not verified truths.

Only the existing observation['proposition_id'] vocabulary is indexed. A label,
shared word or missing observation never creates a contradiction. No natural
language inference or corpus enumeration is performed by the hot read view.
"""
from dataclasses import dataclass
from types import MappingProxyType


@dataclass(frozen=True, slots=True)
class PropositionDirectory:
    by_episode: object
    by_proposition: object

    @classmethod
    def from_rows(cls, rows):
        """Called while cold admission already traverses source observations."""
        by_episode, by_proposition = {}, {}
        for identifier, observations in rows:
            keys = tuple(dict.fromkeys(value for obs in observations
                if isinstance(value := obs.get('proposition_id'), str) and value))
            if not keys:
                continue
            if identifier in by_episode:
                raise ValueError('duplicate proposition source address')
            by_episode[identifier] = keys
            for key in keys:
                by_proposition.setdefault(key, []).append(identifier)
        return cls(MappingProxyType(by_episode), MappingProxyType(
            {key: tuple(values) for key, values in by_proposition.items()}))


def proposition_directories(source):
    """Expose only reviewed public MCP hot-memory layouts."""
    from .mosaic_memory_activation import MemoryActivationIndex, CompositeMemoryActivationIndex
    from .mosaic_compressed_memory import CompressedMemoryActivationIndex
    kind = type(source)
    if kind is MemoryActivationIndex:
        directory = getattr(source, 'proposition_directory', None)
        if type(directory) is not PropositionDirectory:
            raise ValueError('explicit proposition directory not prepared')
        yield directory
    elif kind is CompressedMemoryActivationIndex:
        yield from proposition_directories(source._index)
    elif kind is CompositeMemoryActivationIndex:
        for child in source.sources:
            yield from proposition_directories(child)
    else:
        raise ValueError('unsupported proposition directory source')
