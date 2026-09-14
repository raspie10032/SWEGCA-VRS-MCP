"""Standalone main's explicitly recorded source-family directory.

This adapter replaces private physical-layout dispatch, not family semantics.
Ordinary text records have no derived semantic family by default.
"""


def semantic_family_directories(source):
    yield from source.semantic_families


def family_keys(directories, identifier):
    keys = {}
    for directory in directories:
        if identifier in directory.by_parent:
            keys[identifier] = None
        for parent in directory.by_child.get(identifier, ()):
            keys[parent] = None
    return keys


def family_spans(directories, parent):
    yield (parent,)
    for directory in directories:
        rows = directory.by_parent.get(parent, ())
        if rows:
            yield rows
