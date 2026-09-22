"""Exact IEEE restoration of immutable numeric tuples, not quantization."""
from dataclasses import dataclass
import struct

from .mosaic_lossless_blocks import LosslessBlob


@dataclass(frozen=True, slots=True)
class PackedFloatTuple:
    blob: LosslessBlob
    count: int
    format: str

    def __post_init__(self):
        if (type(self.count) is not int or self.count < 1 or self.format not in ('e','f','d')
            or self.blob.raw_size != self.count * struct.calcsize(self.format)):
            raise ValueError('invalid exact float tuple layout')

    @classmethod
    def build(cls, values, policy):
        if type(values) is not tuple or not values or any(type(v) is not float for v in values):
            raise TypeError('immutable homogeneous float tuple required')
        count=len(values); original=struct.pack(f'<{count}d',*values)
        chosen='d'; packed=original
        for candidate in ('e','f'):
            try:
                payload=struct.pack(f'<{count}{candidate}',*values)
                restored=struct.unpack(f'<{count}{candidate}',payload)
                if struct.pack(f'<{count}d',*restored)==original:
                    chosen=candidate;packed=payload;break
            except (OverflowError,struct.error):
                pass
        blob=LosslessBlob.build(packed,codec=policy.codec,level=policy.level,block_bytes=policy.block_bytes)
        result=cls(blob,count,chosen)
        if struct.pack(f'<{count}d',*result.value()) != original:
            raise ValueError('numeric bit-exact restoration failed')
        return result

    def value(self):
        return struct.unpack(f'<{self.count}{self.format}',self.blob.read().data)
