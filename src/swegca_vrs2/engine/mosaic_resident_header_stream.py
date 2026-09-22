"""Cold-only streaming of a sealed resident archive's JSON node table.

Keep one decoded window plus one node, not the whole parsed JSON object graph.
The caller must check the external archive seal BEFORE entering this parser.
No files, persistent cache, model, or semantic authority are involved.
"""
import codecs
import json


class _Reader:
    def __init__(self, blob, start, end, chunk_bytes):
        self.blob, self.position, self.end = blob, start, end
        self.chunk_bytes = chunk_bytes
        self.decoder = codecs.getincrementaldecoder('utf-8')()
        self.json = json.JSONDecoder()
        self.text, self.cursor = '', 0
        self.finished = False

    def more(self, count=None):
        if self.finished:
            return False
        end = min(self.end, self.position + (count or self.chunk_bytes))
        self.text = self.text[self.cursor:] + self.decoder.decode(
            self.blob[self.position:end], final=end == self.end)
        self.cursor, self.position = 0, end
        self.finished = end == self.end
        return True

    def peek(self):
        while True:
            while self.cursor < len(self.text) and self.text[self.cursor] in ' \t\r\n':
                self.cursor += 1
            if self.cursor < len(self.text):
                return self.text[self.cursor]
            if not self.more():
                return ''

    def take(self, expected):
        if self.peek() != expected:
            raise ValueError('invalid resident header JSON delimiter')
        self.cursor += 1

    def value(self):
        if not self.peek():
            raise ValueError('truncated resident header JSON value')
        growth = self.chunk_bytes
        while True:
            try:
                value, end = self.json.raw_decode(self.text, self.cursor)
            except json.JSONDecodeError:
                if not self.more(growth):
                    raise
                growth *= 2
                continue
            # A number can parse successfully before its final digit arrives.
            # Read through its following delimiter before accepting the value.
            if (end == len(self.text) or self.text[end] not in ' \t\r\n,]}:') and not self.finished:
                self.more(growth)
                growth *= 2
                continue
            self.cursor = end
            return value


def header_events(blob, size, *, chunk_bytes=65536):
    """Yield (field, value), with each nodes[] row emitted as ('nodes', row).

    Top-level field order remains arbitrary, as with the previous JSON loader.
    Exactly one nodes array is required. Duplicate top-level keys fail closed.
    Memory is bounded by the current node size, not the entire node table.
    """
    if type(chunk_bytes) is not int or chunk_bytes < 1:
        raise ValueError('positive header decode window required')
    reader = _Reader(blob, 16, 16+size, chunk_bytes)
    reader.take('{')
    seen = set()
    if reader.peek() != '}':
        while True:
            key = reader.value()
            if type(key) is not str or key in seen or key not in ('schema', 'snapshot_id', 'root', 'nodes'):
                raise ValueError('invalid resident header fields')
            seen.add(key)
            reader.take(':')
            if key == 'nodes':
                reader.take('[')
                if reader.peek() != ']':
                    while True:
                        # The exporter emits adjacent JSON rows. Decode a full
                        # row and consume its already-visible delimiter once,
                        # instead of value/peek/take peeking at it three times.
                        # A row crossing a UTF-8/window boundary, whitespace,
                        # or any ambiguous token still uses the general reader.
                        text, cursor = reader.text, reader.cursor
                        try:
                            row, end = reader.json.raw_decode(text, cursor)
                        except json.JSONDecodeError:
                            pass
                        else:
                            if end < len(text) and text[end] in ',]':
                                delimiter = text[end]
                                reader.cursor = end + 1
                                yield key, row
                                if delimiter == ']':
                                    break
                                continue
                        yield key, reader.value()
                        if reader.peek() == ']':
                            reader.cursor += 1
                            break
                        reader.take(',')
                else:
                    reader.cursor += 1
            else:
                yield key, reader.value()
            if reader.peek() == '}':
                break
            reader.take(',')
    reader.take('}')
    if reader.peek() or seen != {'schema', 'snapshot_id', 'root', 'nodes'}:
        raise ValueError('resident header trailing data or missing fields')
