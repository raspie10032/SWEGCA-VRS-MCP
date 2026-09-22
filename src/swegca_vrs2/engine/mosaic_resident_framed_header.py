"""Cold physical node framing; no new evidence, pickle, or semantic selection."""
import hashlib
import io
import json
import struct

from .mosaic_resident_header_stream import header_events

MAGIC = b'RZLEAF02'
MAX_NODES = 4096
TARGET_BYTES = 1024 * 1024


def _metadata_pairs(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError('duplicate framed metadata field')
        result[key] = value
    return result


def framed_header_events(blob, size):
    """Same event stream as v1; at most one node batch is decoded at a time.

    A single large node is permitted without truncation. Every other batch is
    bounded in both bytes and nodes. The caller checks the external seal first.
    """
    position, stop = 16, 16+size
    metadata_seen = False
    while position < stop:
        if stop-position < 9:
            raise ValueError('truncated resident header frame')
        kind = blob[position:position+1]
        length = struct.unpack_from('>Q', blob, position+1)[0]
        position += 9
        if length > stop-position:
            raise ValueError('resident header frame exceeds extent')
        if kind not in (b'M', b'N', b'S'):
            raise ValueError('unknown resident header frame')
        if kind == b'M' and (metadata_seen or length > 65536):
            raise ValueError('duplicate or oversized resident metadata')
        if kind == b'N' and length > TARGET_BYTES:
            raise ValueError('resident node batch exceeds byte bound')
        payload = blob[position:position+length]
        position += length
        if kind == b'M':
            metadata = json.loads(payload, object_pairs_hook=_metadata_pairs)
            if type(metadata) is not dict or set(metadata) != {'schema', 'snapshot_id', 'root'}:
                raise ValueError('invalid framed resident metadata')
            metadata_seen = True
            yield from metadata.items()
        elif kind == b'N':
            rows = json.loads(payload)
            if type(rows) is not list or not 1 <= len(rows) <= MAX_NODES:
                raise ValueError('invalid resident node batch')
            for row in rows:
                yield 'nodes', row
            del rows
        elif kind == b'S':
            # An indivisible large node has its own frame, never a secretly
            # unbounded batch. The unchanged typed loader checks its layout.
            yield 'nodes', json.loads(payload)
    if not metadata_seen:
        raise ValueError('missing framed resident metadata')


def reframe_resident_leaf(blob, *, expected_sha256, expected_snapshot_id):
    """Re-encode only physical metadata; retain exact node order and payload.

    This is not semantic admission. Both v1 and v2 use the same typed loader.
    The digest must come from an independently checked generation manifest.
    The original archive is never mutated. No source bodies are decoded.
    """
    from .mosaic_resident_leaf_archive import MAGIC as LEGACY_MAGIC, _digest
    _digest(expected_sha256); _digest(expected_snapshot_id)
    if type(blob) is not bytes or hashlib.sha256(blob).hexdigest() != expected_sha256:
        raise ValueError('original resident archive seal differs')
    if len(blob) < 16 or blob[:8] != LEGACY_MAGIC:
        raise ValueError('reframing requires a v1 resident archive')
    size = struct.unpack_from('>Q', blob, 8)[0]
    if size > len(blob)-16:
        raise ValueError('truncated original resident header')
    output = io.BytesIO()
    output.write(MAGIC+b'\0'*8)
    metadata, batch, batch_bytes = {}, [], 2
    def frame(kind, payload):
        output.write(kind+struct.pack('>Q', len(payload)))
        output.write(payload)
    def flush():
        if batch:
            if len(batch) == 1 and len(batch[0])+2 > TARGET_BYTES:
                frame(b'S', batch[0])
            else:
                frame(b'N', b'['+b','.join(batch)+b']')
            batch.clear()
    for field, value in header_events(blob, size):
        if field != 'nodes':
            metadata[field] = value
            continue
        encoded = json.dumps(value, ensure_ascii=False, separators=(',', ':')).encode('utf-8')
        added = len(encoded)+(1 if batch else 0)
        if batch and (len(batch) == MAX_NODES or batch_bytes+added > TARGET_BYTES):
            flush(); batch_bytes = 2; added = len(encoded)
        batch.append(encoded)
        batch_bytes += added
    flush()
    if metadata['schema'] != 1 or metadata['snapshot_id'] != expected_snapshot_id:
        raise ValueError('original resident metadata identity differs')
    frame(b'M', json.dumps(metadata, ensure_ascii=False, separators=(',', ':')).encode('utf-8'))
    header_size = output.tell()-16
    output.write(memoryview(blob)[16+size:])
    output.seek(8)
    output.write(struct.pack('>Q', header_size))
    return output.getvalue()
