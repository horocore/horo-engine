"""Host operation, attribution and privacy tests for the Android deployment command."""

import contextlib
import io
import hashlib
import json
import os
from pathlib import Path
import sys
import tempfile
import zipfile
import time
import unittest
from unittest.mock import patch
from types import SimpleNamespace

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scripts"))
import project
from qualify_xr_android import check_android_preflight, validate_android_preflight

PROFILE = "qualification-debug"
VULKAN_PROFILE = "qualification-vulkan-development"
APPLICATION = "org.horocore.packagequalification"
SERIAL = "emulator-5554"
TRANSPORT = "37"
DIGEST = "a" * 64
PROVENANCE_DIGEST = "b" * 64
SOURCE_REVISION = "c" * 40
HOST_IMAGE_DIGEST = "9" * 64
LOADER_IMAGE_DIGEST = "8" * 64
DEVICE_BUILD = "example/headset/model:14/build/incremental:user/release-keys"
DEVICE_BUILD_DIGEST = hashlib.sha256(DEVICE_BUILD.encode("ascii")).hexdigest()
DEVICE_BUILD_QUERY = ("shell", "getprop", "ro.build.fingerprint")
PACKAGE = "/data/app/~~hash/example==/base.apk"
LIFECYCLE = ("host-state lifecycleRevision=3 activity=1 state=2 process=1 window=5 presentation=8 "
             "factsRevision=5 extent=1280x720 nativeAvailable=1 ready=1 hasWindow=1 hasPresentation=1 "
             "attempts=2 presentAccepted=1")
FRAME = ("host-frame stage=present outcome=accepted attempts=2 presentAccepted=1 beginPending=0 "
         "executePending=0 presentPending=0 completion=unobserved")
OPENXR = "host-openxr abi=arm64-v8a stage=discovered result=0 runtimeMajor=1 runtimeMinor=1 runtimePatch=63"
ENTRY = project.DEPLOY_ENTRY_MARKER
OPENXR_OPTIONS = ("--host", "linux-x64", "--connection", "emulator", "--runtime", "openxr")
LAUNCH_NONCE = "d" * 32
LAUNCH_MARKER = f"{project.DEPLOY_LAUNCH_MARKER}{LAUNCH_NONCE} startTicks=123456"
DEADLINE_MARKER = f"{project.DEPLOY_DEADLINE_MARKER}{LAUNCH_NONCE} startTicks=123456 milliseconds=1000"
LAUNCH_CRASH_MARKER = f"{project.DEPLOY_LAUNCH_CRASH_MARKER}{LAUNCH_NONCE} startTicks=123456"
DEVICE_CLOCK = ("shell", "toybox", "date", "+%s.%N")
UID_QUERY = ("shell", "toybox", "ps", "-p", "123", "-o", "UID")
STAT_QUERY = ("shell", "toybox", "cat", "/proc/123/stat")
LOGCAT = ("logcat", "--pid=123", "-v", "time,epoch,nsec,uid", "-t", "200", "-s",
          "HoroPackageQualification:I", "AndroidRuntime:E")
QUALIFICATION_LOGCAT = ("logcat", "-v", "time,epoch,nsec,uid", "-t", "200", "-s", "HoroPackageQualification:I")
START = "1795000000.100000000"
LAUNCH_TIME = "1795000000.104000000"
DEADLINE_TIME = "1795000000.104500000"
LOG_TIME = "1795000000.105000000"
END = "1795000000.110000000"


def log_line(message, stamp=LOG_TIME, uid="1010200", pid=123, tag="I/HoroPackageQualification"):
    return f" {stamp} {tag}({uid}: {pid}): {message}\n"


def proc_stat(start_ticks=123456):
    # The kernel allows whitespace and ')' in comm, so the numeric fields begin after the closing delimiter.
    return f"123 (probe ) main) S {'0 ' * 18}{start_ticks} 0"


def launch_command(application, *, openxr=False):
    command = ("shell", "am", "start", "-W", "--user", "10", "-n", f"{application}/{application}.PackageActivity",
               "--es", project.DEPLOY_LAUNCH_EXTRA, LAUNCH_NONCE)
    return command + ("--ei", project.DEPLOY_DEADLINE_EXTRA, "1000") if openxr else command


