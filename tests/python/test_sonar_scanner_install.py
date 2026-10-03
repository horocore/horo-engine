from __future__ import annotations

import os
from pathlib import Path
import subprocess
import zipfile


INSTALLER = Path(__file__).resolve().parents[2] / ".github/scripts/install_sonar_scanner.sh"
VERSION = "7.2.0.5079"
SCANNER = f"sonar-scanner-{VERSION}-linux-x64"


def installer_environment(tmp_path: Path) -> dict[str, str]:
    return {**os.environ, "SONAR_SCANNER_VERSION": VERSION,
            "SONAR_USER_HOME": str(tmp_path / "scanner cache ü")}


def test_download_uses_cli_archive_and_preserves_extracted_directory(tmp_path: Path) -> None:
    archive = tmp_path / "scanner.zip"
    with zipfile.ZipFile(archive, "w") as output:
        executable = zipfile.ZipInfo(f"{SCANNER}/bin/sonar-scanner")
        executable.external_attr = 0o100755 << 16
        output.writestr(executable, "#!/bin/sh\nexit 0\n")
    tools = tmp_path / "tools"
    tools.mkdir()
    curl = tools / "curl"
    curl.write_text("""#!/usr/bin/env python3
import os
from pathlib import Path
import shutil
import sys
args = sys.argv[1:]
url = next(arg for arg in args if arg.startswith('https://'))
assert url == 'https://binaries.sonarsource.com/Distribution/sonar-scanner-cli/sonar-scanner-cli-7.2.0.5079-linux-x64.zip', url
shutil.copyfile(os.environ['TEST_SCANNER_ARCHIVE'], args[args.index('-o') + 1])
""")
    curl.chmod(0o755)
    environment = installer_environment(tmp_path)
    environment.update(PATH=f"{tools}{os.pathsep}{os.environ['PATH']}",
                       TEST_SCANNER_ARCHIVE=str(archive))
    subprocess.run(["bash", str(INSTALLER)], env=environment, check=True)
    scanner = Path(environment["SONAR_USER_HOME"]) / SCANNER / "bin/sonar-scanner"
    assert os.access(scanner, os.X_OK)
    subprocess.run([str(scanner)], check=True)


def test_executable_cache_hit_needs_no_download(tmp_path: Path) -> None:
    environment = installer_environment(tmp_path)
    scanner = Path(environment["SONAR_USER_HOME"]) / SCANNER / "bin/sonar-scanner"
    scanner.parent.mkdir(parents=True)
    scanner.write_text("#!/bin/sh\nexit 0\n")
    scanner.chmod(0o755)
    tools = tmp_path / "tools"
    tools.mkdir()
    curl = tools / "curl"
    curl.write_text("#!/bin/sh\nexit 99\n")
    curl.chmod(0o755)
    environment["PATH"] = f"{tools}{os.pathsep}{os.environ['PATH']}"
    subprocess.run(["bash", str(INSTALLER)], env=environment, check=True)
