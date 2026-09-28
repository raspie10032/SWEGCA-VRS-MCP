#!/usr/bin/env python3
"""Emit and verify the Stage 0 source, toolchain, and binary receipt.

This is a build-audit tool that uses only the Python standard library.  It is
not linked into, imported by, or required at runtime by SWEGCA-VRS.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
from typing import Iterable


FORBIDDEN_PATH_PARTS = ("/venv/", "/site-packages/", ".whl")
FORBIDDEN_LINK_NAMES = ("sqlite", "hermes")


def run(arguments: list[str], cwd: Path) -> str:
    completed = subprocess.run(
        arguments,
        cwd=cwd,
        check=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )
    return completed.stdout


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        while chunk := source.read(1024 * 1024):
            digest.update(chunk)
    return digest.hexdigest()


def json_digest(value: object) -> str:
    encoded = json.dumps(value, sort_keys=True, separators=(",", ":")).encode()
    return hashlib.sha256(encoded).hexdigest()


def normalize_paths(root: Path, values: Iterable[str], role: str) -> list[dict[str, object]]:
    records: list[dict[str, object]] = []
    seen: set[str] = set()
    for value in values:
        relative = Path(value)
        if relative.is_absolute() or ".." in relative.parts:
            raise RuntimeError(f"{role} path escapes repository: {value}")
        normalized = relative.as_posix()
        if normalized in seen:
            continue
        seen.add(normalized)
        absolute = root / relative
        if not absolute.is_file():
            raise RuntimeError(f"{role} source is missing: {normalized}")
        records.append(
            {
                "path": normalized,
                "bytes": absolute.stat().st_size,
                "sha256": sha256_file(absolute),
            }
        )
    records.sort(key=lambda row: str(row["path"]))
    return records


def binary_record(root: Path, value: str) -> dict[str, object]:
    relative = Path(value)
    if relative.is_absolute() or ".." in relative.parts:
        raise RuntimeError(f"binary path escapes repository: {value}")
    path = root / relative
    if not path.is_file():
        raise RuntimeError(f"production binary is missing: {relative.as_posix()}")
    dependency_text = run(["ldd", str(path)], root)
    lower_dependencies = dependency_text.lower()
    for forbidden in FORBIDDEN_PATH_PARTS:
        if forbidden in lower_dependencies:
            raise RuntimeError(f"forbidden dependency origin {forbidden}: {relative}")
    for forbidden in FORBIDDEN_LINK_NAMES:
        if forbidden in lower_dependencies:
            raise RuntimeError(f"forbidden linked library {forbidden}: {relative}")
    return {
        "path": relative.as_posix(),
        "bytes": path.stat().st_size,
        "sha256": sha256_file(path),
        "dynamic_dependencies": [line.strip() for line in dependency_text.splitlines() if line.strip()],
    }


def validate_external_manifest(path: Path) -> dict[str, object]:
    data = json.loads(path.read_text())
    if data.get("schema") != "swegca-stage0-external-p0-v1":
        raise RuntimeError("unexpected external P0 manifest schema")
    entries = data.get("entries")
    if not isinstance(entries, list) or len(entries) != 98:
        raise RuntimeError("external P0 manifest must contain exactly 98 entries")
    references: set[str] = set()
    production_requirements = 0
    counts: dict[str, int] = {}
    for row in entries:
        if not isinstance(row, dict):
            raise RuntimeError("external P0 entry is not an object")
        reference = row.get("reference")
        status = row.get("status")
        if not isinstance(reference, str) or not reference or reference in references:
            raise RuntimeError("external P0 reference is missing or duplicated")
        references.add(reference)
        if status not in {"acquired", "unsupported", "missing"}:
            raise RuntimeError(f"invalid external P0 status for {reference}")
        if status == "acquired" and not row.get("known_sha256"):
            raise RuntimeError(f"acquired external P0 lacks SHA-256: {reference}")
        counts[status] = counts.get(status, 0) + 1
        if row.get("required_by_current_production"):
            production_requirements += 1
            if status != "acquired":
                raise RuntimeError(f"production external requirement is unavailable: {reference}")
    return {
        "path": path.name,
        "bytes": path.stat().st_size,
        "sha256": sha256_file(path),
        "entries": len(entries),
        "status_counts": counts,
        "required_by_current_production": production_requirements,
    }


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", required=True)
    parser.add_argument("--build-dir", required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("--build-log", required=True)
    parser.add_argument("--compiler", required=True)
    parser.add_argument("--external-manifest", required=True)
    parser.add_argument("--mcp-source", action="append", default=[])
    parser.add_argument("--support-source", action="append", default=[])
    parser.add_argument("--gate-source", action="append", default=[])
    parser.add_argument("--experimental-source", action="append", default=[])
    parser.add_argument("--production-binary", action="append", default=[])
    return parser.parse_args()


def main() -> int:
    arguments = parse_arguments()
    root = Path(arguments.root).resolve()
    build_dir = (root / arguments.build_dir).resolve()
    output = (root / arguments.output).resolve()
    build_log = (root / arguments.build_log).resolve()
    external_manifest = (root / arguments.external_manifest).resolve()
    if not root.is_dir() or not build_dir.is_dir():
        raise RuntimeError("repository or clean build directory is missing")
    if not build_log.is_file():
        raise RuntimeError("clean build log is missing")
    if run(["git", "status", "--porcelain"], root).strip():
        raise RuntimeError("Stage 0 receipt requires a clean Git worktree")
    head = run(["git", "rev-parse", "HEAD"], root).strip()
    branch = run(["git", "rev-parse", "--abbrev-ref", "HEAD"], root).strip()

    compiler_tokens = shlex.split(arguments.compiler)
    if not compiler_tokens:
        raise RuntimeError("empty compiler command")
    compiler_path = shutil.which(compiler_tokens[0])
    if compiler_path is None:
        raise RuntimeError(f"compiler is not executable: {compiler_tokens[0]}")
    compiler_path = str(Path(compiler_path).resolve())
    for forbidden in FORBIDDEN_PATH_PARTS:
        if forbidden in compiler_path.lower():
            raise RuntimeError(f"compiler comes from forbidden dependency origin: {compiler_path}")

    mcp_sources = normalize_paths(root, arguments.mcp_source, "MCP production")
    support_sources = normalize_paths(root, arguments.support_source, "production support")
    gate_sources = normalize_paths(root, arguments.gate_source, "production gate")
    experimental_sources = normalize_paths(root, arguments.experimental_source, "experimental")
    if not mcp_sources or not arguments.production_binary:
        raise RuntimeError("empty production closure")
    binaries = [binary_record(root, value) for value in sorted(set(arguments.production_binary))]
    external = validate_external_manifest(external_manifest)

    build_lines = [line for line in build_log.read_text(errors="replace").splitlines() if line.strip()]
    compiler_name = Path(compiler_tokens[0]).name
    command_lines = [line for line in build_lines if compiler_name in line]
    if not command_lines:
        raise RuntimeError("clean build log contains no compiler/link command")

    source_roles: dict[str, list[str]] = {}
    for role, records in (
        ("mcp_production", mcp_sources),
        ("production_support", support_sources),
        ("production_gate", gate_sources),
        ("experimental", experimental_sources),
    ):
        for row in records:
            source_roles.setdefault(str(row["path"]), []).append(role)
    overlaps = sorted(path for path, roles in source_roles.items() if len(roles) > 1)
    receipt = {
        "schema": "swegca-stage0-receipt-v1",
        "result": "pass",
        "claim_boundary": "Stage 0 proves a clean source, toolchain, dependency and binary closure. It does not prove architecture parity, runtime integration, performance or scientific effect.",
        "git": {"branch": branch, "head": head, "clean": True},
        "toolchain": {
            "compiler": compiler_path,
            "compiler_version": run([compiler_path, "--version"], root).splitlines()[0],
            "linker_version": run(["ld", "--version"], root).splitlines()[0],
            "command_count": len(command_lines),
            "commands": command_lines,
        },
        "build_log": {
            "path": build_log.relative_to(root).as_posix(),
            "bytes": build_log.stat().st_size,
            "sha256": sha256_file(build_log),
        },
        "source_closure": {
            "mcp_production": mcp_sources,
            "production_support": support_sources,
            "production_gate": gate_sources,
            "experimental": experimental_sources,
            "role_overlap": overlaps,
            "manifest_sha256": json_digest(
                {
                    "mcp_production": mcp_sources,
                    "production_support": support_sources,
                    "production_gate": gate_sources,
                    "experimental": experimental_sources,
                }
            ),
        },
        "external_p0": external,
        "production_binaries": binaries,
        "forbidden_dependency_scan": {
            "path_parts": list(FORBIDDEN_PATH_PARTS),
            "linked_names": list(FORBIDDEN_LINK_NAMES),
            "violations": [],
        },
    }
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(receipt, ensure_ascii=False, indent=2) + "\n")
    reread = json.loads(output.read_text())
    if reread.get("result") != "pass" or reread.get("git", {}).get("head") != head:
        raise RuntimeError("Stage 0 receipt self-verification failed")
    print(
        f"stage0 gate: PASS; mcp_sources={len(mcp_sources)} "
        f"support_sources={len(support_sources)} gate_sources={len(gate_sources)} "
        f"experimental_sources={len(experimental_sources)} binaries={len(binaries)} "
        f"receipt_sha256={sha256_file(output)}"
    )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, subprocess.CalledProcessError, RuntimeError, ValueError, json.JSONDecodeError) as error:
        print(f"stage0 gate: FAIL: {error}", file=sys.stderr)
        raise SystemExit(2)