class AndroidDeployTests(unittest.TestCase):
    def execute(self, override=None, options=(), profile_name=PROFILE, *, launch_reached_device=False, launch_receipt=None, serial=SERIAL):
        """Run the real CLI orchestration against an exact scripted ADB transport."""
        _, profile = project.load_contract(profile_name, ["arm64-v8a"])
        application = profile["applicationId"]
        openxr = "openxrProfile" in profile
        launch = launch_command(application, openxr=openxr)
        stop = ("shell", "am", "force-stop", "--user", "10", application)
        replies = {
            ("devices", "-l"): f"List of devices attached\n{serial} device product:probe transport_id:{TRANSPORT}",
            ("get-serialno",): serial,
            ("get-state",): "device",
            DEVICE_CLOCK: [START] + [END] * 12,
            ("shell", "am", "get-current-user"): "10",
            ("shell", "getprop", "ro.build.version.sdk"): "36",
            ("shell", "getprop", "ro.product.cpu.abi"): "arm64-v8a",
            DEVICE_BUILD_QUERY: DEVICE_BUILD,
            ("shell", "pm", "path", "--user", "10", application): f"package:{PACKAGE}",
            ("shell", "toybox", "sha256sum", PACKAGE): f"{DIGEST}  {PACKAGE}",
            launch: f"Status: ok\nLaunchState: COLD\nActivity: {application}/.PackageActivity",
            stop: "",
            ("shell", "cmd", "package", "list", "packages", "--user", "10", "-U", application):
                f"package:{application} uid:1010200",
            ("shell", "pidof", application): "123",
            UID_QUERY: "UID\n1010200",
            STAT_QUERY: proc_stat(),
            LOGCAT: ((log_line(ENTRY) if profile["renderer"] == "none" else log_line(LIFECYCLE) + log_line(FRAME))
                     + log_line("ignored secret=/sdcard/personal")
                     + log_line("FATAL EXCEPTION: unrelated-thread", pid=999, tag="E/AndroidRuntime")),
        }
        replies.update(override or {})
        commands = []
        running = False
        awaiting_receipt = False
        now = 0.0

        def sleep(seconds):
            nonlocal now
            now += seconds

        def run(command, deadline, *, retain_adb_server=False):
            nonlocal running, awaiting_receipt
            self.assertEqual(retain_adb_server, command[1:] == ["devices", "-l"])
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
            if args == LOGCAT and awaiting_receipt:
                awaiting_receipt = False
                if launch_receipt is not None:
                    return 0, launch_receipt
                receipt = log_line(LAUNCH_MARKER, stamp=LAUNCH_TIME)
                if openxr:
                    receipt += log_line(DEADLINE_MARKER, stamp=DEADLINE_TIME)
                return 0, receipt
            response = replies[args]
            if isinstance(response, list):
                response = response.pop(0)
            if args == launch and (launch_reached_device or not isinstance(response, BaseException)):
                running = True
                awaiting_receipt = not isinstance(response, BaseException)
            if isinstance(response, BaseException):
                raise response
            if args == stop and not isinstance(response, tuple):
                running = False
            return response if isinstance(response, tuple) else (0, response)

        stdout = io.StringIO()
        images = (project.OpenXRDiagnosticImages("arm64-v8a", HOST_IMAGE_DIGEST, LOADER_IMAGE_DIGEST),) if "openxrProfile" in profile else ()
        artifact = project.VerifiedAndroidArtifact(Path("a package ü.apk"), profile, DIGEST, PROVENANCE_DIGEST, SOURCE_REVISION,
                                                   frozenset({"arm64-v8a"}), images)
        with patch.object(project, "deploy_artifact", return_value=artifact), \
                patch.object(project, "android_deploy_host", return_value="linux-x64"), \
                patch.object(project, "deploy_process", side_effect=run), \
                patch.object(project.secrets, "token_hex", return_value=LAUNCH_NONCE), \
                patch.object(project.time, "monotonic", side_effect=lambda: now), \
                patch.object(project.time, "sleep", side_effect=sleep), \
                patch.object(project.time, "monotonic_ns", side_effect=[100_000_000, 101_000_000] + [109_000_000, 111_000_000] * 12), \
                contextlib.redirect_stdout(stdout):
            status = project.main(["android-deploy", "--artifact", ".", "--profile", profile_name, "--sdk", ".",
                                   "--device", serial, "--timeout", "1", *options])
        return status, json.loads(stdout.getvalue()), commands

    def test_scopes_install_launch_process_and_reports_only_typed_facts(self):
        status, report, commands = self.execute(profile_name=VULKAN_PROFILE)
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

    def test_waits_for_evidence_from_the_selected_native_profile(self):
        for profile, markers, expected in (
            (PROFILE, (LIFECYCLE, ENTRY), {"entry"}),
            (VULKAN_PROFILE, (ENTRY, LIFECYCLE, FRAME), {"lifecycle", "renderer"}),
        ):
            with self.subTest(profile=profile):
                status, report, commands = self.execute(
                    {LOGCAT: [log_line(marker) for marker in markers]}, profile_name=profile)
                self.assertEqual((status, report["status"], commands.count(LOGCAT)), (0, "complete", len(markers) + 1))
                self.assertEqual(set(report["observed"]), {"crashObserved"} | expected)
                self.assertFalse(any("force-stop" in cmd for cmd in commands))

    def test_vulkan_lifecycle_without_renderer_remains_partial_at_the_deadline(self):
        status, report, commands = self.execute({LOGCAT: log_line(LIFECYCLE)}, profile_name=VULKAN_PROFILE)
        self.assertEqual((status, report["status"], report["error"]), (1, "partial", "application_evidence_unavailable"))
        self.assertEqual(set(report["observed"]), {"crashObserved", "lifecycle"})
        self.assertGreater(commands.count(LOGCAT), 1)

    def test_pending_capture_can_cancel_without_accepting_unrelated_evidence(self):
        status, report, commands = self.execute({LOGCAT: [log_line(LIFECYCLE), KeyboardInterrupt()]})
        self.assertEqual((status, report["error"], report["cleanup"]), (130, "cancelled", {"status": "stopped"}))
        self.assertEqual(commands.count(LOGCAT), 3)

    def test_requested_zip_contains_exact_projected_report_and_its_digest(self):
        for result, expected in ((log_line(ENTRY), 0),
                                 (log_line("FATAL EXCEPTION: private-thread", tag="E/AndroidRuntime"), 1)):
            with self.subTest(expected=expected), tempfile.TemporaryDirectory() as directory:
                path = Path(directory).resolve() / "qualification evidence ü.zip"
                log = result + log_line("private location /sdcard/user") + log_line(LAUNCH_MARKER)
                status, report, _ = self.execute({LOGCAT: log}, ("--diagnostic-bundle", str(path)))
                self.assertEqual(status, expected)
                with zipfile.ZipFile(path) as bundle:
                    self.assertEqual(bundle.namelist(), ["manifest.json", project.DEPLOY_REPORT_ENTRY])
                    data = bundle.read(project.DEPLOY_REPORT_ENTRY)
                    manifest = json.loads(bundle.read("manifest.json"))
                self.assertEqual(data, project.canonical_json(report))
                self.assertEqual(manifest["files"], [{"path": project.DEPLOY_REPORT_ENTRY, "bytes": len(data),
                                                      "sha256": hashlib.sha256(data).hexdigest()}])
                for denied in ("private-thread", "private location", LAUNCH_NONCE, SERIAL, PACKAGE):
                    self.assertNotIn(denied, data.decode())
                self.assertEqual(list(path.parent.iterdir()), [path])

    def test_occupied_diagnostic_destination_is_rejected_before_device_commands(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory).resolve() / "Existing.ZIP"
            path.write_bytes(b"previous archive")
            destination = path.with_name("existing.zip") if project.os.name == "nt" else path
            status, report, commands = self.execute(options=("--diagnostic-bundle", str(destination)))
            self.assertEqual((status, report["error"], commands), (1, "diagnostic_bundle_destination_invalid", []))
            self.assertEqual(path.read_bytes(), b"previous archive")

    def test_diagnostic_publication_collision_preserves_new_owner_and_device_outcome(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory).resolve() / "race.zip"
            publish = project.os.link

            def compete(staged, destination):
                self.assertEqual(destination, path)
                destination.write_bytes(b"other owner")
                publish(staged, destination)

            with patch.object(project.os, "link", side_effect=compete):
                status, report, commands = self.execute(options=("--diagnostic-bundle", str(path)))
            self.assertEqual((status, report["status"], report["bundleError"]), (1, "complete", "diagnostic_bundle_write_failed"))
            self.assertFalse(any("force-stop" in cmd for cmd in commands))
            self.assertEqual(path.read_bytes(), b"other owner")
            self.assertEqual(list(path.parent.iterdir()), [path])

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
        self.assertEqual(report["artifact"]["openxrImages"],
                         {"abi": "arm64-v8a", "diagnosticHostSha256": HOST_IMAGE_DIGEST, "loaderSha256": LOADER_IMAGE_DIGEST})
        self.assertEqual(report["qualification"]["runtime"],
                         {"requested": "openxr", "selection": "system-default", "status": "cold_system_discovered"})
        self.assertEqual(report["qualification"]["physicalXrQualification"], "not_run")
        self.assertEqual(report["device"], {"androidApi": 36, "abi": "arm64-v8a", "buildSha256": DEVICE_BUILD_DIGEST})
        self.assertEqual(report["observed"]["openxr"]["runtimeVersion"], {"major": 1, "minor": 1, "patch": 63})
        self.assertEqual(report["cleanup"], {"status": "stopped"})
        self.assertIn(("shell", "am", "force-stop", "--user", "10", profile["applicationId"]), commands)
        self.assertNotIn("private", json.dumps(report))
        self.assertNotIn(DEVICE_BUILD, json.dumps(report))

    def test_openxr_checks_the_selected_system_build_before_install(self):
        name = project.OPENXR_DEVELOPMENT_PROFILE
        for digest, expected_status in ((DEVICE_BUILD_DIGEST, 0), ("f" * 64, 1)):
            with self.subTest(match=expected_status == 0):
                options = (*OPENXR_OPTIONS, "--expect-device-build-sha256", digest)
                status, report, commands = self.execute({LOGCAT: log_line(OPENXR)}, options, name)
                self.assertEqual(status, expected_status)
                self.assertEqual(report["device"]["expectedBuildSha256"], digest)
                self.assertEqual(report["device"]["buildSha256"], DEVICE_BUILD_DIGEST)
                if status:
                    self.assertEqual((report["stage"], report["error"]), ("device", "device_build_mismatch"))
                    self.assertFalse(any(command[0] == "install" for command in commands))
                else:
                    self.assertEqual(commands.count(DEVICE_BUILD_QUERY), 2)

    def test_openxr_usb_deployment_report_matches_release_preflight_contract(self):
        name = project.OPENXR_QUEST_DEVELOPMENT_PROFILE
        serial = "TESTHEADSET"
        status, report, _ = self.execute({
            ("devices", "-l"): f"List of devices attached\n{serial} device usb:1-2 transport_id:{TRANSPORT}",
            LOGCAT: log_line(OPENXR + " primaryView0=1024x768 primaryView1=1280x800"),
        }, ("--host", "linux-x64", "--connection", "usb", "--runtime", "openxr", "--expect-runtime-version", "1.1.63"), name,
            serial=serial)
        self.assertEqual(status, 0)
        self.assertEqual(report["observed"]["openxr"]["primaryStereoRecommendedViewExtents"],
                         [{"width": 1024, "height": 768}, {"width": 1280, "height": 800}])
        receipt = project.canonical_json(report)
        declaration = validate_android_preflight({
            "receiptSha256": hashlib.sha256(receipt).hexdigest(), "apkSha256": DIGEST,
            "deviceBuildSha256": DEVICE_BUILD_DIGEST, "profile": name, "adbConnection": "usb",
        })
        check_android_preflight(receipt, declaration, SOURCE_REVISION, "1.1.63")

    def test_openxr_cold_extents_reject_partial_or_invalid_publication(self):
        invalid = (" primaryView0=1024x768", " primaryView0=0x768 primaryView1=1280x800",
                   " primaryView0=4294967296x768 primaryView1=1280x800", " primaryView0=1024x768 primaryView1=1280x800 private=room")
        for suffix in invalid:
            with self.subTest(suffix=suffix):
                with self.assertRaises(project.AndroidDeployError):
                    project.openxr_log_evidence(OPENXR + suffix)

    def test_quest_profile_requires_openxr_intent_and_launches_its_own_application(self):
        name = project.OPENXR_QUEST_DEVELOPMENT_PROFILE
        _, profile = project.load_contract(name, ["arm64-v8a"])
        status, report, commands = self.execute(profile_name=name)
        self.assertEqual((status, report["error"]), (1, "explicit_openxr_intent_required"))
        self.assertEqual(commands, [])

        status, report, commands = self.execute({LOGCAT: log_line(OPENXR)}, OPENXR_OPTIONS, name)
        self.assertEqual((status, report["status"]), (0, "diagnostic_complete"))
        self.assertEqual(report["artifact"]["profile"], name)
        self.assertEqual(report["qualification"]["physicalXrQualification"], "not_run")
        self.assertIn(launch_command(profile["applicationId"], openxr=True), commands)

    def test_openxr_rejects_unverifiable_or_changed_system_builds(self):
        name = project.OPENXR_DEVELOPMENT_PROFILE
        for build in ("", "private\nsecond-line", "x" * 257, "özel-build"):
            with self.subTest(build=build[:16]):
                status, report, commands = self.execute({DEVICE_BUILD_QUERY: build}, OPENXR_OPTIONS, name)
                self.assertEqual((status, report["stage"], report["error"]), (1, "device", "device_build_unverifiable"))
                self.assertFalse(any(command[0] == "install" for command in commands))
        status, report, commands = self.execute(
            {DEVICE_BUILD_QUERY: [DEVICE_BUILD, "example/changed:14/build"], LOGCAT: log_line(OPENXR)}, OPENXR_OPTIONS, name)
        self.assertEqual((status, report["status"], report["error"]), (1, "partial", "device_build_changed"))
        self.assertEqual(report["cleanup"], {"status": "stopped"})
        self.assertNotIn("observed", report)
        self.assertEqual(report["qualification"]["runtime"]["status"], "not_observed")
        self.assertEqual(commands.count(DEVICE_BUILD_QUERY), 2)

    def test_openxr_image_evidence_uses_the_verified_native_entries_for_each_abi(self):
        _, profile = project.load_contract(project.OPENXR_DEVELOPMENT_PROFILE, ["arm64-v8a", "x86_64"])
        library = f"lib{profile['nativeLibrary']}.so"
        inspection = {"contents": [
            {"name": f"lib/arm64-v8a/{library}", "sha256": HOST_IMAGE_DIGEST},
            {"name": f"lib/arm64-v8a/{project.OPENXR_LOADER_NAME}", "sha256": LOADER_IMAGE_DIGEST},
            {"name": f"lib/x86_64/{library}", "sha256": DIGEST},
            {"name": f"lib/x86_64/{project.OPENXR_LOADER_NAME}", "sha256": PROVENANCE_DIGEST},
            {"name": "assets/private/location", "sha256": "0" * 64},
        ]}
        self.assertEqual(project.verified_openxr_images(profile, inspection, frozenset({"x86_64", "arm64-v8a"})),
                         (project.OpenXRDiagnosticImages("arm64-v8a", HOST_IMAGE_DIGEST, LOADER_IMAGE_DIGEST),
                          project.OpenXRDiagnosticImages("x86_64", DIGEST, PROVENANCE_DIGEST)))

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

    def test_openxr_checks_selected_apk_before_any_device_command(self):
        name = project.OPENXR_DEVELOPMENT_PROFILE
        expected = "e" * 64
        options = (*OPENXR_OPTIONS, "--expect-apk-sha256", expected)
        status, report, commands = self.execute({}, options, name)
        self.assertEqual((status, report["stage"], report["error"]), (1, "artifact", "deployment_apk_digest_mismatch"))
        self.assertEqual(report["artifact"]["expectedApkSha256"], expected)
        self.assertEqual(report["artifact"]["apkSha256"], DIGEST)
        self.assertEqual(commands, [])

    def test_openxr_requires_diagnostics_strictly_after_the_attributed_launch_receipt(self):
        stale = OPENXR.replace("runtimePatch=63", "runtimePatch=62")
        invalid_window = log_line(stale, stamp="1795000000.103000000") + log_line(stale, stamp=DEADLINE_TIME)
        status, report, commands = self.execute(
            {LOGCAT: [invalid_window, invalid_window + log_line(OPENXR)]}, OPENXR_OPTIONS, project.OPENXR_DEVELOPMENT_PROFILE)
        self.assertEqual((status, report["status"]), (0, "diagnostic_complete"))
        self.assertEqual(commands.count(LOGCAT), 3)
        self.assertEqual(report["observed"]["openxr"]["runtimeVersion"], {"major": 1, "minor": 1, "patch": 63})
        self.assertEqual(report["cleanup"], {"status": "stopped"})
        exported = json.dumps(report)
        for private in (LAUNCH_TIME, DEADLINE_TIME, LAUNCH_NONCE, "startTicks"):
            self.assertNotIn(private, exported)

    def test_openxr_requires_an_exact_activity_deadline_receipt_before_diagnostics(self):
        launch = log_line(LAUNCH_MARKER, stamp=LAUNCH_TIME)
        deadline = log_line(DEADLINE_MARKER, stamp=DEADLINE_TIME)
        for evidence in ("", deadline * 2, deadline.replace("milliseconds=1000", "milliseconds=999"),
                         deadline.replace(DEADLINE_TIME, LAUNCH_TIME), deadline.replace("startTicks=123456", "startTicks=123457")):
            with self.subTest(evidence=evidence):
                status, report, commands = self.execute(
                    {LOGCAT: log_line(OPENXR)}, OPENXR_OPTIONS, project.OPENXR_DEVELOPMENT_PROFILE,
                    launch_receipt=launch + evidence)
                self.assertEqual((status, report["stage"], report["error"]), (1, "launch", "openxr_launch_deadline_unconfirmed"))
                self.assertNotIn("observed", report)
                self.assertEqual(commands.count(LOGCAT), 1)
                self.assertEqual(report["cleanup"], {"status": "stopped"})
                self.assertNotIn(LAUNCH_NONCE, json.dumps(report))

    def test_openxr_checks_numeric_runtime_version_and_retires_a_mismatch(self):
        name = project.OPENXR_DEVELOPMENT_PROFILE
        for version, status, runtime_status in ((63, 0, "cold_system_discovered"), (64, 1, "version_mismatch")):
            with self.subTest(version=version):
                options = (*OPENXR_OPTIONS, "--expect-apk-sha256", DIGEST, "--expect-runtime-version", f"1.1.{version}")
                actual, report, commands = self.execute({LOGCAT: log_line(OPENXR)}, options, name)
                self.assertEqual(actual, status)
                self.assertEqual(report["qualification"]["runtime"]["status"], runtime_status)
                self.assertEqual(report["qualification"]["runtime"]["expectedVersion"], {"major": 1, "minor": 1, "patch": version})
                self.assertEqual(report["observed"]["openxr"]["runtimeVersion"], {"major": 1, "minor": 1, "patch": 63})
                self.assertEqual(report["qualification"]["physicalXrQualification"], "not_run")
                if status:
                    self.assertEqual(report["error"], "openxr_runtime_version_mismatch")
                    self.assertEqual(report["status"], "partial")
                self.assertEqual(report["cleanup"], {"status": "stopped"})
                self.assertTrue(any("force-stop" in cmd for cmd in commands))

    def test_openxr_cold_discovery_native_failure_keeps_typed_stage(self):
        raw = log_line("host-openxr abi=arm64-v8a stage=extension-admission result=-51 runtimeMajor=0 runtimeMinor=0 runtimePatch=0")
        status, report, _ = self.execute({LOGCAT: raw}, OPENXR_OPTIONS, project.OPENXR_DEVELOPMENT_PROFILE)
        self.assertEqual((status, report["error"]), (1, "openxr_cold_discovery_failed"))
        self.assertEqual(report["observed"]["openxr"], {"abi": "arm64-v8a", "stage": "extension-admission", "result": -51})
        self.assertEqual(report["cleanup"], {"status": "stopped"})

    def test_openxr_retirement_failure_remains_a_failed_deployment(self):
        first = ("host-openxr abi=arm64-v8a stage=instance-retirement result=-2 "
                 "runtimeMajor=0 runtimeMinor=0 runtimePatch=0")
        raw = log_line(first) + log_line("OpenXR instance retirement deadline expired")
        status, report, _ = self.execute({LOGCAT: raw}, OPENXR_OPTIONS, project.OPENXR_DEVELOPMENT_PROFILE)
        self.assertEqual((status, report["error"]), (1, "openxr_cold_discovery_failed"))
        self.assertEqual(report["observed"]["openxr"], {"abi": "arm64-v8a", "stage": "instance-retirement", "result": -2})
        self.assertEqual(report["cleanup"], {"status": "stopped"})
        self.assertEqual(report["qualification"]["physicalXrQualification"], "not_run")

    def test_openxr_crash_before_live_pid_admission_preserves_the_attributed_failure(self):
        _, profile = project.load_contract(project.OPENXR_DEVELOPMENT_PROFILE, ["arm64-v8a"])
        application = profile["applicationId"]
        log = log_line(LAUNCH_MARKER)
        log += log_line(LAUNCH_CRASH_MARKER, stamp=END)
        log += log_line("FATAL EXCEPTION: private-data", pid=999, tag="E/AndroidRuntime")
        status, report, commands = self.execute(
            {("shell", "pidof", application): (1, ""), QUALIFICATION_LOGCAT: log},
            OPENXR_OPTIONS, project.OPENXR_DEVELOPMENT_PROFILE)
        self.assertEqual((status, report["stage"], report["error"]), (1, "launch", "application_crashed"))
        self.assertEqual(report["observed"], {"crashObserved": True})
        self.assertEqual(report["cleanup"], {"status": "already_exited"})
        self.assertIn(QUALIFICATION_LOGCAT, commands)
        self.assertNotIn(LOGCAT, commands)
        self.assertFalse(any("force-stop" in cmd for cmd in commands))
        for private in (LAUNCH_NONCE, "startTicks", "private-data"):
            self.assertNotIn(private, json.dumps(report))

    def test_openxr_retired_crash_requires_matching_ordered_launch_and_crash_receipts(self):
        _, profile = project.load_contract(project.OPENXR_DEVELOPMENT_PROFILE, ["arm64-v8a"])
        application = profile["applicationId"]
        launch = log_line(LAUNCH_MARKER)
        for recorded in (
            launch + log_line(LAUNCH_CRASH_MARKER, stamp=END, uid="10200"),
            launch + log_line(LAUNCH_CRASH_MARKER, stamp=END, pid=124),
            launch + log_line(LAUNCH_CRASH_MARKER.replace("123456", "123457"), stamp=END),
            launch * 2 + log_line(LAUNCH_CRASH_MARKER, stamp=END),
            launch + log_line(LAUNCH_CRASH_MARKER, stamp="1795000000.101000000"),
            launch + log_line(LAUNCH_CRASH_MARKER.replace(LAUNCH_NONCE, "f" * 32), stamp=END),
            log_line(LAUNCH_CRASH_MARKER, stamp=END),
        ):
            with self.subTest(log=recorded):
                status, report, commands = self.execute(
                    {("shell", "pidof", application): (1, ""), QUALIFICATION_LOGCAT: recorded},
                    OPENXR_OPTIONS, project.OPENXR_DEVELOPMENT_PROFILE)
                self.assertEqual((status, report["error"]), (1, "application_process_unavailable"))
                self.assertNotIn("observed", report)
                self.assertFalse(any("force-stop" in cmd for cmd in commands))

    def test_openxr_retired_crash_leaves_a_replacement_process_running(self):
        _, profile = project.load_contract(project.OPENXR_DEVELOPMENT_PROFILE, ["arm64-v8a"])
        application = profile["applicationId"]
        replacement_stat = proc_stat(123457).replace("123 (", "124 (", 1)
        replacement_logcat = (LOGCAT[0], "--pid=124", *LOGCAT[2:])
        status, report, commands = self.execute({
            ("shell", "pidof", application): [(1, "")] * 4 + ["124"] * 3,
            QUALIFICATION_LOGCAT: log_line(LAUNCH_MARKER) + log_line(LAUNCH_CRASH_MARKER, stamp=END),
            ("shell", "toybox", "cat", "/proc/124/stat"): replacement_stat,
            ("shell", "toybox", "ps", "-p", "124", "-o", "UID"): "UID\n1010200",
            replacement_logcat: "",
        }, OPENXR_OPTIONS, project.OPENXR_DEVELOPMENT_PROFILE)
        self.assertEqual((status, report["error"]), (1, "application_process_unavailable"))
        self.assertNotIn("observed", report)
        self.assertEqual(report["cleanup"], {"status": "unconfirmed", "reason": "launch_process_unattributed"})
        self.assertFalse(any("force-stop" in cmd for cmd in commands))

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

    def test_rejects_package_replacement_after_capturing_diagnostics(self):
        checksum = ("shell", "toybox", "sha256sum", PACKAGE)
        admitted = f"{DIGEST}  {PACKAGE}"
        replacement = f"{'b' * 64}  {PACKAGE}"
        status, report, commands = self.execute({checksum: [admitted, replacement, replacement]})
        self.assertEqual((status, report["status"], report["error"]), (1, "partial", "installed_artifact_stale"))
        self.assertEqual(report["cleanup"], {"status": "unconfirmed", "reason": "installed_artifact_stale"})
        self.assertNotIn("observed", report)
        self.assertIn(checksum, commands[commands.index(LOGCAT) + 1:])
        self.assertFalse(any("force-stop" in cmd for cmd in commands))

    def test_openxr_rechecks_the_package_after_stopping_the_probe(self):
        name = project.OPENXR_DEVELOPMENT_PROFILE
        checksum = ("shell", "toybox", "sha256sum", PACKAGE)
        admitted = f"{DIGEST}  {PACKAGE}"
        replacement = f"{'b' * 64}  {PACKAGE}"
        with tempfile.TemporaryDirectory() as directory:
            destination = Path(directory).resolve() / "failure.zip"
            options = (*OPENXR_OPTIONS, "--diagnostic-bundle", str(destination))
            status, report, commands = self.execute(
                {LOGCAT: log_line(OPENXR), checksum: [admitted, admitted, replacement]}, options, name)
            with zipfile.ZipFile(destination) as bundle:
                self.assertEqual(json.loads(bundle.read(project.DEPLOY_REPORT_ENTRY)), report)
        self.assertEqual((status, report["status"], report["error"]), (1, "partial", "installed_artifact_stale"))
        self.assertEqual(report["cleanup"], {"status": "stopped"})
        self.assertNotIn("observed", report)
        self.assertEqual(report["qualification"]["runtime"]["status"], "not_observed")
        self.assertEqual(report["qualification"]["physicalXrQualification"], "not_run")
        self.assertEqual(commands[-1], checksum)

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
        status, report, commands = self.execute({STAT_QUERY: [proc_stat()] * 4 + [proc_stat(123457)] * 4})
        self.assertEqual((status, report["error"]), (1, "application_process_changed"))
        self.assertEqual(report["cleanup"], {"status": "unconfirmed", "reason": "application_process_changed"})
        self.assertFalse(any("force-stop" in cmd for cmd in commands))

    def test_disconnected_transport_never_reselects_target(self):
        status, report, commands = self.execute({
            LOGCAT: (1, "error: device disconnected"),
            ("get-serialno",): [SERIAL] * 3 + [(1, "error: device not found")],
        })
        self.assertEqual((status, report["error"]), (1, "device_disconnected"))
        self.assertEqual(report["cleanup"], {"status": "unconfirmed", "reason": "device_disconnected"})
        self.assertFalse(any("force-stop" in cmd for cmd in commands))
        self.assertEqual(commands.count(("devices", "-l")), 1)

    def test_changed_transport_blocks_install_launch_and_evidence_admission(self):
        for confirmed, stage, install, launch in (
            (1, "install", False, False),
            (2, "launch", True, False),
            (3, "diagnostics", True, True),
            (4, "diagnostics", True, True),
        ):
            with self.subTest(confirmed=confirmed):
                status, report, commands = self.execute({
                    ("get-serialno",): [SERIAL] * confirmed + ["private-foreign-device"],
                })
                self.assertEqual((status, report.get("error"), report["stage"]), (1, "device_transport_changed", stage))
                self.assertEqual(report["status"], "partial")
                self.assertEqual(report["qualification"]["physicalXrQualification"], "not_run")
                self.assertEqual(any(command[0] == "install" for command in commands), install)
                self.assertEqual(launch_command(APPLICATION) in commands, launch)
                self.assertNotIn(("shell", "am", "force-stop", "--user", "10", APPLICATION), commands)
                self.assertEqual(commands.count(("devices", "-l")), 1)
                self.assertNotIn("observed", report)
                if launch:
                    self.assertEqual(report["cleanup"], {"status": "unconfirmed", "reason": "device_transport_changed"})
                self.assertNotIn("private-foreign-device", json.dumps(report))

    def test_openxr_does_not_stop_a_process_when_transport_changes_during_cleanup(self):
        name = project.OPENXR_DEVELOPMENT_PROFILE
        _, profile = project.load_contract(name, ["arm64-v8a"])
        stop = ("shell", "am", "force-stop", "--user", "10", profile["applicationId"])
        status, report, commands = self.execute({
            LOGCAT: log_line(OPENXR),
            ("get-serialno",): [SERIAL] * 5 + ["private-replacement-device"],
        }, OPENXR_OPTIONS, name)
        self.assertEqual((status, report["status"], report["error"]), (1, "partial", "application_stop_unconfirmed"))
        self.assertEqual(report["cleanup"], {"status": "unconfirmed", "reason": "device_transport_changed"})
        self.assertNotIn(stop, commands)
        self.assertNotIn("observed", report)
        self.assertEqual(report["qualification"]["runtime"]["status"], "not_observed")
        self.assertNotIn("private-replacement-device", json.dumps(report))

    def test_openxr_discards_discovery_when_transport_changes_before_publication(self):
        status, report, _ = self.execute({
            LOGCAT: log_line(OPENXR),
            ("get-serialno",): [SERIAL] * 6 + ["private-foreign-device"],
        }, OPENXR_OPTIONS, project.OPENXR_DEVELOPMENT_PROFILE)
        self.assertEqual((status, report["status"], report["error"]), (1, "partial", "device_transport_changed"))
        self.assertNotIn("observed", report)
        self.assertEqual(report["qualification"]["runtime"]["status"], "not_observed")
        self.assertEqual(report["qualification"]["physicalXrQualification"], "not_run")
        self.assertEqual(report["cleanup"], {"status": "stopped"})
        self.assertNotIn("private-foreign-device", json.dumps(report))

    def test_openxr_does_not_continue_after_android_foreground_user_changes(self):
        name = project.OPENXR_DEVELOPMENT_PROFILE
        _, profile = project.load_contract(name, ["arm64-v8a"])
        launch = launch_command(profile["applicationId"], openxr=True)
        stop = ("shell", "am", "force-stop", "--user", "10", profile["applicationId"])
        changed_user = "216037421"
        for confirmed, stage, installed, launched in (
            (1, "install", False, False),
            (2, "launch", True, False),
            (3, "diagnostics", True, True),
        ):
            with self.subTest(stage=stage):
                status, report, commands = self.execute({
                    LOGCAT: log_line(OPENXR),
                    ("shell", "am", "get-current-user"): ["10"] * confirmed + [changed_user],
                }, OPENXR_OPTIONS, name)
                self.assertEqual((status, report["status"], report["stage"], report["error"]),
                                 (1, "partial", stage, "device_user_changed"))
                self.assertEqual(any(command[0] == "install" for command in commands), installed)
                self.assertEqual(launch in commands, launched)
                self.assertEqual(stop in commands, launched)
                if launched:
                    self.assertEqual(report["cleanup"], {"status": "stopped"})
                self.assertNotIn("observed", report)
                self.assertEqual(report["qualification"]["runtime"]["status"], "not_observed")
                self.assertNotIn(changed_user, json.dumps(report))

    def test_openxr_discards_discovery_when_process_retirement_cannot_be_confirmed(self):
        name = project.OPENXR_DEVELOPMENT_PROFILE
        _, profile = project.load_contract(name, ["arm64-v8a"])
        stop = ("shell", "am", "force-stop", "--user", "10", profile["applicationId"])
        status, report, commands = self.execute({LOGCAT: log_line(OPENXR), stop: (1, "stop failed")}, OPENXR_OPTIONS, name)
        self.assertEqual((status, report["status"], report["error"]), (1, "partial", "application_stop_unconfirmed"))
        self.assertEqual(report["cleanup"], {"status": "unconfirmed", "reason": "device_command_failed"})
        self.assertNotIn("observed", report)
        self.assertEqual(report["qualification"]["runtime"]["status"], "not_observed")
        self.assertEqual(commands.count(stop), 1)

    def test_buffered_records_with_reused_pid_or_foreign_uid_cannot_supply_evidence(self):
        stale = log_line(ENTRY, stamp=START)
        stale += log_line("FATAL EXCEPTION: old process", stamp="1795000000.099999999", tag="E/AndroidRuntime")
        stale += log_line(ENTRY, uid="10200")
        stale += log_line(ENTRY, stamp="1795000000.110000001")
        stale += log_line(ENTRY, tag="W/HoroPackageQualification")
        status, report, _ = self.execute({LOGCAT: stale})
        self.assertEqual((status, report["error"]), (1, "application_evidence_unavailable"))
        self.assertEqual(report["observed"], {"crashObserved": False})

    def test_latest_matching_marker_survives_out_of_order_buffer_records(self):
        log = log_line(LIFECYCLE, stamp=END, uid="u10_a200")
        log += log_line(LIFECYCLE.replace("lifecycleRevision=3", "lifecycleRevision=2"))
        log += log_line(FRAME)
        status, report, _ = self.execute({LOGCAT: log}, profile_name=VULKAN_PROFILE)
        self.assertEqual(status, 0)
        self.assertEqual(report["observed"]["lifecycle"]["lifecycleRevision"], 3)
        self.assertNotIn("u10_a200", json.dumps(report))

    def test_vulkan_capture_projects_only_the_latest_attributed_thermal_state(self):
        log = log_line(LIFECYCLE) + log_line(FRAME)
        log += log_line("host-thermal revision=4 state=severe")
        log += log_line("host-thermal revision=6 state=none")
        log += log_line("host-thermal revision=5 state=inactive", stamp=END)
        log += log_line("host-thermal revision=7 state=critical raw=private")
        log += log_line("host-thermal revision=8 state=critical", uid="10200")
        log += log_line("host-thermal revision=9223372036854775808 state=unsupported")
        status, report, _ = self.execute({LOGCAT: log}, profile_name=VULKAN_PROFILE)
        self.assertEqual((status, report["status"]), (0, "complete"))
        self.assertEqual(report["observed"]["thermal"], {"revision": 6, "state": "none"})
        self.assertNotIn("raw=private", json.dumps(report))

    def test_vulkan_can_report_an_unsupported_thermal_signal_without_failing(self):
        log = log_line(LIFECYCLE) + log_line(FRAME) + log_line("host-thermal revision=1 state=unsupported")
        status, report, _ = self.execute({LOGCAT: log}, profile_name=VULKAN_PROFILE)
        self.assertEqual((status, report["status"]), (0, "complete"))
        self.assertEqual(report["observed"]["thermal"], {"revision": 1, "state": "unsupported"})

    def test_vulkan_capture_preserves_thermal_revision_across_diagnostic_polls(self):
        first = log_line(LIFECYCLE) + log_line("host-thermal revision=2 state=inactive")
        second = log_line(FRAME) + log_line("host-thermal revision=1 state=severe")
        status, report, _ = self.execute({LOGCAT: [first, second]}, profile_name=VULKAN_PROFILE)
        self.assertEqual((status, report["status"]), (0, "complete"))
        self.assertEqual(report["observed"]["thermal"], {"revision": 2, "state": "inactive"})

    def test_vulkan_capture_preserves_independent_memory_pressure_evidence(self):
        first = log_line(LIFECYCLE) + log_line("host-memory revision=2 state=critical")
        first += log_line("host-thermal revision=2 state=severe")
        first += log_line("host-memory revision=3 state=hidden")
        first += log_line("host-memory revision=4 state=low raw=private")
        first += log_line("host-memory revision=5 state=low", uid="10200")
        second = log_line(FRAME) + log_line("host-memory revision=1 state=normal")
        second += log_line("host-thermal revision=3 state=none")
        status, report, _ = self.execute({LOGCAT: [first, second]}, profile_name=VULKAN_PROFILE)
        self.assertEqual((status, report["status"]), (0, "complete"))
        self.assertEqual(report["observed"]["memory"], {"revision": 2, "state": "critical"})
        self.assertEqual(report["observed"]["thermal"], {"revision": 3, "state": "none"})
        self.assertNotIn("raw=private", json.dumps(report))

    def test_vulkan_capture_reports_power_saver_from_the_attributed_process(self):
        for revision, state in enumerate(("disabled", "enabled", "unavailable", "unsupported", "inactive"), start=1):
            with self.subTest(state=state):
                log = log_line(LIFECYCLE) + log_line(FRAME)
                log += log_line(f"host-power revision={revision} state={state}")
                log += log_line("host-power revision=6 state=enabled", uid="10200")
                log += log_line("host-power revision=7 state=enabled raw=private")
                status, report, _ = self.execute({LOGCAT: log}, profile_name=VULKAN_PROFILE)
                self.assertEqual((status, report["status"]), (0, "complete"))
                self.assertEqual(report["observed"]["power"], {"revision": revision, "state": state})
                self.assertNotIn("raw=private", json.dumps(report))

    def test_vulkan_capture_requires_a_bounded_rate_only_for_observed_refresh(self):
        log = log_line(LIFECYCLE) + log_line(FRAME)
        log += log_line("host-refresh revision=2 state=observed milliHz=59940")
        log += log_line("host-refresh revision=3 state=observed milliHz=0")
        log += log_line("host-refresh revision=4 state=unavailable milliHz=60000")
        log += log_line("host-refresh revision=5 state=observed milliHz=2147483648")
        log += log_line("host-refresh revision=6 state=observed milliHz=90000", uid="10200")
        status, report, _ = self.execute({LOGCAT: log}, profile_name=VULKAN_PROFILE)
        self.assertEqual((status, report["status"]), (0, "complete"))
        self.assertEqual(report["observed"]["refresh"], {"revision": 2, "state": "observed", "milliHz": 59940})

    def test_vulkan_capture_clears_the_rate_when_display_evidence_is_withdrawn(self):
        first = log_line(LIFECYCLE) + log_line("host-refresh revision=2 state=inactive milliHz=0")
        second = log_line(FRAME) + log_line("host-refresh revision=1 state=observed milliHz=60000")
        status, report, _ = self.execute({LOGCAT: [first, second]}, profile_name=VULKAN_PROFILE)
        self.assertEqual((status, report["status"]), (0, "complete"))
        self.assertEqual(report["observed"]["refresh"], {"revision": 2, "state": "inactive"})

    def test_vulkan_capture_requires_a_bounded_percentage_for_battery_evidence(self):
        log = log_line(LIFECYCLE) + log_line(FRAME)
        log += log_line("host-battery revision=2 state=charging percent=0")
        log += log_line("host-battery revision=3 state=full percent=101")
        log += log_line("host-battery revision=4 state=discharging percent=-1")
        log += log_line("host-battery revision=5 state=unsupported percent=55")
        log += log_line("host-battery revision=6 state=full percent=100", uid="10200")
        status, report, _ = self.execute({LOGCAT: log}, profile_name=VULKAN_PROFILE)
        self.assertEqual((status, report["status"]), (0, "complete"))
        self.assertEqual(report["observed"]["battery"], {"revision": 2, "state": "charging", "percent": 0})

    def test_vulkan_capture_withdraws_a_battery_measurement_when_unavailable(self):
        first = log_line(LIFECYCLE) + log_line("host-battery revision=2 state=inactive percent=-1")
        second = log_line(FRAME) + log_line("host-battery revision=1 state=charging percent=80")
        status, report, _ = self.execute({LOGCAT: [first, second]}, profile_name=VULKAN_PROFILE)
        self.assertEqual((status, report["status"]), (0, "complete"))
        self.assertEqual(report["observed"]["battery"], {"revision": 2, "state": "inactive"})

    def test_vulkan_capture_records_pacing_from_the_frame_owner(self):
        first = log_line(LIFECYCLE) + log_line("host-pacing revision=2 state=thermal-conservation")
        second = log_line(FRAME) + log_line("host-pacing revision=1 state=baseline")
        second += log_line("host-pacing revision=3 state=baseline", uid="10200")
        status, report, _ = self.execute({LOGCAT: [first, second]}, profile_name=VULKAN_PROFILE)
        self.assertEqual((status, report["status"]), (0, "complete"))
        self.assertEqual(report["observed"]["pacing"], {"revision": 2, "state": "thermal-conservation"})

    def test_vulkan_capture_records_native_conservation_modes(self):
        for state in ("power-conservation", "memory-conservation"):
            with self.subTest(state=state):
                log = log_line(LIFECYCLE) + log_line(FRAME)
                log += log_line("host-pacing revision=1 state=thermal-conservation")
                log += log_line(f"host-pacing revision=2 state={state}")
                log += log_line("host-pacing revision=3 state=fast")
                status, report, _ = self.execute({LOGCAT: log}, profile_name=VULKAN_PROFILE)
                self.assertEqual((status, report["status"]), (0, "complete"))
                self.assertEqual(report["observed"]["pacing"], {"revision": 2, "state": state})

    def test_vulkan_capture_records_attempt_count_at_the_pacing_transition(self):
        log = log_line(LIFECYCLE) + log_line(FRAME)
        log += log_line("host-pacing revision=1 state=baseline intervalMs=16 attempts=0")
        log += log_line("host-pacing revision=2 state=thermal-conservation intervalMs=32 attempts=125")
        log += log_line("host-pacing revision=3 state=baseline intervalMs=16 attempts=18446744073709551616")
        log += log_line("host-pacing revision=4 state=baseline intervalMs=2147483648 attempts=130")
        status, report, _ = self.execute({LOGCAT: log}, profile_name=VULKAN_PROFILE)
        self.assertEqual((status, report["status"]), (0, "complete"))
        self.assertEqual(report["observed"]["pacing"],
                         {"revision": 2, "state": "thermal-conservation", "intervalMs": 32, "attempts": 125})

    def test_vulkan_observation_keeps_ordered_pressure_and_lifecycle_transitions_across_polls(self):
        resumed = LIFECYCLE.replace("state=2", "state=3")
        paused = resumed.replace("lifecycleRevision=3", "lifecycleRevision=4").replace("state=3", "state=4")
        recovered = resumed.replace("lifecycleRevision=3", "lifecycleRevision=5")
        initial_memory = log_line("host-memory revision=1 state=normal")
        startup = log_line(resumed) + log_line(FRAME) + initial_memory
        startup += log_line("host-pacing revision=1 state=baseline intervalMs=16 attempts=2")
        pressure = log_line("host-pacing revision=2 state=memory-conservation intervalMs=32 attempts=29", stamp="1795000000.107000000")
        pressure += log_line("host-memory revision=2 state=low", stamp="1795000000.106000000")
        background = log_line(paused, stamp="1795000000.108000000")
        background += log_line("host-memory revision=3 state=inactive", stamp="1795000000.108500000")
        recovery = log_line(recovered, stamp="1795000000.109000000")
        recovery += log_line("host-memory revision=4 state=normal", stamp="1795000000.109500000")
        recovery += log_line("host-pacing revision=3 state=baseline intervalMs=16 attempts=63", stamp="1795000000.110000000")
        recovery += log_line("host-memory revision=99 state=critical", uid="10200")
        recovery += log_line("host-memory revision=100 state=critical payload=private-observation")
        status, report, commands = self.execute(
            {LOGCAT: [startup, pressure + initial_memory, background + pressure + startup] + [recovery] * 9},
            ("--timeout", "3", "--observe-seconds", "2"), VULKAN_PROFILE)

        self.assertEqual((status, report["status"], report["qualification"]["physicalXrQualification"]), (0, "complete", "not_run"))
        self.assertEqual(commands.count(LOGCAT), 1 + 5)  # Launch receipt, then the two-second sampling window.
        window = report["observed"]["observationWindow"]
        self.assertEqual({key: window[key] for key in ("durationSeconds", "complete", "truncated")},
                         {"durationSeconds": 2, "complete": True, "truncated": False})
        transitions = window["transitions"]
        offsets = [item["elapsedNs"] for item in transitions]
        self.assertEqual(offsets, sorted(offsets))
        self.assertEqual([(item["revision"], item["state"], item["elapsedNs"]) for item in transitions if item["category"] == "memory"],
                         [(1, "normal", 5_000_000), (2, "low", 6_000_000), (3, "inactive", 8_500_000), (4, "normal", 9_500_000)])
        self.assertEqual([(item["lifecycleRevision"], item["state"]) for item in transitions if item["category"] == "lifecycle"],
                         [(3, 3), (4, 4), (5, 3)])
        self.assertEqual([(item["revision"], item["state"], item["intervalMs"], item["attempts"])
                          for item in transitions if item["category"] == "pacing"],
                         [(1, "baseline", 16, 2), (2, "memory-conservation", 32, 29), (3, "baseline", 16, 63)])
        for denied in ("private-observation", START, LOG_TIME, END):
            self.assertNotIn(denied, json.dumps(window))

    def test_vulkan_observation_keeps_partial_transitions_after_an_early_crash(self):
        startup = log_line(LIFECYCLE) + log_line(FRAME) + log_line("host-memory revision=1 state=low")
        crash = log_line("FATAL EXCEPTION: private-observation-crash", tag="E/AndroidRuntime")
        status, report, commands = self.execute({LOGCAT: [startup, crash]}, ("--timeout", "3", "--observe-seconds", "2"),
                                                VULKAN_PROFILE)

        self.assertEqual((status, report["status"], report["error"]), (1, "partial", "application_crashed"))
        self.assertEqual(commands.count(LOGCAT), 1 + 2)  # Launch receipt, then startup and crash.
        observed = report["observed"]
        self.assertTrue(observed["crashObserved"])
        window = observed["observationWindow"]
        self.assertEqual((window["durationSeconds"], window["complete"], window["truncated"]), (2, False, False))
        self.assertIn({"category": "memory", "elapsedNs": 5_000_000, "revision": 1, "state": "low"}, window["transitions"])
        self.assertNotIn("private-observation-crash", json.dumps(report))

    def test_vulkan_observation_marks_a_full_transition_buffer_as_partial(self):
        log = log_line(LIFECYCLE) + log_line(FRAME)
        log += log_line("host-memory revision=1 state=normal")
        log += log_line("host-memory revision=2 state=low")
        with patch.object(project, "DEPLOY_TRANSITION_LIMIT", 2):
            status, report, _ = self.execute({LOGCAT: log}, ("--timeout", "2", "--observe-seconds", "1"), VULKAN_PROFILE)

        self.assertEqual((status, report["status"], report["error"]), (1, "partial", "observation_window_truncated"))
        window = report["observed"]["observationWindow"]
        self.assertEqual((window["durationSeconds"], window["complete"], window["truncated"]), (1, True, True))
        self.assertEqual(len(window["transitions"]), 2)

    def test_vulkan_observation_preserves_evidence_when_deadline_precedes_window(self):
        status, report, _ = self.execute(options=("--timeout", "1", "--observe-seconds", "2"), profile_name=VULKAN_PROFILE)
        self.assertEqual((status, report["error"]), (1, "observation_window_incomplete"))
        window = report["observed"]["observationWindow"]
        self.assertFalse(window["complete"])
        self.assertTrue(any(item["category"] == "lifecycle" for item in window["transitions"]))

    def test_pressure_timeline_marks_a_saturated_nonoverlapping_log_tail(self):
        start = project.log_epoch_nanoseconds(START)
        timeline = project.AndroidPressureTimeline(start)
        process = project.DeployedProcess(123, 1010200, 123456)
        log = log_line("non-transition marker") * project.DEPLOY_LOG_TAIL_LINES
        project.package_log_evidence(log, process, start, project.log_epoch_nanoseconds(END), ("memory",), timeline)
        self.assertEqual(timeline.report(1, True)["transitions"], [])
        self.assertTrue(timeline.report(1, True)["truncated"])

    def test_observation_requires_a_bounded_duration_and_vulkan_profile(self):
        for invalid in ("0", "121", "1.5"):
            with self.subTest(invalid=invalid), contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit) as raised:
                self.execute(options=("--observe-seconds", invalid), profile_name=VULKAN_PROFILE)
            self.assertEqual(raised.exception.code, 2)
        status, report, commands = self.execute(options=("--observe-seconds", "1"))
        self.assertEqual((status, report["error"], commands), (1, "observation_profile_unsupported", []))

    def test_device_clock_discontinuity_or_malformed_clock_cannot_supply_evidence(self):
        for final, expected in ((START, "device_log_clock_changed"),
                                ("1795000000.111000001", "device_log_clock_changed"),
                                ("1795000000.%N", "device_log_clock_unavailable")):
            with self.subTest(final=final):
                status, report, _ = self.execute({DEVICE_CLOCK: [START, END, final]})
                self.assertEqual((status, report["error"], report["cleanup"]), (1, expected, {"status": "stopped"}))
                self.assertNotIn("observed", report)

    def test_replacement_cannot_claim_a_matching_buffered_crash(self):
        status, report, _ = self.execute({
            STAT_QUERY: [proc_stat()] * 4 + [proc_stat(123457)] * 4,
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

    def test_malformed_cold_openxr_start_stops_only_with_the_exact_private_receipt(self):
        _, profile = project.load_contract(project.OPENXR_DEVELOPMENT_PROFILE, ["arm64-v8a"])
        application = profile["applicationId"]
        stop = ("shell", "am", "force-stop", "--user", "10", application)
        for uid, cleanup in (("1010200", {"status": "stopped"}),
                             ("10200", {"status": "unconfirmed", "reason": "launch_process_unattributed"})):
            with self.subTest(uid=uid):
                status, report, commands = self.execute(
                    {launch_command(application, openxr=True): f"LaunchState: COLD\nActivity: {application}/.PackageActivity"},
                    OPENXR_OPTIONS, project.OPENXR_DEVELOPMENT_PROFILE, launch_receipt=log_line(LAUNCH_MARKER, uid=uid))
                self.assertEqual((status, report["stage"], report["error"]), (1, "launch", "launch_unconfirmed"))
                self.assertNotIn("observed", report)
                self.assertNotIn(LAUNCH_NONCE, json.dumps(report))
                self.assertEqual(report["cleanup"], cleanup)
                self.assertEqual(stop in commands, cleanup["status"] == "stopped")

    def test_successful_start_still_requires_one_exact_private_cold_receipt(self):
        for receipt in ("", log_line(LAUNCH_MARKER, stamp=START), log_line(LAUNCH_MARKER, uid="10200"),
                        log_line(LAUNCH_MARKER, pid=999), log_line(LAUNCH_MARKER) * 2,
                        log_line(LAUNCH_MARKER.replace("startTicks=123456", "startTicks=123455"))):
            with self.subTest(receipt=receipt):
                status, report, commands = self.execute(launch_receipt=receipt)
                self.assertEqual((status, report["stage"], report["error"]), (1, "launch", "launch_process_unattributed"))
                self.assertEqual(report["cleanup"], {"status": "unconfirmed", "reason": "launch_process_unattributed"})
                self.assertNotIn("observed", report)
                self.assertNotIn(LAUNCH_NONCE, json.dumps(report))
                self.assertNotIn("startTicks", json.dumps(report))
                self.assertFalse(any("force-stop" in cmd for cmd in commands))

    def test_cold_receipt_qualifies_adb_without_a_launch_state_field(self):
        accepted = f"Status: ok\nActivity: {APPLICATION}/.PackageActivity"
        status, report, _ = self.execute({launch_command(APPLICATION): accepted})
        self.assertEqual((status, report["status"]), (0, "complete"))
        self.assertNotIn(LAUNCH_NONCE, json.dumps(report))
        self.assertNotIn("startTicks", json.dumps(report))

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

    def test_recycled_pid_cannot_reuse_the_old_cold_receipt_on_cleanup(self):
        status, report, commands = self.execute({
            launch_command(APPLICATION): KeyboardInterrupt(),
            LOGCAT: log_line(LAUNCH_MARKER),
            STAT_QUERY: proc_stat(123457),
        }, launch_reached_device=True)
        self.assertEqual((status, report["error"]), (130, "cancelled"))
        self.assertEqual(report["cleanup"], {"status": "unconfirmed", "reason": "launch_process_unattributed"})
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

    def test_windows_deployment_requests_owned_capture_and_reports_unconfirmed_retirement(self):
        from dev import CleanupError

        argv = ["adb.exe", "devices", "-l"]
        environment = {"TEST": "owned"}
        child = object()
        with patch.object(project, "os", SimpleNamespace(name="nt")), \
                patch.object(project, "admitted_command", return_value=argv), \
                patch.object(project, "execution_environment", return_value=environment), \
                patch("dev.owned_windows_process", return_value=contextlib.nullcontext(child)) as start, \
                patch.object(project, "read_deploy_process", return_value=(0, "inventory")) as read:
            deadline = time.monotonic() + 1
            self.assertEqual(project.deploy_process(argv, deadline, retain_adb_server=True), (0, "inventory"))
            start.assert_called_once_with(argv, env=environment, capture_output=True, retain_children_on_success=True)
            read.assert_called_once_with(child, deadline, retain_adb_server=True)
            start.side_effect = CleanupError("fixture retirement denied")
            with self.assertRaisesRegex(project.AndroidDeployError, "host_process_stop_unconfirmed"):
                project.deploy_process(argv, deadline)

    @unittest.skipUnless(os.name == "nt", "Requires native Windows Job Object containment")
    def test_windows_failed_deployment_retires_the_child_after_the_leader_exits(self):
        import ctypes
        from ctypes import wintypes

        kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
        kernel.OpenProcess.restype = wintypes.HANDLE
        kernel.WaitForSingleObject.argtypes = [wintypes.HANDLE, wintypes.DWORD]
        kernel.WaitForSingleObject.restype = wintypes.DWORD
        kernel.CloseHandle.argtypes = [wintypes.HANDLE]
        kernel.CloseHandle.restype = wintypes.BOOL
        read = project.read_deploy_process
        child_handle = None

        def observe(process, deadline, **options):
            nonlocal child_handle
            status, output = read(process, deadline, **options)
            child_handle = kernel.OpenProcess(0x00100000, False, int(output.strip()))  # SYNCHRONIZE
            self.assertTrue(child_handle)
            self.assertEqual(kernel.WaitForSingleObject(child_handle, 0), 258)  # WAIT_TIMEOUT
            return status, output

        launch = ("import subprocess,sys; "
                  "child=subprocess.Popen([sys.executable,'-c','import time; time.sleep(20)'], "
                  "stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL); "
                  "print(child.pid,flush=True); sys.exit(7)")
        try:
            with patch.object(project, "read_deploy_process", side_effect=observe):
                status, _ = project.deploy_process([sys.executable, "-c", launch], time.monotonic() + 5)
            self.assertEqual(status, 7)
            self.assertEqual(kernel.WaitForSingleObject(child_handle, 0), 0)  # WAIT_OBJECT_0
        finally:
            if child_handle:
                kernel.CloseHandle(child_handle)

    @unittest.skipUnless(hasattr(os, "waitid") and hasattr(os, "WNOWAIT"), "Requires POSIX child exit observation")
    def test_failed_host_command_retires_its_group_before_reaping(self):
        signal_group = os.killpg
        exit_statuses = []

        def record_and_stop(pid, signal_number):
            exited = os.waitid(os.P_PID, pid, os.WEXITED | os.WNOWAIT | os.WNOHANG)
            exit_statuses.append(exited.si_status)
            signal_group(pid, signal_number)

        with patch.object(os, "killpg", side_effect=record_and_stop):
            status, output = project.deploy_process(
                [sys.executable, "-c", "print('failed'); raise SystemExit(7)"], time.monotonic() + 5)
        self.assertEqual((status, output), (7, "failed\n"))
        self.assertEqual(exit_statuses, [7])

    @unittest.skipUnless(hasattr(os, "waitid") and hasattr(os, "WNOWAIT"), "Requires POSIX child exit observation")
    def test_ordinary_host_command_retires_descendant_without_output_pipe(self):
        signal_group = os.killpg
        signals = []

        def record_and_stop(pid, signal_number):
            signal_group(pid, signal_number)
            signals.append(signal_number)

        script = ("import os, sys, time\n"
                  "if os.fork() == 0:\n"
                  "    os.close(1); os.close(2); time.sleep(5); os._exit(0)\n"
                  "raise SystemExit(int(sys.argv[1]))\n")
        for status in (0, 7):
            with self.subTest(status=status), patch.object(os, "killpg", side_effect=record_and_stop):
                signals.clear()
                command = [sys.executable, "-c", script, str(status)]
                self.assertEqual(project.deploy_process(command, time.monotonic() + 5), (status, ""))
                self.assertEqual(signals, [project.signal.SIGKILL])

    @unittest.skipUnless(hasattr(os, "waitid") and hasattr(os, "WNOWAIT"), "Requires POSIX child exit observation")
    def test_only_successful_inventory_may_retain_its_server(self):
        for status in (0, 7):
            with self.subTest(status=status), patch.object(os, "killpg", wraps=os.killpg) as stop:
                command = [sys.executable, "-c", "import sys; sys.exit(int(sys.argv[1]))", str(status)]
                self.assertEqual(project.deploy_process(command, time.monotonic() + 5, retain_adb_server=True), (status, ""))
                self.assertEqual(stop.call_count, int(status != 0))

    @unittest.skipUnless(hasattr(os, "waitid") and hasattr(os, "WNOWAIT"), "Requires POSIX child exit observation")
    def test_deadline_retires_output_owner_after_group_leader_exits(self):
        spawn = project.subprocess.Popen
        make_thread = project.Thread
        readers = []

        def spawn_exited_leader(*args, **kwargs):
            child = spawn(*args, **kwargs)
            os.waitid(os.P_PID, child.pid, os.WEXITED | os.WNOWAIT)
            return child

        def record_reader(**kwargs):
            reader = make_thread(**kwargs)
            readers.append(reader)
            return reader

        # The descendant holds the output pipe after its parent exits.
        command = [sys.executable, "-c", "import os, time\nif os.fork() == 0:\n    time.sleep(5)"]
        with patch.object(project.subprocess, "Popen", side_effect=spawn_exited_leader), \
                patch.object(project, "Thread", side_effect=record_reader):
            with self.assertRaisesRegex(project.AndroidDeployError, "deadline_exceeded"):
                project.deploy_process(command, time.monotonic() + 0.75)
        self.assertEqual(len(readers), 1)
        readers[0].join(timeout=1)
        self.assertFalse(readers[0].is_alive())
