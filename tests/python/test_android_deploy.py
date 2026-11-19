"""Host operation, attribution and privacy tests for the Android deployment command."""

import contextlib
import io
import hashlib
import json
from pathlib import Path
import sys
import tempfile
import zipfile
import time
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scripts"))
import project

PROFILE = "qualification-debug"
APPLICATION = "org.horocore.packagequalification"
SERIAL = "emulator-5554"
TRANSPORT = "37"
DIGEST = "a" * 64
PACKAGE = "/data/app/~~hash/example==/base.apk"
LIFECYCLE = ("host-state lifecycleRevision=3 activity=1 state=2 process=1 window=5 presentation=8 "
             "factsRevision=5 extent=1280x720 nativeAvailable=1 ready=1 hasWindow=1 hasPresentation=1 "
             "attempts=2 presentAccepted=1")
FRAME = ("host-frame stage=present outcome=accepted attempts=2 presentAccepted=1 beginPending=0 "
         "executePending=0 presentPending=0 completion=unobserved")


class AndroidDeployTests(unittest.TestCase):
    def execute(self, override=None):
        """Run the real CLI orchestration against an exact scripted ADB transport."""
        _, profile = project.load_contract(PROFILE, ["arm64-v8a"])
        launch = ("shell", "am", "start", "-W", "--user", "10", "-n", f"{APPLICATION}/{APPLICATION}.PackageActivity")
        stop = ("shell", "am", "force-stop", "--user", "10", APPLICATION)
        replies = {
            ("devices", "-l"): f"List of devices attached\n{SERIAL} device product:probe transport_id:{TRANSPORT}",
            ("get-serialno",): SERIAL,
            ("get-state",): "device",
            ("shell", "am", "get-current-user"): "10",
            ("shell", "getprop", "ro.build.version.sdk"): "36",
            ("shell", "getprop", "ro.product.cpu.abi"): "arm64-v8a",
            ("shell", "pm", "path", "--user", "10", APPLICATION): f"package:{PACKAGE}",
            ("shell", "toybox", "sha256sum", PACKAGE): f"{DIGEST}  {PACKAGE}",
            launch: f"Status: ok\nLaunchState: COLD\nActivity: {APPLICATION}/.PackageActivity",
            stop: "",
            ("shell", "cmd", "package", "list", "packages", "--user", "10", "-U", APPLICATION):
                f"package:{APPLICATION} uid:1010200",
            ("shell", "pidof", APPLICATION): "123",
            ("shell", "toybox", "ps", "-p", "123", "-o", "UID,STIME:19"): "UID STIME\n1010200 2026-11-18 10:30:01",
            ("logcat", "--pid=123", "-v", "brief", "-t", "200", "-s", "HoroPackageQualification:I", "AndroidRuntime:E"):
                f"I/HoroPackageQualification( 123): {LIFECYCLE}\nI/HoroPackageQualification( 123): {FRAME}\n"
                "I/HoroPackageQualification( 123): ignored secret=/sdcard/personal\n"
                "E/AndroidRuntime( 999): FATAL EXCEPTION: unrelated-thread\n",
        }
        replies.update(override or {})
        commands = []
        running = False

        def run(command, deadline):
            nonlocal running
            if command[1:] == ["devices", "-l"]:
                args = tuple(command[1:])
            else:
                self.assertEqual(command[1:3], ["-t", TRANSPORT])
                args = tuple(command[3:])
            self.assertGreater(deadline, time.monotonic())
            commands.append(args)
            if args[:4] == ("install", "-r", "--user", "10"):
                self.assertEqual(str(Path(args[4])), str(Path("a package ü.apk")))
                return 0, "Performing Streamed Install\nSuccess"
            if args == ("shell", "pidof", APPLICATION) and not running and (not override or args not in override):
                return 1, ""
            response = replies[args]
            if isinstance(response, list):
                response = response.pop(0)
            if isinstance(response, BaseException):
                raise response
            if args == launch:
                running = True
            if args == stop and not isinstance(response, tuple):
                running = False
            return response if isinstance(response, tuple) else (0, response)

        stdout = io.StringIO()
        with patch.object(project, "deploy_artifact",
                          return_value=(Path("a package ü.apk"), profile, DIGEST, {"arm64-v8a"})), \
                patch.object(project, "deploy_process", side_effect=run), contextlib.redirect_stdout(stdout):
            status = project.main(["android-deploy", "--artifact", ".", "--profile", PROFILE, "--sdk", ".",
                                   "--device", SERIAL, "--timeout", "30"])
        return status, json.loads(stdout.getvalue()), commands

    def test_scopes_install_launch_process_and_reports_only_typed_facts(self):
        status, report, commands = self.execute()
        self.assertEqual(status, 0)
        self.assertEqual(report["status"], "complete")
        self.assertEqual(report["artifact"]["apkSha256"], DIGEST)
        self.assertEqual(report["device"], {"androidApi": 36, "abi": "arm64-v8a"})
        self.assertEqual(report["observed"]["renderer"]["presentAccepted"], 1)
        self.assertEqual(report["observed"]["lifecycle"]["window"], 5)
        self.assertFalse(report["observed"]["crashObserved"])
        self.assertLess(next(i for i, cmd in enumerate(commands) if "sha256sum" in cmd),
                        next(i for i, cmd in enumerate(commands) if "start" in cmd))
        for denied in (SERIAL, PACKAGE, "personal", "unrelated-thread", "1010200"):
            self.assertNotIn(denied, json.dumps(report))

    def test_rejects_stale_installation_before_launch(self):
        status, report, commands = self.execute({
            ("shell", "toybox", "sha256sum", PACKAGE): "b" * 64 + f"  {PACKAGE}"})
        self.assertEqual(status, 1)
        self.assertEqual(report["error"], "installed_artifact_stale")
        self.assertNotIn("start", {arg for cmd in commands for arg in cmd})

    def test_untrusted_package_path_is_never_passed_to_remote_shell(self):
        status, report, commands = self.execute({
            ("shell", "pm", "path", "--user", "10", APPLICATION): "package:/data/../app/base.apk"})
        self.assertEqual((status, report["error"]), (1, "installed_artifact_unverifiable"))
        self.assertFalse(any("sha256sum" in cmd for cmd in commands))

    def test_wait_cancellation_and_unplug_are_partial(self):
        for response, code, error in (
            (KeyboardInterrupt(), 130, "cancelled"),
            ((1, "error: device serial SECRET disconnected"), 1, "device_disconnected"),
        ):
            with self.subTest(error=error):
                status, report, commands = self.execute({("get-state",): response})
                self.assertEqual((status, report["status"], report["error"]), (code, "partial", error))
                self.assertNotIn("SECRET", json.dumps(report))
                self.assertEqual(commands, [("devices", "-l"), ("get-serialno",), ("get-state",)])

    def test_refuses_another_users_process_before_logcat(self):
        status, report, commands = self.execute({("shell", "toybox", "ps", "-p", "123", "-o", "UID,STIME:19"): "UID STIME\n10200 2026-11-18 10:30:01"})
        self.assertEqual((status, report["error"]), (1, "application_process_identity_mismatch"))
        self.assertFalse(any(cmd[0] == "logcat" for cmd in commands))

    def test_crash_survives_partial_capture_without_raw_details(self):
        command = ("logcat", "--pid=123", "-v", "brief", "-t", "200", "-s", "HoroPackageQualification:I", "AndroidRuntime:E")
        status, report, _ = self.execute({command: "E/AndroidRuntime( 123): FATAL EXCEPTION: personal-thread-name"})
        self.assertEqual((status, report["status"], report["error"]), (1, "partial", "application_crashed"))
        self.assertTrue(report["observed"]["crashObserved"])
        self.assertNotIn("personal-thread-name", json.dumps(report))

    def test_failed_capture_retires_only_the_attributed_launch(self):
        command = ("logcat", "--pid=123", "-v", "brief", "-t", "200", "-s", "HoroPackageQualification:I", "AndroidRuntime:E")
        for failure, error in (("I/HoroPackageQualification( 123): ignored room=data", "application_evidence_unavailable"),
                               (project.AndroidDeployError("deadline_exceeded"), "deadline_exceeded")):
            with self.subTest(error=error):
                status, report, commands = self.execute({command: failure})
                self.assertEqual((status, report["error"], report["cleanup"]), (1, error, {"status": "stopped"}))
                self.assertIn(("shell", "am", "force-stop", "--user", "10", APPLICATION), commands)
                self.assertNotIn("room=data", json.dumps(report))

    def test_preexisting_process_is_never_replaced_or_stopped(self):
        status, report, commands = self.execute({("shell", "pidof", APPLICATION): "123"})
        self.assertEqual((status, report["error"]), (1, "application_already_running"))
        self.assertNotIn("cleanup", report)
        self.assertFalse(any("install" in cmd or "start" in cmd or "force-stop" in cmd for cmd in commands))

    def test_replaced_process_is_never_stopped(self):
        query = ("shell", "toybox", "ps", "-p", "123", "-o", "UID,STIME:19")
        status, report, commands = self.execute({query: [
            "UID STIME\n1010200 2026-11-18 10:30:01",
            "UID STIME\n1010200 2026-11-18 10:30:02",
            "UID STIME\n1010200 2026-11-18 10:30:02",
        ]})
        self.assertEqual((status, report["error"]), (1, "application_process_changed"))
        self.assertEqual(report["cleanup"], {"status": "unconfirmed", "reason": "application_process_changed"})
        self.assertFalse(any("force-stop" in cmd for cmd in commands))

    def test_disconnected_transport_never_reselects_target(self):
        command = ("logcat", "--pid=123", "-v", "brief", "-t", "200", "-s", "HoroPackageQualification:I", "AndroidRuntime:E")
        status, report, commands = self.execute({
            command: (1, "error: device disconnected"),
            ("get-serialno",): [SERIAL, (1, "error: device not found")],
        })
        self.assertEqual((status, report["error"]), (1, "device_disconnected"))
        self.assertEqual(report["cleanup"], {"status": "unconfirmed", "reason": "device_disconnected"})
        self.assertFalse(any("force-stop" in cmd for cmd in commands))
        self.assertEqual(commands.count(("devices", "-l")), 1)

    def test_ambiguous_activity_start_is_not_attributed(self):
        launch = ("shell", "am", "start", "-W", "--user", "10", "-n", f"{APPLICATION}/{APPLICATION}.PackageActivity")
        status, report, commands = self.execute({
            launch: f"Status: ok\nLaunchState: WARM\nActivity: {APPLICATION}/.PackageActivity",
        })
        self.assertEqual((status, report["error"]), (1, "launch_unconfirmed"))
        self.assertEqual(report["cleanup"], {"status": "unconfirmed", "reason": "launch_process_unattributed"})
        self.assertFalse(any("force-stop" in cmd or "logcat" in cmd for cmd in commands))

    def test_package_bytes_manifest_and_signature_are_verified_before_adb(self):
        tools, profile = project.load_contract(PROFILE, ["arm64-v8a"])
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            sdk = root / "sdk"
            build_tools = sdk / "build-tools" / tools["buildTools"]
            signer = build_tools / "lib" / "apksigner.jar"
            signer.parent.mkdir(parents=True)
            signer.write_bytes(b"fixture")
            (build_tools / "source.properties").write_text(f"Pkg.Revision={tools['buildTools']}\n", encoding="utf-8")
            artifact = root / "artifact"
            artifact.mkdir()
            stage = root / "stage"
            stage.mkdir()
            payload = b"fixture payload"
            digest = hashlib.sha256(payload).hexdigest()
            declared = {
                "schema": 1, "profile": profile, "toolchain": tools,
                "abis": {"arm64-v8a": [{"name": name, "sha256": digest} for name in profile["nativeLibraries"]]},
                "assets": [{"name": name, "sha256": digest} for name in profile["assets"]],
            }
            package = artifact / "signed.apk"
            with zipfile.ZipFile(package, "w") as archive:
                for name in ("classes.dex", "AndroidManifest.xml"):
                    archive.writestr(name, payload)
                for name in profile["nativeLibraries"]:
                    archive.writestr("lib/arm64-v8a/" + name, payload)
                for name in profile["assets"]:
                    archive.writestr("assets/" + name, payload)
                archive.writestr(project.PROVENANCE_ASSET, project.canonical_json(declared))
            (artifact / "provenance.json").write_bytes(project.canonical_json(declared))
            inspection = project.inspect_package(package, declared) | {"signed": True}
            (artifact / "inspection.json").write_bytes(project.canonical_json(inspection))
            badging = "\n".join([
                f"package: name='{APPLICATION}' versionCode='1' versionName='0.2.0'",
                "sdkVersion:'29'", "targetSdkVersion:'36'",
                f"launchable-activity: name='{APPLICATION}.PackageActivity'",
                *(f"uses-permission: name='{name}'" for name in profile["permissions"]),
            ])
            deadline = time.monotonic() + 10
            with patch.object(project, "deploy_process", side_effect=[(0, ""), (0, badging)]) as run:
                staged, _, verified, _ = project.deploy_artifact(artifact, stage, sdk, PROFILE, "java", deadline)
                self.assertEqual(staged.read_bytes(), package.read_bytes())
                self.assertEqual(verified, inspection["apkSha256"])
                self.assertEqual(run.call_args_list[0].args[0], ["java", "-jar", str(signer), "verify", str(staged)])
                self.assertEqual(run.call_args_list[1].args[0][1:], ["dump", "badging", str(staged)])
            changed = inspection | {"apkSha256": "0" * 64}
            (artifact / "inspection.json").write_bytes(project.canonical_json(changed))
            with patch.object(project, "deploy_process") as run, self.assertRaisesRegex(
                    project.AndroidDeployError, "artifact_inspection_mismatch"):
                project.deploy_artifact(artifact, stage, sdk, PROFILE, "java", deadline)
            run.assert_not_called()

    def test_process_output_and_time_are_bounded(self):
        self.assertEqual(project.deploy_process([sys.executable, "-c", "print('ready')"], time.monotonic() + 5),
                         (0, "ready\n" if sys.platform != "win32" else "ready\r\n"))
        with self.assertRaisesRegex(project.AndroidDeployError, "host_output_limit"):
            project.deploy_process([sys.executable, "-c", "import sys; sys.stdout.write('x'*1000000)"], time.monotonic() + 5)
        started = time.monotonic()
        with self.assertRaisesRegex(project.AndroidDeployError, "deadline_exceeded"):
            project.deploy_process([sys.executable, "-c", "import time; time.sleep(15)"], started + 0.3)
        self.assertLess(time.monotonic() - started, 4)
