"""The ported author directory keeps only explicit source proposition IDs."""
from swegca_vrs2.engine.mosaic_memory_activation import (
    CompositeMemoryActivationIndex, MemoryActivationIndex, MemoryEpisode, MemoryStep)
from swegca_vrs2.engine.mosaic_proposition_directory import proposition_directories


def episode(identifier, observation):
    return MemoryEpisode(identifier, ('anchor',), (
        MemoryStep('observation', observation, (), 'record', 'pending',
                   ('source:' + identifier,)),),
        ('source:' + identifier,), '1', 'unverified')


def test_explicit_proposition_directory_survives_hot_and_composite_views():
    first = episode('first', {'proposition_id': 'p', 'text': 'anchor'})
    second = episode('second', {'text': 'p appears only as a word'})
    third = episode('third', {'proposition_id': 'p', 'text': 'anchor'})
    base = MemoryActivationIndex('', {'first': first, 'second': second},
                                 {'anchor': ('first', 'second')})
    added = MemoryActivationIndex('', {'third': third}, {'anchor': ('third',)})
    directories = tuple(proposition_directories(CompositeMemoryActivationIndex((base, added))))
    assert len(directories) == 2
    assert directories[0].by_proposition['p'] == ('first',)
    assert directories[1].by_proposition['p'] == ('third',)
    assert 'second' not in directories[0].by_episode
