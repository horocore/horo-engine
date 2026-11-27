#!/usr/bin/env python3
"""Stage and verify the macOS editor bundle's capture requirements."""

import argparse
import plistlib
import subprocess
import sys
from pathlib import Path


AUDIO_INPUT = "com.apple.security.device.audio-input"
USAGE_FIELDS = ("NSMicrophoneUsageDescription", "NSSpeechRecognitionUsageDescription")
ENTITLEMENTS = (
    Path(__file__).resolve().parents[1]
    / "apps/HoroEditor/resources/HoroEditorCapture.entitlements"
)


def stage_editor(build_dir: Path, destination: Path, identity: str) -> Path:
    if not identity.strip():
        raise ValueError("A signing identity is required.")
    bundle = destination / "HoroEditor.app"
    if bundle.is_symlink() or bundle.exists():
        raise ValueError("HoroEditor.app already exists in the staging directory.")
    subprocess.run(
        ["cmake", "--install", str(build_dir), "--component", "Editor", "--prefix", str(destination)],
        check=True,
    )
    metadata = plistlib.loads((bundle / "Contents/Info.plist").read_bytes())
    for field in USAGE_FIELDS:
        value = metadata.get(field) if isinstance(metadata, dict) else None
        if not isinstance(value, str) or not value.strip():
            raise ValueError(f"The staged editor is missing a valid {field}.")

    subprocess.run(
        ["codesign", "--force", "--sign", identity, "--entitlements", str(ENTITLEMENTS), str(bundle)],
        check=True,
    )
    subprocess.run(["codesign", "--verify", "--deep", "--strict", str(bundle)], check=True)
    signature = subprocess.run(
        ["codesign", "--display", "--entitlements", "-", "--xml", str(bundle)],
        stdout=subprocess.PIPE,
        check=True,
    )
    signed = plistlib.loads(signature.stdout)
    if not isinstance(signed, dict) or signed.get(AUDIO_INPUT) is not True:
        raise ValueError("The staged editor signature is missing the audio-input entitlement.")
    return bundle


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("build_dir", type=Path)
    parser.add_argument("destination", type=Path)
    parser.add_argument("--signing-identity", required=True)
    args = parser.parse_args()
    try:
        stage_editor(args.build_dir, args.destination.resolve(), args.signing_identity)
    except (OSError, ValueError, plistlib.InvalidFileException, subprocess.CalledProcessError) as error:
        print(f"Could not stage the macOS editor: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
