#!/usr/bin/env python3
"""Static source-lineage gate for staged C++ product files.

This does not establish algorithmic parity. An independent source review still
checks every tag against the author function and accepted user directive.
"""

from __future__ import annotations

import argparse
from functools import lru_cache
import hashlib
import os
from pathlib import Path
import re
import subprocess
import sys


PRODUCT_PREFIXES = ("native/", "include/", "cpp/")
AUTHOR_NAMESPACES = ("src/swegca_vrs2/engine/mosaic_", "src/tinylm_slicer/mosaic_")
CPP_SUFFIXES = (".cpp", ".cc", ".cxx", ".c", ".cp", ".cu", ".mm", ".cppm", ".cxxm", ".mxx",
                ".hpp", ".h", ".hxx",
                ".hh", ".inl", ".ipp", ".tpp", ".tcc", ".ixx", ".inc",
                ".c++", ".h++")
TAG = re.compile(
    r"^[ \t]*//[ \t]*SWEGCA:[ \t]+((?:[\w./-]+\.(?:py|md)@[0-9a-f]{7,40}|user@\d{4}-\d{2}-\d{2}):\d+(?:-\d+)?)[ \t]*$",
    re.MULTILINE,
)
FORBIDDEN = re.compile(
    r"\b(?:bm25|idf|embedding|cosine|hnsw|pagerank|torch|llm|http|"
    r"sqlite|tfidf|tf_idf|faiss|knn|rerank|softmax|logit|tokenizer|openai|"
    r"anthropic|gpt)\b", re.I,
)
CONTROL = {"if", "for", "while", "switch", "catch", "sizeof", "alignof", "requires"}
CORE_STANDARD_HEADERS = frozenset({
    "algorithm", "array", "bit", "cmath", "compare", "cstddef", "cstdint",
    "cstring", "limits", "span", "stdexcept", "string", "string_view",
})
CORE_LOCAL_INCLUDE = re.compile(r'"swegca_architecture/[a-z0-9_]+\.hpp"\Z')
CORE_INCLUDE_DIRECTIVE = re.compile(r'^\s*#\s*(include|include_next|import)\b')
CORE_INCLUDE_OPERAND = re.compile(r'^\s*#\s*include\s+(\S+)\s*\Z')
CODEX_ROOT = Path(__file__).resolve().parents[2]
SOURCE_ROOTS = (
    Path(__file__).resolve().parents[1],
    Path(os.environ.get("SWEGCA_TINYLM_SOURCE_ROOT", CODEX_ROOT / "tinylm-slicer-sanabi-bazzite")),
    Path(os.environ.get("SWEGCA_ARCH_SOURCE_ROOT", CODEX_ROOT / "SWEGCA-Architecture")),
)


PINNED_TINYLM = "3bddcb7adc8c07e21a57d8c921d312aed83270e5"
PINNED_ARCH = "5901a5aa2dcbd0ac7ad12ac6dd745699f72288a8"
PINNED_LOCAL_REVISIONS = {
    "7536139": "7536139d5f7b95879f9ba950f0211bc16f4e32c5",
    "c06092a": "c06092af7f6d050fc41a950be637b8e4cea584bc",
    "0dc716a": "0dc716a0153160fb39f2fc063dda9e20ffb9d942",
}
ORDER_PATH = "docs/SWEGCA_VRS_MCP_ORDER_FOR_REVIEW.md"
ORDER_2026_09_22 = "30b73e7cbd5bef29e32db0d9d947c8e70f8622e0"
ORDER_2026_09_23 = "fcab35bc9609840afbf2987680b317e03cc3fd78"

