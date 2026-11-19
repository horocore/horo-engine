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
PROVENANCE_DIGEST = "b" * 64
SOURCE_REVISION = "c" * 40
PACKAGE = "/data/app/~~hash/example==/base.apk"
LIFECYCLE = ("host-state lifecycleRevision=3 activity=1 state=2 process=1 window=5 presentation=8 "
             "factsRevision=5 extent=1280x720 nativeAvailable=1 ready=1 hasWindow=1 hasPresentation=1 "
             "attempts=2 presentAccepted=1")
FRAME = ("host-frame stage=present outcome=accepted attempts=2 presentAccepted=1 beginPending=0 "
         "executePending=0 presentPending=0 completion=unobserved")
OPENXR = "host-openxr abi=arm64-v8a stage=discovered result=0 runtimeMajor=1 runtimeMinor=1 runtimePatch=63"
OPENXR_OPTIONS = ("--host", "linux-x64", "--connection", "emulator", "--runtime", "openxr")
LAUNCH_NONCE = "d" * 32
LAUNCH_MARKER = project.DEPLOY_LAUNCH_MARKER + LAUNCH_NONCE
DEVICE_CLOCK = ("shell", "toybox", "date", "+%s.%N")
UID_QUERY = ("shell", "toybox", "ps", "-p", "123", "-o", "UID")
STAT_QUERY = ("shell", "toybox", "cat", "/proc/123/stat")
LOGCAT = ("logcat", "--pid=123", "-v", "time,epoch,nsec,uid", "-t", "200", "-s",
          "HoroPackageQualification:I", "AndroidRuntime:E")
START = "1795000000.100000000"
LOG_TIME = "1795000000.105000000"
END = "1795000000.110000000"


def log_line(message, stamp=LOG_TIME, uid="1010200", pid=123, tag="I/HoroPackageQualification"):
    return f" {stamp} {tag}({uid}: {pid}): {message}\n"


def proc_stat(start_ticks=123456):
    # The kernel allows whitespace and ')' in comm, so the numeric fields begin after the closing delimiter.
    return f"123 (probe ) main) S {'0 ' * 18}{start_ticks} 0"


def launch_command(application):
    return ("shell", "am", "start", "-W", "--user", "10", "-n", f"{application}/{application}.PackageActivity",
            "--es", project.DEPLOY_LAUNCH_EXTRA, LAUNCH_NONCE)


