#!/usr/bin/env python3
"""Static source-lineage gate for staged C++ product files.

This does not establish algorithmic parity. An independent source review still
checks every tag against the author function and accepted user directive.
"""

from __future__ import annotations

import argparse
from functools import lru_cache
import os
from pathlib import Path
import re
import subprocess
import sys


PRODUCT_PREFIXES = ("native/", "include/", "cpp/")
AUTHOR_NAMESPACES = ("src/swegca_vrs2/engine/mosaic_", "src/tinylm_slicer/mosaic_")
CPP_SUFFIXES = (".cpp", ".cc", ".cxx", ".hpp", ".h")
TAG = re.compile(
    r"^\s*//\s*SWEGCA:\s+((?:[\w./-]+\.py@[0-9a-f]{7,40}|user@\d{4}-\d{2}-\d{2}):\d+(?:-\d+)?)\s*$",
    re.MULTILINE,
)
FORBIDDEN = re.compile(
    r"\b(?:bm25|idf|embedding|cosine|hnsw|pagerank|torch|llm|http|"
    r"sqlite|tfidf|tf_idf|faiss|knn|rerank|softmax|logit|tokenizer|openai|"
    r"anthropic|gpt)\b", re.I,
)
CONTROL = {"if", "for", "while", "switch", "catch", "sizeof", "alignof", "requires"}
CODEX_ROOT = Path(__file__).resolve().parents[2]
SOURCE_ROOTS = (
    Path(os.environ.get("SWEGCA_TINYLM_SOURCE_ROOT", CODEX_ROOT / "tinylm-slicer-sanabi-bazzite")),
    Path(os.environ.get("SWEGCA_ARCH_SOURCE_ROOT", CODEX_ROOT / "SWEGCA-Architecture")),
    Path(os.environ.get("SWEGCA_VRS_SOURCE_ROOT", CODEX_ROOT / "SWEGCA-VRS-MCP")),
)


def git_bytes(*args: str, root: Path | None = None) -> bytes:
    command = ("git", "-C", str(root), *args) if root else ("git", *args)
    environment = os.environ.copy()
    if root is not None:
        # A Git hook exports GIT_DIR/GIT_WORK_TREE for this worktree. Those
        # variables would redirect even `git -C <author repo>` to this repo.
        for name in tuple(environment):
            if name.startswith("GIT_"):
                environment.pop(name)
    return subprocess.check_output(command, stderr=subprocess.DEVNULL, env=environment)


def staged_paths() -> list[str]:
    raw = git_bytes("diff", "--cached", "--name-only", "--diff-filter=ACMR", "-z")
    return [item.decode("utf-8", "surrogateescape") for item in raw.split(b"\0") if item]


def committed_paths(commit: str) -> list[str]:
    raw = git_bytes("diff-tree", "--no-commit-id", "--name-only", "-r", "-z",
                    "--diff-filter=ACMR", commit)
    return [item.decode("utf-8", "surrogateescape") for item in raw.split(b"\0") if item]


@lru_cache(maxsize=256)
def author_blob(reference: str) -> bytes | None:
    source, rest = reference.split("@", 1)
    commit = rest.split(":", 1)[0]
    for root in SOURCE_ROOTS:
        try:
            paths = git_bytes("ls-tree", "-r", "--name-only", commit, root=root).decode().splitlines()
            matches = [path for path in paths if path == source or path.endswith("/" + source)]
            if len(matches) == 1:
                return git_bytes("show", f"{commit}:{matches[0]}", root=root)
            if len(matches) > 1:
                return None
        except (OSError, subprocess.CalledProcessError, UnicodeDecodeError):
            continue
    return None


def valid_tag(reference: str) -> bool:
    source, rest = reference.split("@", 1)
    revision, line_span = rest.split(":", 1)
    first, _, last = line_span.partition("-")
    start, end = int(first), int(last or first)
    if start < 1 or end < start:
        return False
    if source == "user":
        approved = Path(__file__).resolve().parents[1] / "docs" / "SWEGCA_VRS_MCP_ORDER_FOR_REVIEW.md"
        if not approved.is_file():
            return False
        return end <= len(approved.read_text(encoding="utf-8").splitlines())
    blob = author_blob(reference)
    return blob is not None and end <= len(blob.splitlines())


def protected_author_copy(path: str, staged: bytes) -> bool:
    if not path.startswith(AUTHOR_NAMESPACES):
        return True
    original = author_blob(path.rsplit("/", 1)[-1] + "@3bddcb7:1")
    return original is not None and original == staged


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
        # Reject braced value initializers such as std::byte{0} and
        # Json(Json::Array{}); they follow a call but are not definitions.
        if not re.fullmatch(
            r"\s*(?:(?:const|noexcept|override|final)\s*)*"
            r"(?:->\s*[\w:<>, *&]+\s*)?", tail
        ):
            continue
        # Ignore macro declarations and class/namespace openers.
        if re.search(r"\b(?:class|struct|namespace|enum)\s+[^{};]*$", prefix):
            continue
        positions.append(at)
    return positions


def check_cpp(path: str, source: str) -> list[str]:
    issues: list[str] = []
    tags = list(TAG.finditer(source))
    clean = without_comments_and_strings(source)
    definitions = definition_positions(clean)
    if definitions and not tags:
        return [f"{path}: missing // SWEGCA: source@revision:lines tag"]
    for tag in tags:
        if not valid_tag(tag.group(1)):
            line = source.count("\n", 0, tag.start()) + 1
            issues.append(f"{path}:{line}: SWEGCA source tag has no verified source span")
    if forbidden := FORBIDDEN.search(clean):
        issues.append(f"{path}: external memory vocabulary {forbidden.group(0)!r}")
    used = -1
    for position in definitions:
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
    selection = parser.add_mutually_exclusive_group(required=True)
    selection.add_argument("--staged", action="store_true")
    selection.add_argument("--commit")
    args = parser.parse_args()
    issues: list[str] = []
    for path in staged_paths() if args.staged else committed_paths(args.commit):
        blob = git_bytes("show", f":{path}" if args.staged else f"{args.commit}:{path}")
        if path.startswith(AUTHOR_NAMESPACES):
            if not protected_author_copy(path, blob):
                issues.append(f"{path}: author Python copy differs from pinned source")
            continue
        if not path.startswith(PRODUCT_PREFIXES) or not path.endswith(CPP_SUFFIXES):
            continue
        source = blob.decode("utf-8", "strict")
        issues.extend(check_cpp(path, source))
    if issues:
        for issue in issues:
            print(issue, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
