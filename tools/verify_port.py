"""Offline integrity check; hashes are not an authenticity/security certificate."""
import hashlib
import json
from pathlib import Path, PurePosixPath

ROOT = Path(__file__).resolve().parents[1]


def verify():
    manifest = json.loads((ROOT / "UPSTREAM.json").read_text(encoding="utf-8"))
    errors = []
    rows = list(manifest["files"])
    for name in ("ARCHITECTURE_UPSTREAM.json", "VRS_UPSTREAM.json"):
        rows.extend(json.loads((ROOT / name).read_text(encoding="utf-8"))["files"])
    for row in rows:
        name = PurePosixPath(row["destination"])
        if name.is_absolute() or ".." in name.parts:
            errors.append("unsafe manifest path")
            continue
        path = ROOT / name
        if path.is_symlink() or not path.is_file() or not path.resolve().is_relative_to(ROOT):
            errors.append(f"missing or unsafe: {name}")
            continue
        if hashlib.sha256(path.read_bytes()).hexdigest() != row["port_sha256"]:
            errors.append(f"changed: {name}")
    return {"status": "FAIL" if errors else "PASS", "checked_files": len(rows),
            "errors": errors, "algorithm_changes_declared": manifest["algorithm_changes"],
            "omitted_upstream_tests": [r["omitted_test"] for r in manifest["files"] if r.get("omitted_test")],
            "scope": "local port byte consistency; not empirical validation or authentication"}


if __name__ == "__main__":
    result = verify()
    print(json.dumps(result, indent=2))
    raise SystemExit(result["status"] != "PASS")
