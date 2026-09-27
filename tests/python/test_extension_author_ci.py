"""Source-free author CI lock, archive and template contracts."""

import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import sys
from types import SimpleNamespace
import zipfile

import pytest


ROOT = Path(__file__).parents[2]


def load_script(name):
    path = ROOT / "scripts" / name
    spec = importlib.util.spec_from_file_location(name.removesuffix(".py"), path)
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


BOOTSTRAP = load_script("bootstrap_extension_ci.py")
AUTHOR_CI = load_script("run_extension_author_ci.py")
SCAFFOLDER = load_script("scaffold_extension.py")


def archive(entries):
    output = io.BytesIO()
    with zipfile.ZipFile(output, "w") as zipped:
        for name, content in entries.items():
            zipped.writestr(name, content)
    return output.getvalue()


def test_template_pins_actions_tools_platforms_and_no_secrets():
    template = (ROOT / "sdk/ci/extension-author-ci.yml").read_text(encoding="utf-8")
    assert all(platform in template for platform in BOOTSTRAP.PLATFORMS)
    assert "python-version: '3.12.7'" in template
    assert "cmake==3.31.6" in template
    assert "permissions:\n  contents: read" in template
    assert "${{ secrets." not in template
    assert "--commit ${{ github.sha }}" in template
    assert "--repository ${{ github.repository }}" in template
    for action in ("actions/checkout", "actions/setup-python", "actions/upload-artifact"):
        assert template.count(f"{action}@") == 1
        revision = template.split(f"{action}@", 1)[1][:40]
        assert len(revision) == 40
        assert all(character in "0123456789abcdef" for character in revision)


def test_bootstrap_passes_only_canonical_platform_literals():
    for platform in BOOTSTRAP.PLATFORMS:
        assert BOOTSTRAP.canonical_platform(platform) == platform
    with pytest.raises(ValueError, match="unsupported extension CI platform"):
        BOOTSTRAP.canonical_platform("linux-x64 --output /outside")


def test_bootstrap_reconstructs_only_fixed_width_lowercase_commit():
    commit = "0" * 39 + "a"
    assert BOOTSTRAP.canonical_commit(commit) == commit
    for malicious in ("--output", "a" * 39 + ";", "A" * 40, "a" * 41, "a" * 39 + "\n"):
        with pytest.raises(ValueError, match="source commit"):
            BOOTSTRAP.canonical_commit(malicious)


def test_bootstrap_reconstructs_only_bounded_repository_identity():
    assert BOOTSTRAP.canonical_repository("example/author") == "example/author"
    for malicious in ("--output /outside", "--output/repo", "example/author;exec", "example/author/extra",
                      "example/author\n--output", "example\\author"):
        with pytest.raises(ValueError, match="repository attribution"):
            BOOTSTRAP.canonical_repository(malicious)


def test_scaffold_carries_versioned_ci_contract_without_source_path(tmp_path):
    output = tmp_path / "author"
    SCAFFOLDER.write_project(output, "com.example.author", "Author", "1.0.0", "backend")
    assert (output / ".github/workflows/extension-ci.yml").is_file()
    assert (output / ".horo/ci/bootstrap.py").is_file()
    assert (output / ".horo/extension-ci.lock.json").is_file()
    assert (output / "horo-package.toml").is_file()
    assert "horo-package.toml" in (output / "CMakeLists.txt").read_text(encoding="utf-8")
    assert str(ROOT) not in "\n".join(path.read_text(encoding="utf-8") for path in output.rglob("*") if path.is_file())


