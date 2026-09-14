"""Detached array metadata, shared immutable payload; no validation authority."""
import numpy as np


def immutable_numeric_array(value: np.ndarray) -> np.ndarray:
    """Freeze mutable buffers once, reuse only a proven immutable bytes owner.

    A readonly flag (including a readonly memoryview of a bytearray or mmap) is
    not proof of immutability. Return separate ndarray metadata even on reuse:
    changing the caller's shape/dtype must not change the returned view.
    Content/schema validation remains the responsibility of the caller.
    """
    if not isinstance(value, np.ndarray) or value.dtype.hasobject:
        raise ValueError('plain non-object array required')
    owner = value
    if type(value) is np.ndarray and value.flags.c_contiguous:
        while type(owner) is np.ndarray:
            owner = owner.base
        if type(owner) is bytes:
            return value.view()
    return np.frombuffer(value.tobytes(), dtype=value.dtype).reshape(value.shape)