# The product and its original VRS sources share one Git object database.
# Only these inspected, exact source pairs from that database can be cited.
# A source in this list is an input to semantic review, not proof of parity or
# proof that a ported file is the original author's exact bytes.
PINNED_LOCAL_SOURCE_PAIRS: frozenset[tuple[str, str]] = frozenset({
    ("src/swegca_vrs2/conversation_finalize.py", "c06092a"),
    ("src/swegca_vrs2/cue_shards.py", "c06092a"),
    ("src/swegca_vrs2/engine/mosaic_memory_activation.py", "7536139"),
    ("src/swegca_vrs2/engine/mosaic_memory_promotion.py", "7536139"),
    ("src/swegca_vrs2/engine/mosaic_semantic_family_directory.py", "7536139"),
    ("src/swegca_vrs2/engine/mosaic_vrs_coactivation.py", "c06092a"),
    ("src/swegca_vrs2/engine/mosaic_vrs_coactivation_navigation.py", "c06092a"),
    ("src/swegca_vrs2/engine/mosaic_vrs_connectivity_regions.py", "7536139"),
    ("src/swegca_vrs2/engine/mosaic_vrs_dependency_index.py", "7536139"),
    ("src/swegca_vrs2/engine/mosaic_vrs_event_delta.py", "7536139"),
    ("src/swegca_vrs2/engine/mosaic_vrs_event_kernel.py", "7536139"),
    ("src/swegca_vrs2/engine/mosaic_vrs_event_signal.py", "7536139"),
    ("src/swegca_vrs2/engine/mosaic_vrs_portal_lifecycle.py", "c06092a"),
    ("src/swegca_vrs2/engine/mosaic_vrs_region_arrays.py", "7536139"),
    ("src/swegca_vrs2/engine/mosaic_vrs_region_publication.py", "0dc716a"),
    ("src/swegca_vrs2/engine/mosaic_vrs_state_update.py", "7536139"),
    ("src/swegca_vrs2/exact_replay.py", "c06092a"),
    ("src/swegca_vrs2/linked_shards.py", "c06092a"),
    ("src/swegca_vrs2/native_context.py", "7536139"),
    ("src/swegca_vrs2/native_journal.py", "c06092a"),
    ("src/swegca_vrs2/native_lock.py", "c06092a"),
    ("src/swegca_vrs2/native_transport.py", "7536139"),
    ("src/swegca_vrs2/session_capture.py", "c06092a"),
    ("src/swegca_vrs2/store.py", "7536139"),
    ("src/swegca_vrs2/store.py", "c06092a"),
})


def pinned_revision(given: str, pinned: str) -> bool:
    return len(given) >= 7 and pinned.startswith(given)


def git_bytes(*args: str, root: Path | None = None) -> bytes:
    command = ("git", "-C", str(root), *args) if root else ("git", *args)
    environment = os.environ.copy()
    if root is not None:
        # A Git hook exports GIT_DIR/GIT_WORK_TREE for this worktree. Those
        # variables would redirect even `git -C <author repo>` to this repo.
        for name in tuple(environment):
            if name.startswith("GIT_"):
                environment.pop(name)
    # A replace ref can substitute a different commit's tree even when
    # `git show` is passed a complete object ID. This applies to source
    # lookups and to the commit selected by --commit.
    environment["GIT_NO_REPLACE_OBJECTS"] = "1"
    return subprocess.check_output(command, stderr=subprocess.DEVNULL, env=environment)


def grafts_present(root: Path | None = None) -> bool:
    # GIT_NO_REPLACE_OBJECTS does not disable the older info/grafts file.
    # Refuse a repository with grafts before trusting its commit ancestry.
    raw = git_bytes("rev-parse", "--path-format=absolute", "--git-path", "info/grafts", root=root)
    return Path(os.fsdecode(raw).removesuffix("\n")).exists()


def staged_paths() -> list[str]:
    raw = git_bytes("diff", "--cached", "--no-renames", "--name-only",
                    "--diff-filter=ACMRDT", "-z")
    return [item.decode("utf-8", "surrogateescape") for item in raw.split(b"\0") if item]


def all_index_paths() -> list[str]:
    raw = git_bytes("ls-files", "-z", "--cached")
    return [item.decode("utf-8", "surrogateescape") for item in raw.split(b"\0") if item]


def committed_paths(commit: str) -> list[str]:
    raw = git_bytes("diff-tree", "--root", "-m", "--no-commit-id",
                    "--no-renames", "--name-only", "-r", "-z",
                    "--diff-filter=ACMRDT", commit)
    return list(dict.fromkeys(item.decode("utf-8", "surrogateescape")
                              for item in raw.split(b"\0") if item))


def file_mode(path: str, commit: str | None) -> str | None:
    if commit is None:
        raw = git_bytes("ls-files", "--stage", "-z", "--", path)
    else:
        raw = git_bytes("ls-tree", "-z", commit, "--", path)
    entries = [item for item in raw.split(b"\0") if item]
    if not entries:
        return None
    if len(entries) != 1 or b"\t" not in entries[0]:
        return "unmerged"
    return entries[0].split(b" ", 1)[0].decode("ascii", "strict")