def test_lock_rejects_unpinned_url_digest_and_partial_matrix(tmp_path):
    valid = {"schemaVersion": 1, "sdkVersion": "1.0.0", "platforms": {
        platform: {"url": "https://example.test/sdk.zip", "sha256": "a" * 64} for platform in BOOTSTRAP.PLATFORMS}}
    path = tmp_path / ".horo/extension-ci.lock.json"
    path.parent.mkdir()
    path.write_text(json.dumps(valid), encoding="utf-8")
    assert BOOTSTRAP.read_lock(tmp_path, "linux-x64") == ("1.0.0", "https://example.test/sdk.zip", "a" * 64)
    valid["platforms"]["linux-x64"]["url"] = "http://example.test/sdk.zip"
    path.write_text(json.dumps(valid), encoding="utf-8")
    with pytest.raises(ValueError, match="HTTPS"):
        BOOTSTRAP.read_lock(tmp_path, "linux-x64")
    valid["platforms"]["linux-x64"]["url"] = "https://example.test/sdk.zip"
    valid["platforms"]["linux-x64"]["sha256"] = "not-pinned"
    path.write_text(json.dumps(valid), encoding="utf-8")
    with pytest.raises(ValueError, match="SHA-256"):
        BOOTSTRAP.read_lock(tmp_path, "linux-x64")
    del valid["platforms"]["macos-arm64"]
    path.write_text(json.dumps(valid), encoding="utf-8")
    with pytest.raises(ValueError, match="four supported"):
        BOOTSTRAP.read_lock(tmp_path, "linux-x64")


def test_bootstrap_paths_and_identity_cannot_escape_author_project(tmp_path):
    lock = tmp_path / ".horo/extension-ci.lock.json"
    lock.parent.mkdir()
    lock.write_text("{}", encoding="utf-8")
    args = SimpleNamespace(project=tmp_path, lock=lock, output=tmp_path / "ci-artifacts",
                           commit="a" * 40, repository="example/author")
    assert BOOTSTRAP.author_paths(args) == (tmp_path, tmp_path / "ci-artifacts")
    args.lock = tmp_path / "../outside.json"
    with pytest.raises(ValueError, match="author project's .horo lock"):
        BOOTSTRAP.author_paths(args)
    args.lock = lock
    args.output = tmp_path / "../outside"
    with pytest.raises(ValueError, match="author project's ci-artifacts"):
        BOOTSTRAP.author_paths(args)
    args.output = tmp_path / "ci-artifacts"
    args.commit = "--fake-option"
    with pytest.raises(ValueError, match="source commit"):
        BOOTSTRAP.author_paths(args)
    args.commit = "a" * 40
    args.repository = "example/author;exec"
    with pytest.raises(ValueError, match="repository attribution"):
        BOOTSTRAP.author_paths(args)


def test_bootstrap_rejects_symlinked_lock(tmp_path):
    lock = tmp_path / ".horo/extension-ci.lock.json"
    lock.parent.mkdir()
    outside = tmp_path / "outside.json"
    outside.write_text("{}", encoding="utf-8")
    try:
        lock.symlink_to(outside)
    except OSError:
        pytest.skip("host cannot create symlinks")
    args = SimpleNamespace(project=tmp_path, lock=lock, output=tmp_path / "ci-artifacts",
                           commit="a" * 40, repository="example/author")
    with pytest.raises(ValueError, match="author project's .horo lock"):
        BOOTSTRAP.author_paths(args)


def test_sdk_download_digest_and_safe_extraction(monkeypatch, tmp_path):
    content = archive({"bin/tool": "safe", "share/metadata.json": "{}"})

    class Response(io.BytesIO):
        def geturl(self):
            return "https://example.test/sdk.zip"

    monkeypatch.setattr(BOOTSTRAP.urllib.request, "urlopen", lambda *_args, **_kwargs: Response(content))
    digest = hashlib.sha256(content).hexdigest()
    BOOTSTRAP.extract_sdk(BOOTSTRAP.fetch_archive("https://example.test/sdk.zip", digest), tmp_path)
    assert (tmp_path / "bin/tool").read_text(encoding="utf-8") == "safe"
    with pytest.raises(ValueError, match="SHA-256"):
        BOOTSTRAP.fetch_archive("https://example.test/sdk.zip", "0" * 64)


