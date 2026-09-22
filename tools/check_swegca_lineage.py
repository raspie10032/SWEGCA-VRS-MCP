#!/usr/bin/env python3
"""Static source-lineage gate for staged C++ product files.

This does not establish algorithmic parity. An independent source review still
checks every tag against the author function and accepted user directive.
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys


PRODUCT_PREFIXES = ("native/", "include/", "cpp/")
AUTHOR_NAMESPACES = ("src/swegca_vrs2/engine/mosaic_", "src/tinylm_slicer/mosaic_")
CPP_SUFFIXES = (".cpp", ".cc", ".cxx", ".hpp", ".h")
TAG = re.compile(r"^\s*//\s*SWEGCA:\s+\S+@\S+:\d+(?:-\d+)?\s*$", re.MULTILINE)
FORBIDDEN = re.compile(r"\b(?:bm25|idf|embedding|cosine|hnsw|pagerank|torch|llm|http)\b", re.I)
CONTROL = {"if", "for", "while", "switch", "catch", "sizeof", "alignof", "requires"}


def git_bytes(*args: str) -> bytes:
    return subprocess.check_output(("git", *args))


def staged_paths() -> list[str]:
    raw = git_bytes("diff", "--cached", "--name-only", "--diff-filter=ACMR", "-z")
    return [item.decode("utf-8", "surrogateescape") for item in raw.split(b"\0") if item]


def without_comments_and_strings(source: str) -> str:
    """Preserve line positions while hiding tokens that are not executable C++."""
    output: list[str] = []
    index = 0
    state = "code"
    while index < len(source):
        char = source[index]
        next_char = source[index + 1] if index + 1 < len(source) else ""
        if state == "code":
            if char == "/" and next_char == "/":
                output.extend("  ")
                index += 2
                state = "line_comment"
                continue
            if char == "/" and next_char == "*":
                output.extend("  ")
                index += 2
                state = "block_comment"
                continue
            if char in ('"', "'"):
                output.append(" ")
                state = "double_string" if char == '"' else "single_string"
                index += 1
                continue
            output.append(char)
        elif state == "line_comment":
            output.append("\n" if char == "\n" else " ")
            if char == "\n":
                state = "code"
        elif state == "block_comment":
            output.append("\n" if char == "\n" else " ")
            if char == "*" and next_char == "/":
                output.append(" ")
                index += 1
                state = "code"
        else:
            output.append("\n" if char == "\n" else " ")
            if char == "\\" and next_char:
                output.append("\n" if next_char == "\n" else " ")
                index += 1
            elif (state == "double_string" and char == '"') or (
                state == "single_string" and char == "'"
            ):
                state = "code"
        index += 1
    return "".join(output)


def definition_positions(code: str) -> list[int]:
    """Conservative C++ function opener scan; source review covers edge cases."""
    positions: list[int] = []
    for match in re.finditer(r"\{", code):
        at = match.start()
        prefix = code[max(0, at - 500):at]
        # Walk back from the last closing parenthesis to its matching opener.
        # An if (predicate()) block must resolve to `if`, not `predicate`.
        close = prefix.rfind(")")
        if close < 0:
            continue
        depth = 0
        opening = -1
        for index in range(close, -1, -1):
            if prefix[index] == ")":
                depth += 1
            elif prefix[index] == "(":
                depth -= 1
                if depth == 0:
                    opening = index
                    break
        if opening < 0:
            continue
        before = prefix[:opening].rstrip()
        token = re.search(r"([\w:~]+)$", before)
        if not token:
            continue
        name = token.group(1).split("::")[-1]
        if name in CONTROL or before.endswith("]") or before.endswith("if constexpr"):
            continue
        tail = prefix[close + 1:]
        if ";" in tail or "{" in tail or "}" in tail:
            continue
        # Ignore macro declarations and class/namespace openers.
        if re.search(r"\b(?:class|struct|namespace|enum)\s+[^{};]*$", prefix):
            continue
        positions.append(at)
    return positions


def check_cpp(path: str, source: str) -> list[str]:
    issues: list[str] = []
    tags = list(TAG.finditer(source))
    if not tags:
        return [f"{path}: missing // SWEGCA: source@revision:lines tag"]
    clean = without_comments_and_strings(source)
    if forbidden := FORBIDDEN.search(clean):
        issues.append(f"{path}: external memory vocabulary {forbidden.group(0)!r}")
    used = -1
    for position in definition_positions(clean):
        line = source.count("\n", 0, position) + 1
        eligible = [i for i, tag in enumerate(tags) if tag.end() < position and i > used]
        if not eligible:
            issues.append(f"{path}:{line}: function definition has no unused SWEGCA source tag")
            continue
        tag_index = eligible[-1]
        tag_line = source.count("\n", 0, tags[tag_index].start()) + 1
        if line - tag_line > 10:
            issues.append(f"{path}:{line}: source tag is more than 10 lines above definition")
            continue
        used = tag_index
    return issues


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--staged", action="store_true", required=True)
    parser.parse_args()
    issues: list[str] = []
    for path in staged_paths():
        if path.startswith(AUTHOR_NAMESPACES):
            issues.append(f"{path}: author Python namespace is protected from edited copies")
            continue
        if not path.startswith(PRODUCT_PREFIXES) or not path.endswith(CPP_SUFFIXES):
            continue
        source = git_bytes("show", f":{path}").decode("utf-8", "strict")
        issues.extend(check_cpp(path, source))
    if issues:
        for issue in issues:
            print(issue, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