class AndroidDeployTests(unittest.TestCase):
    def execute(self, override=None, options=(), profile_name=PROFILE, *, launch_reached_device=False):
        """Run the real CLI orchestration against an exact scripted ADB transport."""
        _, profile = project.load_contract(profile_name, ["arm64-v8a"])
        application = profile["applicationId"]
        launch = launch_command(application)
        stop = ("shell", "am", "force-stop", "--user", "10", application)
        replies = {
            ("devices", "-l"): f"List of devices attached\n{SERIAL} device product:probe transport_id:{TRANSPORT}",
            ("get-serialno",): SERIAL,
            ("get-state",): "device",
            DEVICE_CLOCK: [START, END],
            ("shell", "am", "get-current-user"): "10",
            ("shell", "getprop", "ro.build.version.sdk"): "36",
            ("shell", "getprop", "ro.product.cpu.abi"): "arm64-v8a",
            ("shell", "pm", "path", "--user", "10", application): f"package:{PACKAGE}",
            ("shell", "toybox", "sha256sum", PACKAGE): f"{DIGEST}  {PACKAGE}",
            launch: f"Status: ok\nLaunchState: COLD\nActivity: {application}/.PackageActivity",
            stop: "",
            ("shell", "cmd", "package", "list", "packages", "--user", "10", "-U", application):
                f"package:{application} uid:1010200",
            ("shell", "pidof", application): "123",
            UID_QUERY: "UID\n1010200",
            STAT_QUERY: proc_stat(),
            LOGCAT: (log_line(LIFECYCLE) + log_line(FRAME) + log_line("ignored secret=/sdcard/personal")
                     + log_line("FATAL EXCEPTION: unrelated-thread", pid=999, tag="E/AndroidRuntime")),
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
            if args == ("shell", "pidof", application) and not running and (not override or args not in override):
                return 1, ""
            response = replies[args]
            if isinstance(response, list):
                response = response.pop(0)
            if args == launch and (launch_reached_device or not isinstance(response, BaseException)):
                running = True
            if isinstance(response, BaseException):
                raise response
            if args == stop and not isinstance(response, tuple):
                running = False
            return response if isinstance(response, tuple) else (0, response)

        stdout = io.StringIO()
        artifact = project.VerifiedAndroidArtifact(Path("a package ü.apk"), profile, DIGEST, PROVENANCE_DIGEST, SOURCE_REVISION,
                                                   frozenset({"arm64-v8a"}))
        with patch.object(project, "deploy_artifact", return_value=artifact), \
                patch.object(project, "android_deploy_host", return_value="linux-x64"), \
                patch.object(project, "deploy_process", side_effect=run), \
                patch.object(project.secrets, "token_hex", return_value=LAUNCH_NONCE), \
                patch.object(project.time, "monotonic_ns", side_effect=[100_000_000, 101_000_000, 109_000_000, 111_000_000]), \
                contextlib.redirect_stdout(stdout):
            status = project.main(["android-deploy", "--artifact", ".", "--profile", profile_name, "--sdk", ".",
                                   "--device", SERIAL, "--timeout", "30", *options])
        return status, json.loads(stdout.getvalue()), commands

    def test_scopes_install_launch_process_and_reports_only_typed_facts(self):
        status, report, commands = self.execute()
        self.assertEqual(status, 0)
        self.assertEqual(report["status"], "complete")
        self.assertEqual(report["artifact"]["apkSha256"], DIGEST)
        self.assertEqual(report["artifact"]["provenanceSha256"], PROVENANCE_DIGEST)
        self.assertEqual(report["artifact"]["sourceRevision"], SOURCE_REVISION)
        self.assertEqual(report["device"], {"androidApi": 36, "abi": "arm64-v8a"})
        self.assertEqual(report["observed"]["renderer"]["presentAccepted"], 1)
        self.assertEqual(report["observed"]["lifecycle"]["window"], 5)
        self.assertFalse(report["observed"]["crashObserved"])
        self.assertEqual(report["qualification"], {
            "host": {"requested": None, "observed": "linux-x64"},
            "connection": {"requested": None, "observed": "emulator"},
            "runtime": {"requested": "none", "status": "not_requested"},
            "physicalXrQualification": "not_run",
        })
        self.assertLess(next(i for i, cmd in enumerate(commands) if "sha256sum" in cmd),
                        next(i for i, cmd in enumerate(commands) if "start" in cmd))
        for denied in (SERIAL, PACKAGE, "personal", "unrelated-thread", "1010200", START, LOG_TIME, END):
            self.assertNotIn(denied, json.dumps(report))

    def test_explicit_host_and_connection_match_the_selected_target(self):
        status, report, _ = self.execute(options=("--host", "linux-x64", "--connection", "emulator", "--runtime", "none"))
        self.assertEqual(status, 0)
        self.assertEqual(report["qualification"]["host"], {"requested": "linux-x64", "observed": "linux-x64"})
        self.assertEqual(report["qualification"]["connection"], {"requested": "emulator", "observed": "emulator"})

    def test_host_or_connection_mismatch_never_installs(self):
        status, report, commands = self.execute(options=("--host", "windows-x64"))
        self.assertEqual((status, report["stage"], report["error"]), (1, "selection", "deployment_host_mismatch"))
        self.assertEqual(commands, [])
        status, report, commands = self.execute(options=("--connection", "usb"))
        self.assertEqual((status, report["stage"], report["error"]), (1, "device", "device_connection_mismatch"))
        self.assertEqual(report["qualification"]["connection"], {"requested": "usb", "observed": "emulator"})
        self.assertEqual(commands, [("devices", "-l"), ("get-serialno",)])

    def test_openxr_requires_exact_intent_and_static_profile_admission_before_device_use(self):
        status, report, commands = self.execute(options=("--runtime", "openxr"))
        self.assertEqual((status, report["error"]), (1, "xr_target_selection_incomplete"))
        self.assertEqual(commands, [])
        status, report, commands = self.execute(options=("--runtime", "openxr", "--host", "linux-x64", "--connection", "usb"))
        self.assertEqual((status, report["stage"], report["error"]), (1, "runtime", "android_openxr_route_unavailable"))
        self.assertEqual(report["qualification"]["runtime"], {"requested": "openxr", "status": "unsupported"})
        self.assertEqual(report["qualification"]["profile"], PROFILE)
        self.assertEqual(report["qualification"]["physicalXrQualification"], "not_run")
        self.assertNotIn("artifact", report)
        self.assertNotIn("device", report)
        self.assertEqual(commands, [])

    def test_connection_modes_only_classify_supported_adb_inventory_forms(self):
        for serial, details, expected in (
            ("AB123", ["usb:1-2"], "usb"),
            ("127.0.0.1:5555", [], "tcpip"),
            ("[::1]:5555", [], "tcpip"),
            ("adb-foobar-q7dQub._adb-tls-connect._tcp", [], "tcpip"),
            ("AB123", [], "unknown"),
            ("emulator-5554", ["usb:1-2"], "unknown"),
            ("127.0.0.1:99999", [], "unknown"),
        ):
            with self.subTest(serial=serial, details=details):
                self.assertEqual(project.adb_connection_mode(serial, details), expected)

    def test_openxr_discovery_launch_is_attributed_redacted_and_stopped(self):
        name = project.OPENXR_DEVELOPMENT_PROFILE
        _, profile = project.load_contract(name, ["arm64-v8a"])
        log = log_line(OPENXR) + log_line("host-openxr raw=private", pid=999)
        status, report, commands = self.execute({LOGCAT: log}, OPENXR_OPTIONS, name)
        self.assertEqual((status, report["status"], report["stage"]), (0, "diagnostic_complete", "diagnostics"))
        self.assertEqual(report["artifact"]["apkSha256"], DIGEST)
        self.assertEqual(report["artifact"]["openxr"], profile["openxrProfile"])
        self.assertEqual(report["qualification"]["runtime"],
                         {"requested": "openxr", "selection": "system-default", "status": "cold_system_discovered"})
        self.assertEqual(report["qualification"]["physicalXrQualification"], "not_run")
        self.assertEqual(report["observed"]["openxr"]["runtimeVersion"], {"major": 1, "minor": 1, "patch": 63})
        self.assertEqual(report["cleanup"], {"status": "stopped"})
        self.assertIn(("shell", "am", "force-stop", "--user", "10", profile["applicationId"]), commands)
        self.assertNotIn("private", json.dumps(report))

    def test_openxr_discovery_rejects_ambiguous_or_wrong_abi_evidence_and_stops(self):
        name = project.OPENXR_DEVELOPMENT_PROFILE
        for raw, reason in ((log_line(OPENXR) * 2, "openxr_diagnostic_ambiguous"),
                            (log_line(OPENXR.replace("arm64-v8a", "x86_64")), "openxr_diagnostic_abi_mismatch"),
                            (log_line(OPENXR.replace("result=0", "result=-1")), "openxr_diagnostic_invalid")):
            with self.subTest(reason=reason):
                status, report, _ = self.execute({LOGCAT: raw}, OPENXR_OPTIONS, name)
                self.assertEqual((status, report["error"]), (1, reason))
                self.assertEqual(report["cleanup"], {"status": "stopped"})
                self.assertEqual(report["qualification"]["physicalXrQualification"], "not_run")

    def test_openxr_cold_discovery_native_failure_keeps_typed_stage(self):
        raw = log_line("host-openxr abi=arm64-v8a stage=extension-admission result=-51 runtimeMajor=0 runtimeMinor=0 runtimePatch=0")
        status, report, _ = self.execute({LOGCAT: raw}, OPENXR_OPTIONS, project.OPENXR_DEVELOPMENT_PROFILE)
        self.assertEqual((status, report["error"]), (1, "openxr_cold_discovery_failed"))
        self.assertEqual(report["observed"]["openxr"], {"abi": "arm64-v8a", "stage": "extension-admission", "result": -51})
        self.assertEqual(report["cleanup"], {"status": "stopped"})

    def test_explicit_connection_fails_closed_for_contradictory_inventory(self):
        inventory = f"List of devices attached\n{SERIAL} device usb:1-2 transport_id:{TRANSPORT}"
        status, report, commands = self.execute({("devices", "-l"): inventory}, options=("--connection", "emulator"))
        self.assertEqual((status, report["error"]), (1, "device_connection_unverifiable"))
        self.assertEqual(report["qualification"]["connection"], {"requested": "emulator", "observed": "unknown"})
        self.assertEqual(commands, [("devices", "-l"), ("get-serialno",)])

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
        status, report, commands = self.execute({UID_QUERY: "UID\n10200"})
        self.assertEqual((status, report["error"]), (1, "application_process_identity_mismatch"))
        self.assertFalse(any(cmd[0] == "logcat" for cmd in commands))

    def test_crash_survives_partial_capture_without_raw_details(self):
        status, report, _ = self.execute({LOGCAT: log_line("FATAL EXCEPTION: personal-thread-name", tag="E/AndroidRuntime")})
        self.assertEqual((status, report["status"], report["error"]), (1, "partial", "application_crashed"))
        self.assertTrue(report["observed"]["crashObserved"])
        self.assertNotIn("personal-thread-name", json.dumps(report))

    def test_failed_capture_retires_only_the_attributed_launch(self):
        for failure, error in ((log_line("ignored room=data"), "application_evidence_unavailable"),
                               (project.AndroidDeployError("deadline_exceeded"), "deadline_exceeded")):
            with self.subTest(error=error):
                status, report, commands = self.execute({LOGCAT: failure})
                self.assertEqual((status, report["error"], report["cleanup"]), (1, error, {"status": "stopped"}))
                self.assertIn(("shell", "am", "force-stop", "--user", "10", APPLICATION), commands)
                self.assertNotIn("room=data", json.dumps(report))

    def test_preexisting_process_is_never_replaced_or_stopped(self):
        status, report, commands = self.execute({("shell", "pidof", APPLICATION): "123"})
        self.assertEqual((status, report["error"]), (1, "application_already_running"))
        self.assertNotIn("cleanup", report)
        self.assertFalse(any("install" in cmd or "start" in cmd or "force-stop" in cmd for cmd in commands))

    def test_replaced_process_is_never_stopped(self):
        status, report, commands = self.execute({STAT_QUERY: [proc_stat()] * 2 + [proc_stat(123457)] * 4})
        self.assertEqual((status, report["error"]), (1, "application_process_changed"))
        self.assertEqual(report["cleanup"], {"status": "unconfirmed", "reason": "application_process_changed"})
        self.assertFalse(any("force-stop" in cmd for cmd in commands))

    def test_disconnected_transport_never_reselects_target(self):
        status, report, commands = self.execute({
            LOGCAT: (1, "error: device disconnected"),
            ("get-serialno",): [SERIAL, (1, "error: device not found")],
        })
        self.assertEqual((status, report["error"]), (1, "device_disconnected"))
        self.assertEqual(report["cleanup"], {"status": "unconfirmed", "reason": "device_disconnected"})
        self.assertFalse(any("force-stop" in cmd for cmd in commands))
        self.assertEqual(commands.count(("devices", "-l")), 1)

    def test_buffered_records_with_reused_pid_or_foreign_uid_cannot_supply_evidence(self):
        stale = log_line(LIFECYCLE, stamp=START)
        stale += log_line("FATAL EXCEPTION: old process", stamp="1795000000.099999999", tag="E/AndroidRuntime")
        stale += log_line(FRAME, uid="10200")
        stale += log_line(FRAME, stamp="1795000000.110000001")
        status, report, _ = self.execute({LOGCAT: stale})
        self.assertEqual((status, report["error"]), (1, "application_evidence_unavailable"))
        self.assertEqual(report["observed"], {"crashObserved": False})

    def test_latest_matching_marker_survives_out_of_order_buffer_records(self):
        log = log_line(LIFECYCLE, stamp=END, uid="u10_a200")
        log += log_line(LIFECYCLE.replace("lifecycleRevision=3", "lifecycleRevision=2"))
        status, report, _ = self.execute({LOGCAT: log})
        self.assertEqual(status, 0)
        self.assertEqual(report["observed"]["lifecycle"]["lifecycleRevision"], 3)
        self.assertNotIn("u10_a200", json.dumps(report))

    def test_device_clock_discontinuity_or_malformed_clock_cannot_supply_evidence(self):
        for final, expected in ((START, "device_log_clock_changed"),
                                ("1795000000.111000001", "device_log_clock_changed"),
                                ("1795000000.%N", "device_log_clock_unavailable")):
            with self.subTest(final=final):
                status, report, _ = self.execute({DEVICE_CLOCK: [START, final]})
                self.assertEqual((status, report["error"], report["cleanup"]), (1, expected, {"status": "stopped"}))
                self.assertNotIn("observed", report)

    def test_replacement_cannot_claim_a_matching_buffered_crash(self):
        status, report, _ = self.execute({
            STAT_QUERY: [proc_stat()] * 2 + [proc_stat(123457)] * 4,
            LOGCAT: log_line("FATAL EXCEPTION: replaced process", tag="E/AndroidRuntime"),
        })
        self.assertEqual((status, report["error"]), (1, "application_process_changed"))
        self.assertNotIn("observed", report)

    def test_process_incarnation_changes_during_uid_observation(self):
        status, report, commands = self.execute({STAT_QUERY: [proc_stat()] + [proc_stat(123457)] * 3})
        self.assertEqual((status, report["error"]), (1, "application_process_changed"))
        self.assertEqual(report["cleanup"], {"status": "unconfirmed", "reason": "launch_process_unattributed"})
        self.assertFalse(any("force-stop" in cmd for cmd in commands))

    def test_ambiguous_activity_start_is_not_attributed(self):
        status, report, commands = self.execute({
            launch_command(APPLICATION): f"Status: ok\nLaunchState: WARM\nActivity: {APPLICATION}/.PackageActivity",
            LOGCAT: log_line(LAUNCH_MARKER),
        })
        self.assertEqual((status, report["error"]), (1, "launch_unconfirmed"))
        self.assertEqual(report["cleanup"], {"status": "unconfirmed", "reason": "launch_process_unattributed"})
        self.assertFalse(any("force-stop" in cmd or "logcat" in cmd for cmd in commands))

    def test_timed_out_launch_stops_only_the_process_with_its_private_receipt(self):
        status, report, commands = self.execute({
            launch_command(APPLICATION): project.AndroidDeployError("deadline_exceeded"),
            LOGCAT: log_line(LAUNCH_MARKER),
        }, launch_reached_device=True)
        self.assertEqual((status, report["stage"], report["error"]), (1, "launch", "deadline_exceeded"))
        self.assertEqual(report["cleanup"], {"status": "stopped"})
        self.assertTrue(any("force-stop" in cmd for cmd in commands))
        self.assertNotIn("observed", report)
        self.assertNotIn(LAUNCH_NONCE, json.dumps(report))

    def test_interrupted_launch_with_no_started_process_leaves_cleanup_unconfirmed(self):
        status, report, commands = self.execute({launch_command(APPLICATION): KeyboardInterrupt()})
        self.assertEqual((status, report["error"]), (130, "cancelled"))
        self.assertEqual(report["cleanup"], {"status": "unconfirmed", "reason": "launch_process_unattributed"})
        self.assertFalse(any("force-stop" in cmd for cmd in commands))

    def test_cleanup_requires_one_fresh_receipt_from_the_same_uid_and_pid(self):
        for log in (log_line(LAUNCH_MARKER, stamp=START), log_line(LAUNCH_MARKER, uid="10200"),
                    log_line(LAUNCH_MARKER, pid=999), log_line(LAUNCH_MARKER) * 2, log_line(LIFECYCLE)):
            with self.subTest(log=log):
                status, report, commands = self.execute({
                    launch_command(APPLICATION): KeyboardInterrupt(), LOGCAT: log,
                }, launch_reached_device=True)
                self.assertEqual((status, report["error"]), (130, "cancelled"))
                self.assertEqual(report["cleanup"], {"status": "unconfirmed", "reason": "launch_process_unattributed"})
                self.assertFalse(any("force-stop" in cmd for cmd in commands))

    def test_recovered_process_is_rechecked_before_stop(self):
        status, report, commands = self.execute({
            launch_command(APPLICATION): KeyboardInterrupt(),
            LOGCAT: log_line(LAUNCH_MARKER),
            STAT_QUERY: [proc_stat()] * 2 + [proc_stat(123457)] * 2,
        }, launch_reached_device=True)
        self.assertEqual((status, report["error"]), (130, "cancelled"))
        self.assertEqual(report["cleanup"], {"status": "unconfirmed", "reason": "application_process_changed"})
        self.assertFalse(any("force-stop" in cmd for cmd in commands))

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
            dependencies = project.canonical_json([{"group": "androidx.core", "module": "core", "version": "1.13.1", "sha256": DIGEST}])
            declared = {
                "schema": 1, "profile": profile, "toolchain": tools,
                "sourceRevision": SOURCE_REVISION,
                "javaDependencyLockSha256": project.digest(project.ROOT / "android/gradle/gradle.lockfile"),
                "abis": {"arm64-v8a": [{"name": name, "sha256": digest} for name in profile["nativeLibraries"]]},
                "assets": [{"name": name, "sha256": digest} for name in profile["assets"]]
                          + [{"name": project.JAVA_DEPENDENCIES_ASSET, "sha256": hashlib.sha256(dependencies).hexdigest()}],
            }
            package = artifact / "signed.apk"
            with zipfile.ZipFile(package, "w") as archive:
                for name in ("classes.dex", "AndroidManifest.xml"):
                    archive.writestr(name, payload)
                for name in profile["nativeLibraries"]:
                    archive.writestr("lib/arm64-v8a/" + name, payload)
                for name in profile["assets"]:
                    archive.writestr("assets/" + name, payload)
                archive.writestr("assets/" + project.JAVA_DEPENDENCIES_ASSET, dependencies)
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
                verified = project.deploy_artifact(artifact, stage, sdk, PROFILE, "java", deadline)
                self.assertEqual(verified.path.read_bytes(), package.read_bytes())
                self.assertEqual(verified.apk_sha256, inspection["apkSha256"])
                self.assertEqual(verified.provenance_sha256, inspection["provenanceSha256"])
                self.assertEqual(verified.source_revision, SOURCE_REVISION)
                self.assertEqual(run.call_args_list[0].args[0], ["java", "-jar", str(signer), "verify", str(verified.path)])
                self.assertEqual(run.call_args_list[1].args[0][1:], ["dump", "badging", str(verified.path)])
            changed = inspection | {"apkSha256": "0" * 64}
            (artifact / "inspection.json").write_bytes(project.canonical_json(changed))
            with patch.object(project, "deploy_process") as run, self.assertRaisesRegex(
                    project.AndroidDeployError, "artifact_inspection_mismatch"):
                project.deploy_artifact(artifact, stage, sdk, PROFILE, "java", deadline)
            run.assert_not_called()
            for mutation, error in (({"javaDependencyLockSha256": "0" * 64}, "artifact_dependency_lock_stale"),
                                    ({"sourceRevision": "private local path"}, "artifact_source_revision_unavailable"),
                                    ({"assets": declared["assets"][:-1]}, "artifact_contract_mismatch")):
                with self.subTest(error=error):
                    (artifact / "provenance.json").write_bytes(project.canonical_json(declared | mutation))
                    with patch.object(project, "deploy_process") as run, self.assertRaisesRegex(project.AndroidDeployError, error):
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