@pytest.mark.skipif(os.name == "nt", reason="POSIX executable-mode contract")
def test_sdk_extraction_preserves_tool_permission_and_rejects_symlinks(tmp_path):
    executable = zipfile.ZipInfo("bin/horo-package")
    executable.create_system = 3
    executable.external_attr = 0o100755 << 16
    output = io.BytesIO()
    with zipfile.ZipFile(output, "w") as zipped:
        zipped.writestr(executable, b"tool")
    BOOTSTRAP.extract_sdk(output.getvalue(), tmp_path)
    assert (tmp_path / "bin/horo-package").stat().st_mode & 0o111

    symlink = zipfile.ZipInfo("bin/alias")
    symlink.create_system = 3
    symlink.external_attr = 0o120777 << 16
    output = io.BytesIO()
    with zipfile.ZipFile(output, "w") as zipped:
        zipped.writestr(symlink, "../elsewhere")
    payload = output.getvalue()
    with pytest.raises(ValueError, match="unsafe"):
        BOOTSTRAP.extract_sdk(payload, tmp_path / "unsafe")


@pytest.mark.parametrize("entry", ("../escape", "/absolute", "bin/../../escape", "bin/./tool", "bin//tool", "C:/tool", "bin\\evil"))
def test_sdk_archive_rejects_unsafe_paths(entry, tmp_path):
    payload = archive({entry: "bad"})
    with pytest.raises(ValueError, match="unsafe"):
        BOOTSTRAP.extract_sdk(payload, tmp_path)


def test_runner_rejects_missing_and_unsafe_module_entries(tmp_path):
    with pytest.raises(ValueError, match="unsupported extension CI tool"):
        AUTHOR_CI.run(tmp_path, "sh", "-c", "exit 0")
    stage = tmp_path / "stage"
    stage.mkdir()
    manifest = {"id": "com.example.author", "modules": [{"entry": "../outside"}]}
    (stage / "extension.json").write_text(json.dumps(manifest), encoding="utf-8")
    with pytest.raises(ValueError, match="unsafe"):
        AUTHOR_CI.module_paths(stage)
    manifest["modules"][0]["entry"] = "bin/module.so"
    (stage / "extension.json").write_text(json.dumps(manifest), encoding="utf-8")
    with pytest.raises(ValueError, match="absent"):
        AUTHOR_CI.module_paths(stage)
    (stage / "bin").mkdir()
    (stage / "bin/module.so").write_bytes(b"module")
    assert AUTHOR_CI.module_paths(stage) == (stage / "bin/module.so",)


def test_runner_rejects_empty_test_report_and_mismatched_package_identity(tmp_path):
    report = tmp_path / "ctest.xml"
    report.write_text('<testsuite tests="0"/>', encoding="utf-8")
    with pytest.raises(ValueError, match="no executed"):
        AUTHOR_CI.require_executed_tests(report)
    report.write_text('<testsuite tests="1"><testcase name="contract"/></testsuite>', encoding="utf-8")
    AUTHOR_CI.require_executed_tests(report)

    report.write_text('<testsuite><testcase name="skipped"><skipped/></testcase></testsuite>', encoding="utf-8")
    with pytest.raises(ValueError, match="no executed"):
        AUTHOR_CI.require_executed_tests(report)

    report.write_text('<!DOCTYPE testsuite [<!ENTITY expanded "unsafe">]>'
                      '<testsuite><testcase name="&expanded;"/></testsuite>', encoding="utf-8")
    with pytest.raises(ValueError, match="DTD or entity"):
        AUTHOR_CI.require_executed_tests(report)
    report.write_bytes(b" " * (AUTHOR_CI.MAXIMUM_JUNIT_BYTES + 1))
    with pytest.raises(ValueError, match="bounded parse size"):
        AUTHOR_CI.require_executed_tests(report)

    stage = tmp_path / "stage"
    stage.mkdir()
    (stage / "extension.json").write_text(json.dumps({"id": "com.example.author", "modules": [
        {"version": "1.0.0"}]}), encoding="utf-8")
    (stage / "horo-package.toml").write_text('schemaVersion = 1\n[package]\nid = "com.example.other"\nversion = "1.0.0"\n',
                                             encoding="utf-8")
    with pytest.raises(ValueError, match="identities disagree"):
        AUTHOR_CI.package_identity(stage)
    (stage / "horo-package.toml").write_text('schemaVersion = 1\n[package]\nid = "com.example.author"\nversion = "1.0.0"\n',
                                             encoding="utf-8")
    assert AUTHOR_CI.package_identity(stage) == "com.example.author"


