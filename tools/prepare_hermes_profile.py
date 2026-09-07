"""Create a NEW isolated Hermes profile; never rewrite an existing profile.

Installs only plugin files/configuration, not Hermes, credentials, model settings,
or a background service. Install this package in the Hermes Python environment
(without core extras) and run the core service separately with core extras.
"""
import argparse
import json
import os
from pathlib import Path
import shutil


def prepare(profile: Path, socket_path: Path):
    if not profile.is_absolute() or not socket_path.is_absolute():
        raise ValueError("use absolute paths")
    if len(str(socket_path).encode()) > 100 or any(c in str(socket_path) for c in "\n\r\x00"):
        raise ValueError("use a short, single-line Unix socket path")
    # Existing memory/config must be explicitly migrated by the operator.
    profile.mkdir(mode=0o700, parents=True, exist_ok=False)
    plugin = Path(__file__).resolve().parents[1] / "integrations/hermes/swegca-vrs"
    shutil.copytree(plugin, profile / "plugins/swegca-vrs", ignore=shutil.ignore_patterns("__pycache__"))
    (profile / "config.yaml").write_text(
        "# SWEGCA is the only long-term memory provider; host transcript remains intact.\n"
        "memory:\n  provider: swegca-vrs\n  memory_enabled: false\n  user_profile_enabled: false\n",
        encoding="utf-8")
    (profile / ".env").write_text("SWEGCA_MEMORY_SOCKET=" + json.dumps(str(socket_path)) + "\n", encoding="utf-8")
    os.chmod(profile / ".env", 0o600)
    return {"profile": str(profile), "socket": str(socket_path),
            "model_configured": False, "service_started": False,
            "existing_profiles_modified": False}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--profile", type=Path, required=True)
    parser.add_argument("--socket", type=Path, required=True)
    args = parser.parse_args()
    print(json.dumps(prepare(args.profile, args.socket), indent=2))
