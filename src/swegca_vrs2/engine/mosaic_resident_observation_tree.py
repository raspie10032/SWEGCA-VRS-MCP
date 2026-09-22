"""Closed resident observation trees without an eagerly restored object graph.

Cold admission validates every node. Hot views decode requested fields from owned
immutable bytes; no JSON, disk, hash, model, mutable cache or cognitive authority.
Large compressed text/numeric blocks remain shared external immutable objects.
"""
from collections.abc import Mapping
from dataclasses import dataclass, field
import io
import struct
from types import MappingProxyType

from .mosaic_compressed_memory import CompressedObservation, _CompressedText
from .mosaic_lossless_float_tuple import PackedFloatTuple

_FRAME = struct.Struct('>cQ')
_LENGTH = struct.Struct('>I')


def _frame(blob, start, stop):
    if stop-start < _FRAME.size:
        raise ValueError('truncated resident observation node')
    tag, size = _FRAME.unpack_from(blob, start)
    body = start+_FRAME.size
    if size > stop-body:
        raise ValueError('resident observation node exceeds extent')
    return tag, body, body+size


def _members(blob, body, stop):
    while body < stop:
        if stop-body < _LENGTH.size:
            raise ValueError('truncated resident observation key')
        size = _LENGTH.unpack_from(blob, body)[0]
        body += _LENGTH.size
        if size > stop-body:
            raise ValueError('resident observation key exceeds extent')
        key = blob[body:body+size].decode('utf-8')
        start = body+size
        _, _, body = _frame(blob, start, stop)
        yield key, start, body


def _validate(blob, start, stop, externals):
    tag, body, end = _frame(blob, start, stop)
    size = end-body
    if tag == b'N':
        if size: raise ValueError('invalid null node')
    elif tag == b'B':
        if blob[body:end] not in (b'\0', b'\1'): raise ValueError('invalid bool node')
    elif tag == b'I':
        raw = blob[body:end]
        if str(int(raw)).encode('ascii') != raw: raise ValueError('invalid integer node')
    elif tag == b'F':
        if size != 8: raise ValueError('invalid float node')
    elif tag == b'S':
        blob[body:end].decode('utf-8')
    elif tag == b'X':
        if size != 4 or _LENGTH.unpack_from(blob, body)[0] >= len(externals):
            raise ValueError('invalid external observation reference')
    elif tag == b'T':
        while body < end:
            body = _validate(blob, body, end, externals)
    elif tag == b'M':
        keys = set()
        for key, child, child_end in _members(blob, body, end):
            if key in keys: raise ValueError('duplicate resident observation key')
            keys.add(key)
            _validate(blob, child, child_end, externals)
    else:
        raise ValueError('unknown resident observation node')
    return end


def _value(blob, start, stop, externals):
    tag, body, end = _frame(blob, start, stop)
    if tag == b'M':
        return PackedResidentObservation._view(blob, externals, start, end)
    if tag == b'T':
        values = []
        while body < end:
            values.append(_value(blob, body, end, externals))
            _, _, body = _frame(blob, body, end)
        return tuple(values)
    if tag == b'N': return None
    if tag == b'B': return blob[body] == 1
    if tag == b'I': return int(blob[body:end])
    if tag == b'F': return struct.unpack_from('>d', blob, body)[0]
    if tag == b'S': return blob[body:end].decode('utf-8')
    if tag == b'X': return externals[_LENGTH.unpack_from(blob, body)[0]].value()
    raise ValueError('unknown admitted resident observation node')


