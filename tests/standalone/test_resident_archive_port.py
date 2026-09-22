"""Author resident archive retains exact original and explicit VRS addresses."""
import hashlib

from swegca_vrs2.engine.mosaic_compressed_memory import (
    CompressionPolicy, compress_hot_memory_index, resident_python_bytes)
from swegca_vrs2.engine.mosaic_memory_activation import (
    MemoryActivationIndex, MemoryEpisode, MemoryStep)
from swegca_vrs2.engine.mosaic_proposition_directory import proposition_directories
from swegca_vrs2.engine.mosaic_resident_leaf_archive import (
    dump_resident_leaf, load_resident_leaf)


def test_resident_directory_archive_roundtrips_original_and_proposition():
    step = MemoryStep('observation',
        {'text': 'exact original', 'proposition_id': 'p'}, (), 'record',
        'pending', ('source:one',))
    original = MemoryEpisode('episode:one', ('anchor',), (step,),
        ('source:one',), '1', 'unverified')
    index = MemoryActivationIndex('', {'episode:one': original},
                                  {'anchor': ('episode:one',)})
    compressed = compress_hot_memory_index(index, CompressionPolicy())
    blob = dump_resident_leaf(compressed, resident_directory=True)
    restored = load_resident_leaf(blob,
        expected_sha256=hashlib.sha256(blob).hexdigest(),
        expected_snapshot_id=index.snapshot_id, maximum_bytes=len(blob))
    assert restored.snapshot_id == index.snapshot_id
    assert restored.episode('episode:one') == original
    assert tuple(proposition_directories(restored))[0].by_proposition['p'] == ('episode:one',)
    assert resident_python_bytes(restored) > 0