def is_product_path(path: str) -> bool:
    lower = path.lower()
    # A compiled source is product code even if a build file places it under
    # src/, tools/, or another directory. Check the directory entries too:
    # Git can replace a top-level source directory with a symlink.
    return (lower.startswith(PRODUCT_PREFIXES) or
            lower in ("native", "include", "cpp") or
            lower.endswith(CPP_SUFFIXES))


@lru_cache(maxsize=256)
def source_blob(root_index: int, full_revision: str, source: str) -> bytes | None:
    if source.startswith("/") or any(part in ("", ".", "..") for part in source.split("/")):
        return None
    root = SOURCE_ROOTS[root_index]
    try:
        if grafts_present(root):
            return None
        if git_bytes("rev-parse", "--show-object-format", root=root) != b"sha1\n":
            return None
        commit = verified_source_object(root, full_revision, "commit")
        if commit is None:
            return None
        tree_line = commit.split(b"\n", 1)[0]
        if not re.fullmatch(rb"tree [0-9a-f]{40}", tree_line):
            return None
        oid = tree_line[5:].decode("ascii")
        parts = [os.fsencode(part) for part in source.split("/")]
        for index, part in enumerate(parts):
            tree = verified_source_object(root, oid, "tree")
            if tree is None:
                return None
            entries: list[tuple[bytes, bytes, str]] = []
            at = 0
            while at < len(tree):
                space = tree.find(b" ", at)
                nul = tree.find(b"\0", space + 1)
                if space < 0 or nul < 0 or nul + 21 > len(tree):
                    return None
                entries.append((tree[at:space], tree[space + 1:nul],
                                tree[nul + 1:nul + 21].hex()))
                at = nul + 21
            found = [entry for entry in entries if entry[1] == part]
            if len(found) != 1:
                return None
            mode, _, oid = found[0]
            if index + 1 < len(parts):
                if mode != b"40000":
                    return None
            elif mode not in (b"100644", b"100755"):
                return None
        return verified_source_object(root, oid, "blob")
    except (OSError, subprocess.CalledProcessError):
        return None


def verified_source_object(root: Path, oid: str, kind: str) -> bytes | None:
    # Git can read a loose object stored under a forged hash without checking
    # its contents. Validate the exact commit, every path tree, and the blob.
    # Require Git's own object hash as well: a plain hashlib SHA-1 check alone
    # does not use Git's collision-detecting SHA-1 implementation.
    if not re.fullmatch(r"[0-9a-f]{40}", oid):
        return None
    if git_bytes("cat-file", "-t", oid, root=root) != (kind + "\n").encode():
        return None
    data = git_bytes("cat-file", kind, oid, root=root)
    framed = kind.encode() + b" " + str(len(data)).encode() + b"\0" + data
    if hashlib.sha1(framed).hexdigest() != oid:
        return None
    environment = os.environ.copy()
    for name in tuple(environment):
        if name.startswith("GIT_"):
            environment.pop(name)
    environment["GIT_NO_REPLACE_OBJECTS"] = "1"
    git_oid = subprocess.check_output(
        ("git", "-C", str(root), "hash-object", "-t", kind, "--stdin"),
        input=data, stderr=subprocess.DEVNULL, env=environment,
    ).decode("ascii", "strict").strip()
    return data if git_oid == oid else None


def author_blob(reference: str) -> bytes | None:
    source, rest = reference.split("@", 1)
    revision = rest.split(":", 1)[0]
    if source.startswith("src/tinylm_slicer/") and pinned_revision(revision, PINNED_TINYLM):
        return source_blob(1, PINNED_TINYLM, source)
    return None


