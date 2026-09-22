"""Lossless immutable resident bytes; no memory selection or cognitive authority.

Build/verify are cold or new-wave operations. Reads decode only intersecting blocks
in RAM, without JSON, filesystem, network or cryptographic hashing. This codec is
not a HotMemoryIndex adapter, persistence format, or live ownership transition.
"""

from __future__ import annotations

import hashlib
import sys
import zlib
from dataclasses import dataclass
from types import MappingProxyType
from typing import Iterable, Mapping

try:
    from compression import zstd
except ImportError:  # Keep the repository's Python >=3.11 baseline usable.
    zstd = None

SCHEMA = "rozephine-lossless-resident-blocks-v1"
MAX_BLOCK_BYTES = 4 * 1024 * 1024
DEFAULT_BLOCK_BYTES = 64 * 1024


class CorruptBlock(ValueError):
    """Invalid layout, length, frame or checksum; never return partial evidence."""


@dataclass(frozen=True, slots=True)
class ReadReceipt:
    data: bytes
    block_indices: tuple[int, ...]
    decoded_bytes: int


@dataclass(frozen=True, slots=True)
class Block:
    codec: str
    payload: bytes
    raw_size: int
    crc32: int

    def __post_init__(self) -> None:
        if self.codec not in ("raw", "zlib", "zstd"):
            raise ValueError("unsupported block codec")
        if type(self.payload) is not bytes:
            raise TypeError("block payload must be immutable bytes")
        if type(self.raw_size) is not int or not 0 < self.raw_size <= MAX_BLOCK_BYTES:
            raise ValueError("block raw_size outside bounded range")
        if not 0 <= self.crc32 <= 0xFFFFFFFF:
            raise ValueError("invalid block checksum")
        if len(self.payload) > self.raw_size:
            raise ValueError("expanded frames must use raw fallback")
        if self.codec == "raw" and len(self.payload) != self.raw_size:
            raise ValueError("raw block length mismatch")

    def decode(self) -> bytes:
        # CRC is an accidental-corruption check, not semantic authority or a
        # cryptographic hash. Strong integrity is checked at build/import, not
        # repeatedly on the live judgment path.
        try:
            if self.codec == "raw":
                raw = self.payload
            elif self.codec == "zlib":
                decoder = zlib.decompressobj()
                raw = decoder.decompress(self.payload, self.raw_size + 1)
                if not decoder.eof or decoder.unused_data or decoder.unconsumed_tail:
                    raise CorruptBlock("incomplete, trailing or oversized zlib frame")
            else:
                if zstd is None:
                    raise RuntimeError("zstd block requires Python compression.zstd")
                decoder = zstd.ZstdDecompressor(
                    options={zstd.DecompressionParameter.window_log_max: 23}
                )
                raw = decoder.decompress(self.payload, max_length=self.raw_size + 1)
                if not decoder.eof or decoder.unused_data:
                    raise CorruptBlock("incomplete, trailing or oversized zstd frame")
        except (zlib.error, EOFError) as exc:
            raise CorruptBlock("invalid compressed frame") from exc
        except Exception as exc:
            if zstd is not None and isinstance(exc, zstd.ZstdError):
                raise CorruptBlock("invalid compressed frame") from exc
            raise
        if len(raw) != self.raw_size or zlib.crc32(raw) != self.crc32:
            raise CorruptBlock("block length or checksum mismatch")
        return raw


