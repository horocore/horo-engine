"""Pure coverage planner/publication contracts; execution belongs to the CI host."""
import ast
import importlib.util
import json
import os
from pathlib import Path
import sys

import pytest

pytest.importorskip("defusedxml.ElementTree")
SCRIPTS = Path(__file__).resolve().parents[2] / "scripts"
sys.path.insert(0, str(SCRIPTS))
try:
    SPEC = importlib.util.spec_from_file_location("collect_sonar_coverage", SCRIPTS / 'analyze.py')
    collector = importlib.util.module_from_spec(SPEC)
    sys.modules[SPEC.name] = collector
    SPEC.loader.exec_module(collector)
finally:
    sys.path.pop(0)


def fixture(tmp_path: Path, count: int = 130) -> tuple:
    root = tmp_path / "repo space örnek"
    build = root / "build" / "sonar"
    stage = build / "coverage-batches.test"
    stage.mkdir(parents=True)
    source = root / "src" / "shared template.cpp"
    source.parent.mkdir()
    source.write_text("callback();\nuntested();\n", encoding="utf-8")
    files = []
    for number in range(count):
        path = build / f"object {number:04d} örnek.gcno"
        path.touch()
        files.append(path)
    return root, build, stage, source, files


def read_manifest(path: Path) -> list[Path]:
    data = path.read_bytes()
    assert data.endswith(b"\0")
    return [Path(os.fsdecode(name)) for name in data[:-1].split(b"\0")]