def valid_tag(reference: str) -> bool:
    source, rest = reference.split("@", 1)
    revision, line_span = rest.split(":", 1)
    first, _, last = line_span.partition("-")
    start, end = int(first), int(last or first)
    if start < 1 or end < start:
        return False
    if source == "user":
        # The review order has an original 22 Sep version and a 23 Sep
        # amendment. Only amendment lines may be cited as 23 Sep directives.
        if revision == "2026-09-22":
            order_revision = ORDER_2026_09_22
            approved = not any(start <= hi and end >= lo for lo, hi in
                               ((69, 70), (76, 78)))
        elif revision == "2026-09-23":
            order_revision = ORDER_2026_09_23
            approved = any(start >= lo and end <= hi for lo, hi in
                           ((69, 70), (76, 78), (112, 115)))
        else:
            return False
        blob = source_blob(0, order_revision, ORDER_PATH)
        return approved and blob is not None and end <= len(blob.splitlines())
    if source.startswith("src/swegca_vrs2/"):
        full_revision = next((full for short, full in PINNED_LOCAL_REVISIONS.items()
                              if pinned_revision(revision, full) and
                              (source, short) in PINNED_LOCAL_SOURCE_PAIRS), None)
        blob = None if full_revision is None else source_blob(0, full_revision, source)
    elif source.startswith("src/tinylm_slicer/") and pinned_revision(revision, PINNED_TINYLM):
        blob = source_blob(1, PINNED_TINYLM, source)
    elif source.startswith(("src/swegca/", "paper/swegca/")) and pinned_revision(revision, PINNED_ARCH):
        blob = source_blob(2, PINNED_ARCH, source)
    else:
        return False
    if blob is None or end > len(blob.splitlines()):
        return False
    return True


def protected_author_copy(path: str, staged: bytes) -> bool:
    if not path.startswith(AUTHOR_NAMESPACES):
        return True
    # The copied author's engine module lives under a different package
    # prefix in this repository. Rewrite that exact prefix, never a basename.
    source_path = path.replace("src/swegca_vrs2/engine/mosaic_",
                               "src/tinylm_slicer/mosaic_", 1)
    original = author_blob(source_path + "@" + PINNED_TINYLM + ":1")
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
        line = source.count("\n", 0, tag.start()) + 1
        if not valid_tag(tag.group(1)):
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


def check_layering(path: str, source: str) -> list[str]:
    if not path.startswith("cpp/swegca_architecture/"):
        return []
    issues: list[str] = []
    code = without_comments_and_strings(source)
    # The masked source identifies actual directives without mistaking comments
    # or strings for code. Validate the original operand, including its quotes.
    for line_number, (masked, original) in enumerate(
        zip(code.splitlines(), source.splitlines()), 1
    ):
        if not CORE_INCLUDE_DIRECTIVE.match(masked):
            continue
        match = CORE_INCLUDE_OPERAND.fullmatch(original)
        operand = match.group(1) if match else ""
        standard = operand.startswith("<") and operand.endswith(">") and (
            operand[1:-1] in CORE_STANDARD_HEADERS
        )
        if not (CORE_LOCAL_INCLUDE.fullmatch(operand) or standard):
            issues.append(f"{path}:{line_number}: SWEGCA verifier include is outside core allowlist")
    if re.search(r'\bvrs\b', code):
        issues.append(f"{path}: SWEGCA verifier references VRS")
    return issues


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    selection = parser.add_mutually_exclusive_group(required=True)
    selection.add_argument("--staged", action="store_true")
    selection.add_argument("--all", action="store_true", help="audit every tracked file in the index")
    selection.add_argument("--commit")
    args = parser.parse_args()
    issues: list[str] = []
    if grafts_present():
        print("Git info/grafts is present; source lineage cannot be trusted", file=sys.stderr)
        return 1
    paths = staged_paths() if args.staged else all_index_paths() if args.all else committed_paths(args.commit)
    for path in paths:
        protected = path.startswith(AUTHOR_NAMESPACES)
        product = is_product_path(path)
        if not protected and not product:
            continue
        mode = file_mode(path, None if args.staged or args.all else args.commit)
        if mode is None:
            if protected:
                issues.append(f"{path}: protected author copy was deleted or moved")
            continue
        if mode not in ("100644", "100755"):
            issues.append(f"{path}: non-regular product or author file mode {mode}")
            continue
        blob = git_bytes("show", f":{path}" if args.staged or args.all else f"{args.commit}:{path}")
        if protected:
            if not protected_author_copy(path, blob):
                issues.append(f"{path}: author Python copy differs from pinned source")
            continue
        if not path.lower().endswith(CPP_SUFFIXES):
            issues.append(f"{path}: unrecognized product source suffix")
            continue
        source = blob.decode("utf-8", "strict")
        issues.extend(check_layering(path, source))
        issues.extend(check_cpp(path, source))
    if issues:
        for issue in issues:
            print(issue, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