def test_bootstrap_checks_downloaded_sdk_version_before_runner(monkeypatch, tmp_path):
    sdk = archive({
        "share/horo/extension-sdk/extension-sdk.json": json.dumps({"sdk": {"version": "1.2.3"}}),
        "bin/horo-extension-author-ci.py": "print('fixture')",
        "bin/untrusted-alternative.py": "print('not the runner')",
    })
    digest = hashlib.sha256(sdk).hexdigest()
    lock = {"schemaVersion": 1, "sdkVersion": "1.2.3", "platforms": {
        platform: {"url": "https://example.test/sdk.zip", "sha256": digest} for platform in BOOTSTRAP.PLATFORMS}}
    lock_path = tmp_path / ".horo/extension-ci.lock.json"
    lock_path.parent.mkdir()
    lock_path.write_text(json.dumps(lock), encoding="utf-8")

    class Response(io.BytesIO):
        def geturl(self):
            return "https://example.test/sdk.zip"

    monkeypatch.setattr(BOOTSTRAP.urllib.request, "urlopen", lambda *_args, **_kwargs: Response(sdk))
    monkeypatch.setattr(BOOTSTRAP.host_platform, "system", lambda: "Linux")
    monkeypatch.setattr(BOOTSTRAP.host_platform, "machine", lambda: "x86_64")
    launched = []

    def record(command, check, shell):
        launched.append((command, check, shell))
        return type("Exit", (), {"returncode": 0})()

    monkeypatch.setattr(BOOTSTRAP.subprocess, "run", record)
    monkeypatch.setattr(sys, "argv", ["bootstrap.py", "--lock", str(lock_path), "--platform", "linux-x64",
                                       "--project", str(tmp_path), "--output", str(tmp_path / "ci-artifacts"),
                                       "--repository", "example/author", "--commit", "a" * 40])
    assert BOOTSTRAP.main() == 0
    assert len(launched) == 1
    assert launched[0][2] is False
    assert launched[0][0][0] == sys.executable
    assert Path(launched[0][0][1]).name == "horo-extension-author-ci.py"
    sdk_root = Path(launched[0][0][launched[0][0].index("--sdk") + 1])
    assert Path(launched[0][0][1]) == sdk_root / "bin/horo-extension-author-ci.py"
    assert launched[0][0][launched[0][0].index("--platform") + 1] == "linux-x64"
    assert launched[0][0][launched[0][0].index("--commit") + 1] == "a" * 40
    assert launched[0][0][launched[0][0].index("--repository") + 1] == "example/author"
    assert "--sdk-sha256" in launched[0][0]
    assert digest in launched[0][0]

    sys.argv[sys.argv.index("--commit") + 1] = "a" * 39 + ";"
    assert BOOTSTRAP.main() == 2
    assert len(launched) == 1
    sys.argv[sys.argv.index("--commit") + 1] = "a" * 40

    sys.argv[sys.argv.index("--repository") + 1] = "--output /outside"
    assert BOOTSTRAP.main() == 2
    assert len(launched) == 1
    sys.argv[sys.argv.index("--repository") + 1] = "example/author"

    lock["sdkVersion"] = "9.9.9"
    lock_path.write_text(json.dumps(lock), encoding="utf-8")
    assert BOOTSTRAP.main() == 2
    assert len(launched) == 1


def test_runner_build_stage_requires_a_real_executed_test(monkeypatch, tmp_path):
    scratch = tmp_path / "scratch"
    scratch.mkdir()
    commands = []

    def record(_sdk, tool, *arguments):
        commands.append((tool, arguments))
        if tool == "ctest":
            junit = Path(arguments[arguments.index("--output-junit") + 1])
            junit.write_text('<testsuite><testcase name="contract"/></testsuite>', encoding="utf-8")

    monkeypatch.setattr(AUTHOR_CI, "run", record)
    stage = AUTHOR_CI.build_and_stage(tmp_path / "sdk", tmp_path / "project", scratch)
    assert stage == scratch / "stage"
    assert [tool for tool, _args in commands] == ["cmake", "cmake", "ctest", "cmake"]


