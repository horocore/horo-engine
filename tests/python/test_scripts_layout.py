"""Repository tooling has three real implementation files, not hidden modules."""
import ast
from pathlib import Path
import sys
import threading

import analyze

SCRIPTS = Path(__file__).resolve().parents[2] / "scripts"


def test_entire_scripts_tree_has_exactly_three_python_files():
    assert sorted(p.relative_to(SCRIPTS).as_posix() for p in SCRIPTS.rglob("*.py")) == [
        "analyze.py", "dev.py", "project.py"]
    for name in ("analyze", "dev", "project"):
        tree = ast.parse((SCRIPTS / f"{name}.py").read_text(encoding="utf-8"))
        assert sum(isinstance(n, (ast.FunctionDef, ast.ClassDef)) for n in tree.body) >= 5
        assert not any(isinstance(n, ast.Call) and isinstance(n.func, ast.Name)
                       and n.func.id == "exec" for n in ast.walk(tree))
    assert not (SCRIPTS / "hooks").exists()
    assert not list(SCRIPTS.rglob("*.sh"))


def test_unified_analysis_cli_exposes_commands_and_rejects_unverified_ports():
    result = analyze.native_run([sys.executable, str(SCRIPTS / "analyze.py"), "--help"],
                                SCRIPTS.parent, 10, None, threading.Event())
    assert result.returncode == 0, result.stderr
    for command in ("check", "doctor", "stop", "complexity"):
        assert command in result.stdout
    for args in (["check", "--port", "64120"], ["check", "--files", "x.cpp", "--dirty"],
                 ["complexity", "--files", "x.cpp", "--base", "main"]):
        result = analyze.native_run([sys.executable, str(SCRIPTS / "analyze.py"), *args],
                                    SCRIPTS.parent, 10, None, threading.Event())
        assert result.returncode == 2
        assert "error:" in result.stderr


def test_dev_setup_uses_native_build_options_without_installing(monkeypatch, tmp_path):
    import dev
    calls = []
    monkeypatch.setattr(dev, "run_build", lambda **kwargs: calls.append(kwargs) or 0)
    assert dev.main(["setup", "--dir", str(tmp_path), "--target", "HoroFoundation", "--jobs", "2", "--no-cache"]) == 0
    assert calls == [{"target": "HoroFoundation", "build_directory": tmp_path, "jobs": 2, "use_cache": False}]


def test_dev_open_pr_preserves_argument_boundaries(monkeypatch, tmp_path):
    import dev
    body = tmp_path / "body with spaces.md"
    body.write_text("reviewed body", encoding="utf-8")
    calls = []
    monkeypatch.setattr(dev, "execute_subprocess", lambda argv: calls.append(argv) or 0)
    assert dev.main(["open-pr", "title; no shell", str(body), "main", "topic", "milestone", "project space"]) == 0
    assert calls == [["gh", "pr", "create", "--title", "title; no shell", "--body-file", str(body),
                      "--base", "main", "--head", "topic", "--assignee", "@me", "--milestone", "milestone",
                      "--project", "project space"]]
