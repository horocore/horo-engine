#!/usr/bin/env python3
"""Select the installed Mesa lavapipe manifest for explicit native qualification."""

import json
from pathlib import Path
import subprocess


def select_manifest(package_paths: list[str]) -> Path:
    """Require one valid manifest from the installed package, without a GPU fallback."""
    candidates = sorted({Path(path) for path in package_paths
                         if Path(path).name.startswith("lvp_icd") and Path(path).suffix == ".json"
                         and Path(path).is_file()})
    if len(candidates) != 1:
        raise ValueError(f"Expected exactly one installed lavapipe ICD manifest, found {len(candidates)}")
    selected = candidates[0].resolve(strict=True)
    manifest = json.loads(selected.read_text(encoding="utf-8"))
    if not isinstance(manifest.get("ICD", {}).get("library_path"), str) or not manifest["ICD"]["library_path"]:
        raise ValueError("Installed lavapipe ICD manifest has no library path")
    return selected


def main() -> None:
    inventory = subprocess.run(["dpkg-query", "-L", "mesa-vulkan-drivers"], check=True,
                               capture_output=True, text=True)
    print(select_manifest(inventory.stdout.splitlines()))


if __name__ == "__main__":
    main()
