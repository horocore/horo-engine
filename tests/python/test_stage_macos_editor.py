"""Verify staging order and fail-closed checks for the installed editor."""

import plistlib
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scripts"))
import stage_macos_editor as stage


class StageMacEditorTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.destination = Path(temporary.name) / "release with spaces"
        self.bundle = self.destination / "HoroEditor.app"
        self.metadata = {key: "A usage description" for key in stage.USAGE_FIELDS}
        self.signed_entitlements = {stage.AUDIO_INPUT: True}

    def run_tool(self, command, **_options):
        if command[0] == "cmake":
            contents = self.bundle / "Contents"
            (contents / "MacOS").mkdir(parents=True)
            (contents / "MacOS/HoroEditor").write_bytes(b"built editor")
            (contents / "Info.plist").write_bytes(plistlib.dumps(self.metadata))
        if "--display" in command:
            return subprocess.CompletedProcess(command, 0, plistlib.dumps(self.signed_entitlements))
        return subprocess.CompletedProcess(command, 0)

    def test_signs_and_verifies_the_installed_bundle(self):
        with patch.object(stage.subprocess, "run", side_effect=self.run_tool) as run:
            self.assertEqual(stage.stage_editor(Path("build/release"), self.destination, "-"), self.bundle)
        commands = [call.args[0] for call in run.call_args_list]
        self.assertEqual([command[:2] for command in commands], [
            ["cmake", "--install"],
            ["codesign", "--force"],
            ["codesign", "--verify"],
            ["codesign", "--display"],
        ])
        self.assertEqual(commands[0][-3:], ["Editor", "--prefix", str(self.destination)])
        self.assertEqual(commands[1][-3:], ["--entitlements", str(stage.ENTITLEMENTS), str(self.bundle)])
        self.assertTrue((self.bundle / "Contents/MacOS/HoroEditor").is_file())
        self.assertTrue(plistlib.loads(stage.ENTITLEMENTS.read_bytes())[stage.AUDIO_INPUT])

    def test_rejects_missing_usage_description_before_signing(self):
        del self.metadata["NSSpeechRecognitionUsageDescription"]
        with patch.object(stage.subprocess, "run", side_effect=self.run_tool) as run:
            with self.assertRaisesRegex(ValueError, "NSSpeechRecognitionUsageDescription"):
                stage.stage_editor(Path("build/release"), self.destination, "-")
        self.assertEqual(run.call_count, 1)

    def test_rejects_signature_without_audio_entitlement(self):
        self.signed_entitlements[stage.AUDIO_INPUT] = False
        with patch.object(stage.subprocess, "run", side_effect=self.run_tool):
            with self.assertRaisesRegex(ValueError, "audio-input"):
                stage.stage_editor(Path("build/release"), self.destination, "-")

    def test_refuses_to_sign_a_bundle_left_by_an_earlier_install(self):
        self.run_tool(["cmake", "--install"])
        previous = self.bundle / "Contents/MacOS/HoroEditor"
        previous.write_bytes(b"older editor")

        def no_new_install(command, **options):
            if command[0] == "cmake":
                return subprocess.CompletedProcess(command, 0)
            return self.run_tool(command, **options)

        with patch.object(stage.subprocess, "run", side_effect=no_new_install) as run:
            with self.assertRaisesRegex(ValueError, "already exists"):
                stage.stage_editor(Path("build/release"), self.destination, "-")
            run.assert_not_called()
        self.assertEqual(previous.read_bytes(), b"older editor")


if __name__ == "__main__":
    unittest.main()