@dataclass(frozen=True, slots=True)
class LosslessBlob:
    """Immutable physical layout; content digest is independent of codec/chunks."""

    blocks: tuple[Block, ...]
    block_bytes: int
    raw_size: int
    content_sha256: str

    def __post_init__(self) -> None:
        if type(self.blocks) is not tuple or any(type(b) is not Block for b in self.blocks):
            raise TypeError("blocks must be an immutable tuple of Block")
        if type(self.block_bytes) is not int or not 1 <= self.block_bytes <= MAX_BLOCK_BYTES:
            raise ValueError("block_bytes outside bounded range")
        if type(self.raw_size) is not int or self.raw_size < 0:
            raise ValueError("invalid raw_size")
        expected = (self.raw_size + self.block_bytes - 1) // self.block_bytes
        if len(self.blocks) != expected:
            raise ValueError("block count mismatch")
        for i, block in enumerate(self.blocks):
            if block.raw_size != min(self.block_bytes, self.raw_size - i * self.block_bytes):
                raise ValueError("block layout mismatch")
        if (len(self.content_sha256) != 64 or
                any(c not in "0123456789abcdef" for c in self.content_sha256)):
            raise ValueError("invalid content digest")

    @classmethod
    def build(cls, raw: bytes, *, codec: str = "zlib", level: int = 3,
              block_bytes: int = DEFAULT_BLOCK_BYTES) -> LosslessBlob:
        if type(raw) is not bytes:
            raise TypeError("input must be detached immutable bytes")
        if type(block_bytes) is not int or not 1 <= block_bytes <= MAX_BLOCK_BYTES:
            raise ValueError("block_bytes outside bounded range")
        if codec not in ("raw", "zlib", "zstd"):
            raise ValueError("unsupported codec")
        if codec == "zstd" and zstd is None:
            raise RuntimeError("zstd requires Python compression.zstd; use zlib otherwise")
        if type(level) is not int or not 1 <= level <= 9:
            raise ValueError("diagnostic compression level must be 1..9")
        blocks = []
        for start in range(0, len(raw), block_bytes):
            chunk = raw[start:start + block_bytes]
            if codec == "zlib":
                encoded = zlib.compress(chunk, level)
            elif codec == "zstd":
                encoded = zstd.compress(chunk, options={
                    zstd.CompressionParameter.compression_level: level,
                    zstd.CompressionParameter.checksum_flag: 1,
                })
            else:
                encoded = chunk
            use_compressed = len(encoded) < len(chunk)
            block = Block(codec if use_compressed else "raw",
                          encoded if use_compressed else chunk,
                          len(chunk), zlib.crc32(chunk))
            if block.decode() != chunk:
                raise CorruptBlock("build roundtrip mismatch")
            blocks.append(block)
        return cls(tuple(blocks), block_bytes, len(raw), hashlib.sha256(raw).hexdigest())

    def read(self, start: int = 0, stop: int | None = None) -> ReadReceipt:
        """Strict half-open byte range; no full-blob scan, cache or hidden I/O."""
        if stop is None:
            stop = self.raw_size
        if type(start) is not int or type(stop) is not int:
            raise TypeError("byte offsets must be integers")
        if not 0 <= start <= stop <= self.raw_size:
            raise ValueError("byte range outside blob")
        if start == stop:
            return ReadReceipt(b"", (), 0)
        indices = tuple(range(start // self.block_bytes, (stop - 1) // self.block_bytes + 1))
        pieces = []
        decoded_bytes = 0
        for index in indices:
            raw = self.blocks[index].decode()
            decoded_bytes += len(raw)
            base = index * self.block_bytes
            pieces.append(raw[max(0, start - base):min(len(raw), stop - base)])
        return ReadReceipt(b"".join(pieces), indices, decoded_bytes)

    def verify_cold(self) -> None:
        """Explicit bootstrap/import validation. Never invoke per judgment."""
        digest = hashlib.sha256()
        for block in self.blocks:
            digest.update(block.decode())
        if digest.hexdigest() != self.content_sha256:
            raise CorruptBlock("content digest mismatch")

    @property
    def stored_payload_bytes(self) -> int:
        return sum(len(block.payload) for block in self.blocks)

    def resident_size_estimate(self) -> int:
        """Python object graph estimate; excludes allocator/native workspace/RSS."""
        size = sys.getsizeof(self) + sys.getsizeof(self.blocks)
        size += sys.getsizeof(self.content_sha256)
        size += sys.getsizeof(self.raw_size) + sys.getsizeof(self.block_bytes)
        for block in self.blocks:
            size += sum(sys.getsizeof(item) for item in
                        (block, block.codec, block.payload, block.raw_size, block.crc32))
        return size


@dataclass(frozen=True, slots=True)
class ResidentRecordStore:
    """Physical address table, NOT semantic index or main persistent state.

    Duplicate byte payloads share one immutable blob, but addresses stay distinct.
    No parent generation support is claimed here; that belongs to the hot-index
    integration. Build once at bootstrap or for only the incoming wave.
    """

    records: Mapping[str, LosslessBlob]
    _mapping_size: int = 0

    def __post_init__(self) -> None:
        detached = dict(self.records)
        if any(type(k) is not str or not k or type(v) is not LosslessBlob
               for k, v in detached.items()):
            raise ValueError("invalid record address or blob")
        object.__setattr__(self, "records", MappingProxyType(detached))
        object.__setattr__(self, "_mapping_size", sys.getsizeof(detached))

    @classmethod
    def build(cls, records: Iterable[tuple[str, bytes]], **options) -> ResidentRecordStore:
        table = {}
        shared = {}
        for address, raw in records:
            if type(address) is not str or not address:
                raise ValueError("record address must be a nonempty string")
            if address in table:
                raise ValueError("duplicate address; do not silently overwrite experience")
            if type(raw) is not bytes:
                raise TypeError("record content must be immutable bytes")
            digest = hashlib.sha256(raw).digest()
            blob = shared.get(digest)
            if blob is None:
                blob = LosslessBlob.build(raw, **options)
                shared[digest] = blob
            elif blob.read().data != raw:
                # Exact bytes, not hash equality alone, authorize physical sharing.
                raise CorruptBlock("content hash collision")
            table[address] = blob
        return cls(table)

    def read(self, address: str, start: int = 0, stop: int | None = None) -> ReadReceipt:
        return self.records[address].read(start, stop)

    def stats(self) -> dict[str, int]:
        # Diagnostic only: explicit traversal, never called on hot read.
        unique = {id(blob): blob for blob in self.records.values()}
        address_bytes = self._mapping_size + sys.getsizeof(self.records)
        address_bytes += sum(sys.getsizeof(key) for key in self.records)
        return {
            "address_count": len(self.records),
            "unique_blob_count": len(unique),
            "logical_raw_bytes": sum(blob.raw_size for blob in self.records.values()),
            "unique_raw_bytes": sum(blob.raw_size for blob in unique.values()),
            "stored_payload_bytes": sum(blob.stored_payload_bytes for blob in unique.values()),
            "address_table_estimate_bytes": address_bytes,
            "resident_estimate_bytes": sys.getsizeof(self) + address_bytes +
                sum(blob.resident_size_estimate() for blob in unique.values()),
        }
