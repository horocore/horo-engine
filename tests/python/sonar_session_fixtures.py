"""Disposable Code profile inputs and process identities shared by session tests."""

def profile_inputs(tmp_path):
    source = tmp_path / "source Code"
    (source / "User").mkdir(parents=True)
    extensions = tmp_path / "extensions"
    extension = extensions / "sonarsource.sonarlint-vscode-6.1.1-win32-x64"
    extension.mkdir(parents=True)
    (extension / "package.json").write_text('{}', encoding="utf-8")
    profile = tmp_path / "managed" / "profile"
    return source, extensions, profile



def code_tree(profile, platform_name="win32"):
    executable = "C:/Code/Code.exe" if platform_name == "win32" else "/usr/share/code/code"
    program = "Code.exe" if platform_name == "win32" else "code"
    record = {"pid": 10, "started": "100", "profile": str(profile), "nonce": "nonce",
              "executable": executable}
    table = {
        10: {"parent": 1, "started": "100", "executable": record["executable"],
             "argv": [program, "--user-data-dir", str(profile)], "markers": {}},
        11: {"parent": 10, "started": "110", "executable": executable,
             "argv": [program, "--type=extensionHost"], "markers": {
                 "HORO_QUALITY_PROFILE": str(profile), "HORO_QUALITY_SESSION": "nonce"}},
    }
    return record, table
