"""Run the foreground memory policy against a virtual clock without an Android device."""

from pathlib import Path
import re
import shutil
import subprocess  # nosec B404 - runs the local JDK with fixed arguments and repository-owned fixtures.

import pytest


def test_android_foreground_memory_observation(tmp_path):
    javac, java = shutil.which("javac"), shutil.which("java")
    if not javac or not java:
        pytest.skip("A local JDK is required for the Android memory policy test")
    version = subprocess.run([javac, "-version"], shell=False, capture_output=True, text=True, timeout=10)  # nosec B603
    major = re.match(r"javac (\d+)", version.stdout)
    if version.returncode or not major or int(major[1]) < 17:
        pytest.skip("JDK 17 or later is required for the Android memory policy test")
    root = Path(__file__).resolve().parents[2]
    package = Path("org/horocore/qualification")
    source = root / "apps/android-package-qualification/java" / package / "MemoryObservation.java"
    test = root / "tests/java" / package / "MemoryObservationTest.java"
    subprocess.run(  # nosec B603
        [javac, "--release", "17", "-d", str(tmp_path), str(source), str(test)], shell=False, check=True, timeout=30
    )
    subprocess.run(  # nosec B603
        [java, "-cp", str(tmp_path), "org.horocore.qualification.MemoryObservationTest"], shell=False, check=True, timeout=30
    )