@dataclass(frozen=True, slots=True, eq=False)
class PackedResidentObservation(Mapping):
    """A field view; the original admitted bytes own every nested view's data."""

    _blob: bytes
    _externals: tuple
    _start: int = field(init=False, default=0)
    _stop: int = field(init=False)
    _fields_by_key: Mapping | None = field(init=False, default=None, repr=False)

    def __post_init__(self):
        if type(self._blob) is not bytes or type(self._externals) is not tuple:
            raise TypeError('immutable resident observation storage required')
        if any(type(item) not in (_CompressedText, PackedFloatTuple) for item in self._externals):
            raise TypeError('invalid resident observation external type')
        stop = len(self._blob)
        if _frame(self._blob, 0, stop)[0] != b'M':
            raise ValueError('resident observation root must be a mapping')
        if _validate(self._blob, 0, stop, self._externals) != stop:
            raise ValueError('trailing resident observation bytes')
        object.__setattr__(self, '_stop', stop)

    @classmethod
    def _view(cls, blob, externals, start, stop, *, indexed=True):
        # Only an admitted directory root or _value on an admitted parent calls
        # this internal helper; indexed views are detached per-consumer state.
        result = object.__new__(cls)
        for name, value in (('_blob', blob), ('_externals', externals),
                            ('_start', start), ('_stop', stop)):
            object.__setattr__(result, name, value)
        offsets = ({key:(child,end) for key,child,end in _members(blob,start+_FRAME.size,stop)}
                   if indexed else None)
        object.__setattr__(result,'_fields_by_key',MappingProxyType(offsets) if indexed else None)
        return result

    def request_view(self):
        """Detached field directory for one consumer; never mutate the owner."""
        return self._view(self._blob,self._externals,self._start,self._stop)

    def _fields(self):
        if self._fields_by_key is not None:
            return ((key,start,end) for key,(start,end) in self._fields_by_key.items())
        return _members(self._blob, self._start+_FRAME.size, self._stop)

    def __iter__(self):
        return (key for key, _, _ in self._fields())

    def __len__(self):
        if self._fields_by_key is not None: return len(self._fields_by_key)
        return sum(1 for _ in self._fields())

    def __getitem__(self, key):
        if self._fields_by_key is not None:
            start,stop = self._fields_by_key[key]
            return _value(self._blob,start,stop,self._externals)
        for candidate, start, stop in self._fields():
            if candidate == key:
                return _value(self._blob, start, stop, self._externals)
        raise KeyError(key)


def pack_observation(observation):
    """Offline physical conversion. No observation decoding or semantic hashing."""
    if type(observation) is PackedResidentObservation:
        if observation._start == 0 and observation._stop == len(observation._blob):
            return observation
        return PackedResidentObservation(observation._blob[observation._start:observation._stop],
                                         observation._externals)
    if not isinstance(observation, Mapping):
        raise TypeError('observation mapping required')
    output, externals, external_ids = io.BytesIO(), [], {}

    def write(value):
        start = output.tell()
        output.write(b'\0'*_FRAME.size)
        if value is None: tag = b'N'
        elif type(value) is bool:
            tag = b'B'; output.write(bytes((value,)))
        elif type(value) is int:
            tag = b'I'; output.write(str(value).encode('ascii'))
        elif type(value) is float:
            tag = b'F'; output.write(struct.pack('>d', value))
        elif type(value) is str:
            tag = b'S'; output.write(value.encode('utf-8'))
        elif type(value) is tuple:
            tag = b'T'
            for item in value: write(item)
        elif type(value) in (_CompressedText, PackedFloatTuple):
            tag = b'X'
            key = external_ids.get(id(value))
            if key is None:
                key = len(externals); external_ids[id(value)] = key; externals.append(value)
            output.write(_LENGTH.pack(key))
        elif isinstance(value, Mapping):
            tag = b'M'
            nodes = value._nodes if type(value) is CompressedObservation else value
            for key, item in nodes.items():
                if type(key) is not str: raise TypeError('string observation key required')
                encoded = key.encode('utf-8')
                output.write(_LENGTH.pack(len(encoded))); output.write(encoded)
                write(item)
        else:
            raise TypeError('unsupported resident observation type')
        stop = output.tell()
        output.seek(start); output.write(_FRAME.pack(tag, stop-start-_FRAME.size)); output.seek(stop)

    write(observation)
    return PackedResidentObservation(output.getvalue(), tuple(externals))