def test_runner_packages_and_publishes_attributable_artifacts(monkeypatch, tmp_path):
    sdk, scratch, stage = tmp_path / "sdk", tmp_path / "scratch", tmp_path / "stage"
    scratch.mkdir()
    (stage / "bin").mkdir(parents=True)
    (stage / "bin/module.so").write_bytes(b"module")
    (stage / "extension.json").write_text(json.dumps({"id": "com.example.author", "modules": [
        {"entry": "bin/module.so", "version": "1.0.0"}]}), encoding="utf-8")
    (stage / "horo-package.toml").write_text('schemaVersion = 1\n[package]\nid = "com.example.author"\n'
                                             'version = "1.0.0"\n', encoding="utf-8")
    (scratch / "ctest.xml").write_text('<testsuite><testcase name="contract"/></testsuite>', encoding="utf-8")
    commands = []

    def record(_sdk, tool, *arguments):
        commands.append((tool, arguments))
        if tool == "horo-package" and arguments[0] == "pack":
            Path(arguments[2]).write_bytes(b"canonical package")

    monkeypatch.setattr(AUTHOR_CI, "run", record)
    output = tmp_path / "project/ci-artifacts"
    args = SimpleNamespace(repository="example/author", commit="a" * 40, platform="linux-x64",
                           sdk_sha256="b" * 64)
    AUTHOR_CI.package_and_publish(sdk, stage, scratch, output, args, "1.0.0")
    provenance = json.loads((output / "provenance.json").read_text(encoding="utf-8"))
    assert provenance["repository"] == "example/author"
    assert provenance["commit"] == "a" * 40
    assert provenance["packageId"] == "com.example.author"
    assert provenance["artifactSha256"] == hashlib.sha256(b"canonical package").hexdigest()
    assert provenance["verification"] == "archive-integrity-unsigned"
    assert (output / "extension.horopkg").read_bytes() == b"canonical package"
    assert (output / "ctest.xml").is_file()
    assert [tool for tool, _arguments in commands] == ["horo-extension-validate",
                                                    "horo-extension-conformance", "horo-package", "horo-package"]


def test_runner_main_rejects_traversal_and_accepts_fixed_artifact_path(monkeypatch, tmp_path):
    sdk, project = tmp_path / "sdk", tmp_path / "project"
    (sdk / "share/horo/extension-sdk").mkdir(parents=True)
    (sdk / "share/horo/extension-sdk/extension-sdk.json").write_text(
        json.dumps({"sdk": {"version": "1.0.0"}}), encoding="utf-8")
    project.mkdir()
    arguments = ["runner.py", "--sdk", str(sdk), "--project", str(project), "--output",
                 str(tmp_path / "outside"), "--platform", "linux-x64", "--repository", "example/author",
                 "--commit", "a" * 40, "--sdk-sha256", "b" * 64]
    monkeypatch.setattr(sys, "argv", arguments)
    assert AUTHOR_CI.main() == 2
    redirected = tmp_path / "redirected"
    redirected.mkdir()
    try:
        (project / "ci-artifacts").symlink_to(redirected, target_is_directory=True)
    except OSError:
        pass
    else:
        arguments[arguments.index("--output") + 1] = str(project / "ci-artifacts")
        assert AUTHOR_CI.main() == 2
        (project / "ci-artifacts").unlink()
    arguments[arguments.index("--output") + 1] = str(project / "ci-artifacts")
    called = []
    monkeypatch.setattr(AUTHOR_CI, "build_and_stage", lambda *_args: called.append("build") or tmp_path / "stage")
    monkeypatch.setattr(AUTHOR_CI, "package_and_publish", lambda *_args: called.append("package"))
    assert AUTHOR_CI.main() == 0
    assert called == ["build", "package"]