@pytest.mark.parametrize("count", [1, 64, 65, 130])
def test_planner_accounts_for_every_input_once_with_bounded_nul_batches(tmp_path: Path, count: int) -> None:
    root, build, stage, _, files = fixture(tmp_path, count)
    metrics = collector.plan(root, build, stage)
    manifests = read_manifest(stage / "batches.manifest")
    observed = []
    for manifest in manifests:
        assert manifest.parent == stage
        batch = read_manifest(manifest)
        assert 0 < len(batch) <= 64
        observed.extend(batch)
    assert observed == sorted(files)
    assert len(set(observed)) == count
    assert metrics == {"inputs": count, "batches": (count + 63) // 64, "batchLimit": 64}
    before = (stage / "batches.manifest").read_bytes()
    assert collector.plan(root, build, stage) == metrics
    assert (stage / "batches.manifest").read_bytes() == before


def test_newlines_quotes_spaces_unicode_are_data_not_commands(tmp_path: Path) -> None:
    root, build, stage, _, _ = fixture(tmp_path, 0)
    names = ["line\nbreak.gcno", "quoted ' $() ; örnek.gcno"]
    files = [build / name for name in names]
    for file in files:
        file.touch()
    collector.plan(root, build, stage)
    assert read_manifest(read_manifest(stage / "batches.manifest")[0]) == sorted(files)


def report(source: Path) -> dict:
    return {"sources": {str(source): {"": {"lines": {"1": 9, "2": 0},
                                           "functions": {"shared": {"start_line": 1, "execution_count": 9}},
                                           "branches": {"1": [9, 0]}}}}}


def test_publication_retains_merged_counters_and_zero_hit_lines(tmp_path: Path) -> None:
    root, build, stage, source, _ = fixture(tmp_path, 0)
    data = report(source)
    (stage / "fastcov-coverage.json").write_text(json.dumps(data))
    assert collector.publish(root, build, stage) == {"lines": 2, "covered": 1, "excludedNoncode": 0}
    assert json.loads((build / "fastcov-coverage.json").read_text()) == data
    document = collector.element_tree.parse(build / "coverage.xml")
    assert [line.attrib for line in document.findall("file/lineToCover")] == [
        {"lineNumber": "1", "covered": "true"}, {"lineNumber": "2", "covered": "false"}]


@pytest.mark.parametrize("failure", ["missing", "invalid_json", "invalid_counter", "empty"])
@pytest.mark.parametrize("existing", [False, True])
def test_failed_data_validation_preserves_both_previous_reports(tmp_path: Path, failure: str, existing: bool) -> None:
    root, build, stage, source, _ = fixture(tmp_path, 0)
    merged = stage / "fastcov-coverage.json"
    data = report(source)
    if failure == "invalid_counter":
        data["sources"][str(source)][""]["lines"]["1"] = -1
    if failure == "empty":
        data["sources"] = {}
    if failure != "missing":
        merged.write_text("{" if failure == "invalid_json" else json.dumps(data))
    outputs = [build / "fastcov-coverage.json", build / "coverage.xml"]
    if existing:
        for output in outputs:
            output.write_bytes(b"previous complete report")
    with pytest.raises((OSError, ValueError)):
        collector.publish(root, build, stage)
    for output in outputs:
        assert output.read_bytes() == b"previous complete report" if existing else not output.exists()


def test_empty_inputs_do_not_create_manifest(tmp_path: Path) -> None:
    root, build, stage, _, _ = fixture(tmp_path, 0)
    with pytest.raises(ValueError, match="No gcno"):
        collector.plan(root, build, stage)
    assert not (stage / "batches.manifest").exists()


@pytest.mark.parametrize("kind", ["outside_build", "build_root", "stage_root", "stage_symlink"])
def test_invalid_ownership_area_is_rejected(tmp_path: Path, kind: str) -> None:
    root, build, stage, _, _ = fixture(tmp_path, 1)
    if kind == "outside_build":
        build = tmp_path
    elif kind == "build_root":
        build = root
    elif kind == "stage_root":
        stage = build
    else:
        link = build / "coverage-batches.link"
        link.symlink_to(stage, target_is_directory=True)
        stage = link
    with pytest.raises(ValueError):
        collector.plan(root, build, stage)


def test_publication_io_failure_is_terminal_not_a_transactional_pair(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    root, build, stage, source, _ = fixture(tmp_path, 0)
    (stage / "fastcov-coverage.json").write_text(json.dumps(report(source)))
    json_output = build / "fastcov-coverage.json"
    xml_output = build / "coverage.xml"
    json_output.write_bytes(b"previous JSON")
    xml_output.write_bytes(b"previous XML")
    replace = collector.os.replace

    def fail_xml(candidate: Path, output: Path) -> None:
        if output == xml_output:
            raise OSError("publication I/O failure")
        replace(candidate, output)

    monkeypatch.setattr(collector.os, 'replace', fail_xml)
    monkeypatch.setattr(sys, "argv", ["collector", "publish", "--root", str(root), "--build-directory", str(build), "--stage", str(stage)])
    with pytest.raises(SystemExit) as error:
        collector.collector_main()
    assert error.value.code == 2
    assert json.loads(json_output.read_text())["sources"]
    assert xml_output.read_bytes() == b"previous XML"
    workflow = (SCRIPTS.parent / ".github/workflows/sonar.yml").read_text()
    assert "!cancelled() && steps.coverage.outcome == 'success'" in workflow


COVERAGE_IMPORTS = {"argparse", "json", "os", "re", "pathlib.Path",
                    "xml.etree.ElementTree", "defusedxml.ElementTree"}
COVERAGE_GLOBALS = {"BATCH_FILES", "__doc__", "Path", "ValueError", "OSError", "KeyError", "TypeError",
                    "any", "bool", "bytes", "dict", "int", "isinstance", "len", "list", "print",
                    "range", "set", "sorted", "str", "tuple"}
COVERAGE_OS = {"os.fsencode", "os.replace", "os.path", "os.path.realpath", "os.sep"}


def coverage_import_bindings(node) -> list[tuple[str, str]]:
    """Retain every import, including rebinding within one statement."""
    if isinstance(node, ast.Import):
        return [(alias.asname or alias.name.split(".")[0], alias.name) for alias in node.names]
    if isinstance(node, ast.ImportFrom):
        return [(alias.asname or alias.name, f"{node.module}.{alias.name}") for alias in node.names]
    return []


def coverage_imports(nodes) -> dict[str, str]:
    """Map local import bindings to their fully qualified authority."""
    return dict(binding for node in nodes for binding in coverage_import_bindings(node))


def coverage_body_errors(name: str, node: ast.FunctionDef, imports: dict[str, str]) -> list[str]:
    """Reject local authority imports and non-data OS operations, even aliases."""
    bindings = dict(imports, **coverage_imports(ast.walk(node)))
    errors = [f"{name} imports {imported}" for child in ast.walk(node)
              for _, imported in coverage_import_bindings(child) if imported not in COVERAGE_IMPORTS]
    for child in ast.walk(node):
        if isinstance(child, ast.Attribute):
            root, separator, tail = ast.unparse(child).partition(".")
            attribute = bindings.get(root, root) + separator + tail
            if attribute.startswith("os.") and attribute not in COVERAGE_OS:
                errors.append(f"{name} reaches {attribute}")
    return errors


def coverage_reference_errors(name: str, references: set[str], imports: dict[str, str]) -> list[str]:
    """Classify only non-function references after graph edges are scheduled."""
    errors = []
    for reference in references:
        if reference in imports:
            if imports[reference] not in COVERAGE_IMPORTS:
                errors.append(f"{name} reaches import {imports[reference]}")
        elif reference not in COVERAGE_GLOBALS:
            errors.append(f"{name} reaches unapproved global {reference}")
    return errors


def coverage_authority_errors(source: str) -> list[str]:
    """Follow global references, including callback aliases, from the data API.

    Symbol tables distinguish local names such as ``output`` from unrelated
    global analysis functions. Merely importing a tool elsewhere in the unified
    CLI is harmless; making it reachable from the coverage API is not.
    """
    import symtable

    tree = ast.parse(source)
    functions = {node.name: node for node in tree.body if isinstance(node, ast.FunctionDef)}
    tables = {table.get_name(): table for table in symtable.symtable(source, "coverage", "exec").get_children()}
    imports = coverage_imports(tree.body)
    pending = ["collector_main", "plan", "publish"]
    visited = set()
    errors = []
    while pending:
        name = pending.pop()
        if name in visited:
            continue
        visited.add(name)
        if name not in functions:
            errors.append(f"Missing coverage function: {name}")
            continue
        node, table = functions[name], tables[name]
        references = {symbol.get_name() for symbol in table.get_symbols()
                      if symbol.is_global() and symbol.is_referenced()}
        pending.extend(references & functions.keys())
        errors.extend(coverage_reference_errors(name, references - functions.keys(), imports))
        errors.extend(coverage_body_errors(name, node, imports))
    return errors


@pytest.mark.parametrize("injected", [
    "subprocess.run(['gcov'])",
    "shutil.which('gcov')",
    "runner = os.system; runner('gcov')",
    "run(['gcov'], root)",
    "__import__('subprocess').run(['gcov'])",
    "coverage_hidden_launcher()",
    "import subprocess as duplicate; import json as duplicate",
    "import os as local_os; local_os.system('gcov')",
])
def test_coverage_authority_guard_rejects_reachable_execution(injected: str) -> None:
    source = (SCRIPTS / 'analyze.py').read_text()
    marker = "    _, build, stage = area(root, build, stage)"
    assert source.count(marker) == 1
    source = source.replace(marker, f"    {injected}\n{marker}")
    source += "\ndef coverage_hidden_launcher():\n    import subprocess as process\n    process.run(['gcov'])\n"
    assert coverage_authority_errors(source)


def test_python_api_has_no_tool_selection_or_process_launch_authority() -> None:
    source = (SCRIPTS / 'analyze.py').read_text()
    assert coverage_authority_errors(source) == []
    host = (SCRIPTS.parent / ".github/scripts/collect_sonar_coverage.sh").read_text()
    assert 'mapfile -d' in host
    assert '--gcda-files "${inputs[@]}"' in host
    assert '--include "${includes[@]}"' in host
    assert '--add-tracefile "${reports[@]}"' in host
    assert '--gcov gcov --jobs 4 --process-gcno' in host


@pytest.mark.parametrize("target_name", ["HoroAudioComponentPreviewHostTests", "HoroEditorNavigationBakeHostTests"])
def test_executable_host_test_sources_are_collected(target_name: str) -> None:
    """Application sources exercised by the headless host must reach the report."""
    repository = SCRIPTS.parent
    cmake = (repository / "tests/CMakeLists.txt").read_text()
    target = cmake.split(f"add_executable({target_name}", 1)[1].split(")", 1)[0]
    application_sources = [
        token.strip('"').removeprefix("${PROJECT_SOURCE_DIR}/")
        for token in target.split()
        if "${PROJECT_SOURCE_DIR}/apps/" in token
    ]
    assert application_sources
    host = (repository / ".github/scripts/collect_sonar_coverage.sh").read_text()
    for source in application_sources:
        assert f'"$workspace/{source}"' in host
