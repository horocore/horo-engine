"""Protect write-token isolation and unchanged qualification work across main/PR jobs."""

from __future__ import annotations

import ast
import operator
import os
from pathlib import Path
import re
import shutil
import subprocess
import textwrap

import pytest


ROOT = Path(__file__).resolve().parents[2]


def job(workflow: str, name: str) -> str:
    match = re.search(rf"^  {re.escape(name)}:\n(.*?)(?=^  [\w-]+:|\Z)",
                      workflow, re.MULTILINE | re.DOTALL)
    assert match is not None
    return match[1]


def condition(block: str, context: dict[str, str], *, cancelled: bool = False) -> bool:
    """Interpret the token guards with an explicit, non-executing cancellation status."""
    match = re.search(r"^    if: \$\{\{ (.*?) \}\}$", block, re.MULTILINE)
    assert match is not None
    expression = match[1]
    for name, value in context.items():
        expression = expression.replace(name, repr(value))
    expression = expression.replace("&&", " and ").replace("||", " or ")
    expression = re.sub(r"!(?!=)", "not ", expression).strip()
    return bool(boolean_value(ast.parse(expression, mode="eval").body, cancelled=cancelled))


def boolean_value(node: ast.AST, *, cancelled: bool | None = None) -> str | bool:
    """Model only the closed zero-argument status call; never execute workflow code."""
    if (isinstance(node, ast.Call) and isinstance(node.func, ast.Name)
            and node.func.id == "cancelled" and not node.args and not node.keywords
            and cancelled is not None):
        return cancelled
    if isinstance(node, ast.Constant):
        return node.value
    if isinstance(node, ast.BoolOp):
        return logical_value(node, cancelled=cancelled)
    if isinstance(node, ast.UnaryOp) and isinstance(node.op, ast.Not):
        return not boolean_value(node.operand, cancelled=cancelled)
    if isinstance(node, ast.Compare) and len(node.ops) == 1:
        return comparison_value(node, cancelled=cancelled)
    raise ValueError("Unexpected token-guard expression")


def logical_value(node: ast.BoolOp, *, cancelled: bool | None = None) -> str | bool:
    """Preserve operand values for Actions' conditional group-key selection."""
    values = [boolean_value(value, cancelled=cancelled) for value in node.values]
    short_circuit = isinstance(node.op, ast.Or)
    return next((value for value in values if bool(value) == short_circuit), values[-1])


def comparison_value(node: ast.Compare, *, cancelled: bool | None = None) -> bool:
    """Restrict comparisons to the two string equality operators used by Actions."""
    operation = {ast.Eq: operator.eq, ast.NotEq: operator.ne}.get(type(node.ops[0]))
    if operation is None:
        raise ValueError("Unexpected token-guard comparison")
    return operation(boolean_value(node.left, cancelled=cancelled),
                     boolean_value(node.comparators[0], cancelled=cancelled))


def concurrency_key(block: str, context: dict[str, str]) -> str:
    """Resolve the actual workflow group with the same restricted expression subset."""
    match = re.search(r"^\s+group: (.*)$", block, re.MULTILINE)
    assert match is not None

    def interpolate(expression: re.Match[str]) -> str:
        source = expression[1].strip()
        for name, value in context.items():
            source = source.replace(name, repr(value))
        source = source.replace("&&", " and ").replace("||", " or ")
        return str(boolean_value(ast.parse(source, mode="eval").body))

    return re.sub(r"\$\{\{(.*?)\}\}", interpolate, match[1])


def ci_context(ref: str, run: str, platform: str = "Linux / GCC") -> dict[str, str]:
    return {"github.workflow": "CI", "github.ref": ref, "github.run_id": run,
            "matrix.name": platform, "matrix.os": platform}


def test_pr_burst_keeps_obsolete_commit_cancellation_scoped_to_each_ref() -> None:
    source = (ROOT / ".github/workflows/ci.yml").read_text(encoding="utf-8")
    workflow = source.split("\njobs:", 1)[0]
    keys = set()
    for number in range(1, 11):
        ref = f"refs/pull/{number}/merge"
        first = concurrency_key(workflow, ci_context(ref, f"{number}01"))
        assert first == concurrency_key(workflow, ci_context(ref, f"{number}02"))
        keys.add(first)
    assert len(keys) == 10
    assert "cancel-in-progress: ${{ github.event_name == 'pull_request' }}" in workflow
    main = "refs/heads/main"
    assert concurrency_key(workflow, ci_context(main, "101")) != concurrency_key(workflow, ci_context(main, "102"))


@pytest.mark.parametrize("name", ["tooling", "android-storage", "shaders", "directx-resources"])
def test_main_support_lanes_coalesce_without_cancelling_other_prs(name: str) -> None:
    source = (ROOT / ".github/workflows/ci.yml").read_text(encoding="utf-8")
    block = job(source, name)
    main = "refs/heads/main"
    assert concurrency_key(block, ci_context(main, "101")) == concurrency_key(block, ci_context(main, "102"))
    assert "      cancel-in-progress: false\n" in block
    keys = {concurrency_key(block, ci_context(f"refs/pull/{number}/merge", str(number)))
            for number in range(1, 11)}
    assert len(keys) == 10


def test_main_native_writers_serialize_by_platform_without_global_head_of_line_wait() -> None:
    source = (ROOT / ".github/workflows/ci.yml").read_text(encoding="utf-8")
    block = job(source, "test-main")
    keys = set()
    for platform in ("Linux / GCC", "Windows / MSVC"):
        context = ci_context("refs/heads/main", "101", platform)
        key = concurrency_key(block, context)
        context["github.run_id"] = "102"
        assert concurrency_key(block, context) == key
        keys.add(key)
    macos = job(source, "macos-build-main")
    context = ci_context("refs/heads/main", "101", "macOS / Clang")
    macos_key = concurrency_key(macos, context)
    context["github.run_id"] = "102"
    assert concurrency_key(macos, context) == macos_key
    keys.add(macos_key)
    assert len(keys) == 3
    assert "      cancel-in-progress: false\n" in macos
    assert "    concurrency:" not in job(source, "macos-build")
    assert "      cancel-in-progress: false\n" in block
    assert "    concurrency:" not in job(source, "test")
    shader = job(source, "shaders")
    assert concurrency_key(shader, ci_context("refs/heads/main", "101", "ubuntu-26.04")) != concurrency_key(
        shader, ci_context("refs/heads/main", "102", "windows-2022"))


@pytest.mark.parametrize("workflow,readonly,writer", [
    ("ci", "test", "test-main"), ("sonar", "sonar", "sonar-main"),
    ("ci", "macos-build", "macos-build-main"),
    ("ci", "shaders", "shaders-main"),
])
@pytest.mark.parametrize("repository", ["horocore/horo-engine", "fork/horo-engine"])
@pytest.mark.parametrize("ref", ["refs/heads/main", "refs/heads/topic", "refs/pull/17/merge"])
@pytest.mark.parametrize("event", ["push", "pull_request", "workflow_dispatch"])
def test_only_canonical_main_selects_the_write_job(
    workflow: str, readonly: str, writer: str, repository: str, ref: str, event: str,
) -> None:
    source = (ROOT / f".github/workflows/{workflow}.yml").read_text(encoding="utf-8")
    context = {"github.repository": repository, "github.ref": ref,
               "github.event_name": event,
               "github.event.pull_request.head.repo.full_name": repository}
    context["needs.tooling.result"] = "success"
    expected = (repository == "horocore/horo-engine" and ref == "refs/heads/main"
                and event in {"push", "workflow_dispatch"})
    assert condition(job(source, writer), context) is expected
    assert condition(job(source, readonly), context) is not expected


@pytest.mark.parametrize("writer", [False, True])
@pytest.mark.parametrize("cancelled", [False, True])
@pytest.mark.parametrize("tooling", ["success", "failure", "cancelled", "skipped"])
@pytest.mark.parametrize("native_job", ["test", "macos-build"])
def test_native_status_guards_require_uncancelled_successful_tooling(
    writer: bool, cancelled: bool, tooling: str, native_job: str,
) -> None:
    source = (ROOT / ".github/workflows/ci.yml").read_text(encoding="utf-8")
    context = {"github.repository": "horocore/horo-engine",
               "github.ref": "refs/heads/main" if writer else "refs/heads/topic",
               "github.event_name": "push", "needs.tooling.result": tooling}
    permitted = not cancelled and tooling == "success"
    assert condition(job(source, native_job + "-main"), context, cancelled=cancelled) is (writer and permitted)
    assert condition(job(source, native_job), context, cancelled=cancelled) is (not writer and permitted)


@pytest.mark.parametrize("expression", ["unknown()", "cancelled(True)", "cancelled(flag=True)", "obj.cancelled()"])
def test_status_model_rejects_arbitrary_or_parameterized_calls(expression: str) -> None:
    parsed = ast.parse(expression, mode="eval").body
    with pytest.raises(ValueError, match="Unexpected token-guard expression"):
        boolean_value(parsed, cancelled=False)


def test_foreign_fork_sonar_policy_is_preserved() -> None:
    source = (ROOT / ".github/workflows/sonar.yml").read_text(encoding="utf-8")
    context = {"github.repository": "horocore/horo-engine", "github.ref": "refs/pull/17/merge",
               "github.event_name": "pull_request",
               "github.event.pull_request.head.repo.full_name": "fork/horo-engine"}
    assert condition(job(source, "sonar"), context) is False
    assert condition(job(source, "sonar-main"), context) is False


@pytest.mark.parametrize("workflow,readonly,writer,fields", [
    ("ci", "test", "test-main", ("steps", "env", "strategy", "defaults")),
    ("ci", "shaders", "shaders-main", ("steps", "env", "strategy", "defaults")),
    ("sonar", "sonar", "sonar-main", ("steps", "env", "runs-on")),
])
def test_both_jobs_share_the_complete_pipeline_with_distinct_permissions(
    workflow: str, readonly: str, writer: str, fields: tuple[str, ...],
) -> None:
    source = (ROOT / f".github/workflows/{workflow}.yml").read_text(encoding="utf-8")
    reader = job(source, readonly)
    main = job(source, writer)
    assert "      packages: read\n" in reader
    assert "packages: write" not in reader
    assert "      packages: write\n" in main
    timeout = 90 if readonly == "shaders" else 180
    assert f"    timeout-minutes: {timeout}\n" in reader
    assert f"    timeout-minutes: {timeout}\n" in main
    for field in fields:
        match = re.search(rf"^    {field}: &(\w+)", reader, re.MULTILINE)
        assert match is not None
        assert f"    {field}: *{match[1]}\n" in main
    if readonly == "test":
        assert "max-parallel: 2" in reader
        assert reader.count("          - name:") == 2
        matrix = reader.split("    strategy:", 1)[1].split("    steps:", 1)[0]
        assert "- name: Linux / GCC" in matrix
        assert "- name: Windows / MSVC" in matrix
        assert "- name: macOS / Clang" not in matrix
    if readonly == "shaders":
        assert "max-parallel: 1" in reader
        assert reader.count("          - os:") == 2
    assert "cancel-in-progress: ${{ github.event_name == 'pull_request' }}" in source
    group = re.search(r"^  group: (.*)$", source, re.MULTILINE)
    assert group is not None
    assert "${{ github.workflow }}" in group[1]
    assert "github.ref" in group[1]
    if workflow == "sonar":
        assert "run_id" not in group[1]


def step(source: str, name: str) -> str:
    return source.split(f"      - name: {name}\n", 1)[1].split("\n      - name:", 1)[0]


@pytest.mark.parametrize("cancelled", [False, True])
@pytest.mark.parametrize("cache", ["success", "failure", "cancelled", "skipped"])
@pytest.mark.parametrize("debug", ["success", "failure", "cancelled", "skipped"])
def test_auxiliary_restore_survives_debug_failure_without_relaxing_primary_publication(
    cancelled: bool, cache: str, debug: str,
) -> None:
    source = job((ROOT / ".github/workflows/ci.yml").read_text(encoding="utf-8"), "test")
    context = {"steps.cache.outcome": cache, "steps.debug_build.outcome": debug,
               "github.repository": "horocore/horo-engine", "github.ref": "refs/heads/main"}
    restore = "\n".join(line[4:] for line in step(source, "Restore auxiliary compiler variants").splitlines())
    assert condition(restore, context, cancelled=cancelled) is (not cancelled and cache == "success")
    publication = "\n".join(line[4:] for line in step(source, "Publish main Debug compiler checkpoint").splitlines())
    assert condition(publication, context, cancelled=cancelled) is (not cancelled and debug == "success")


@pytest.mark.parametrize("compiler,dependency,flush", [
    ("false", "true", False), ("false", "false", True),
    ("true", "true", True), ("true", "false", True),
])
def test_dependency_only_checkpoint_avoids_flush_without_hiding_compiler_failures(
    tmp_path: Path, compiler: str, dependency: str, flush: bool,
) -> None:
    action = (ROOT / ".github/actions/save-native-cache/action.yml").read_text(encoding="utf-8")
    block = action.split("    - name: Flush and report compiler cache\n", 1)[1].split("\n    - name:", 1)[0]
    guard = re.search(r"^      if: (.*)$", block, re.MULTILINE)
    assert guard is not None
    selected = condition("    if: ${{ " + guard[1] + " }}",
                         {"inputs.compiler-ready": compiler, "inputs.dependency-ready": dependency})
    assert selected is flush
    host_bash = shutil.which("bash")
    assert host_bash is not None
    bash = Path(host_bash).resolve(strict=True)
    assert bash.is_absolute() and bash.is_file() and os.access(bash, os.X_OK)
    executable = tmp_path / "sccache"
    calls = tmp_path / "calls"
    executable.write_text('#!/bin/sh\nprintf "%s\\n" "$1" >> "$CACHE_CALLS"\n'
                          'if [ "$1" = --stop-server ]; then exit 2; fi\n', encoding="utf-8")
    executable.chmod(0o700)
    if selected:
        script = textwrap.dedent(block.split("      run: |\n", 1)[1])
        environment = dict(os.environ, PATH=str(tmp_path) + os.pathsep + os.environ["PATH"],
                           RUNNER_PLATFORM="Windows", CACHE_CALLS=str(calls))
        # Only the reviewed repository action body and fixed test-owned cache executable are dispatched.
        completed = subprocess.run([str(bash), "-e", "-o", "pipefail", "-c", script],
                                   env=environment, capture_output=True, check=False, shell=False, timeout=5)
        assert completed.returncode == 2
        assert calls.read_text(encoding="utf-8").splitlines() == ["--show-stats", "--stop-server"]
    else:
        assert not calls.exists()
    workflow = job((ROOT / ".github/workflows/ci.yml").read_text(encoding="utf-8"), "test")
    assert "compiler-ready: 'false'" in step(workflow, "Checkpoint complete dependency sources")
    assert "if: github.ref == 'refs/heads/main' && inputs.dependency-hit != 'true' && inputs.dependency-ready == 'true'" in action
    assert "if: github.ref == 'refs/heads/main' && inputs.compiler-ready == 'true'" in action


def test_compiled_native_stores_publish_before_the_next_phase_without_actions_copies() -> None:
    source = job((ROOT / ".github/workflows/ci.yml").read_text(encoding="utf-8"), "test")
    assert source.index("name: Build Debug") < source.index("name: Publish main Debug compiler checkpoint")
    assert source.index("name: Publish main Debug compiler checkpoint") < source.index("name: Restore auxiliary compiler variants")
    assert source.index("name: Build OpenXR lifecycle and header boundaries") < source.index("name: Publish main auxiliary compiler checkpoint")
    assert source.index("name: Publish main auxiliary compiler checkpoint") < source.index("name: Test Debug")
    for name in ("Restore dependency and compiler caches", "Restore auxiliary compiler variants"):
        assert "compiler-storage: registry" in step(source, name)
    for name in ("Checkpoint main Debug compiler cache", "Checkpoint auxiliary compiler variants"):
        assert "compiler-ready: 'false'" in step(source, name)
    for name in ("Publish main Debug compiler checkpoint", "Publish main auxiliary compiler checkpoint"):
        block = step(source, name)
        assert "!cancelled()" in block
        assert "github.ref == 'refs/heads/main'" in block
        assert "continue-on-error: true" in block
        assert "uses: ./.github/actions/publish-compiler-checkpoint" in block


def test_shader_checkpoint_has_its_own_profile_and_publishes_only_compiled_state() -> None:
    source = (ROOT / ".github/workflows/ci.yml").read_text(encoding="utf-8")
    shader = job(source, "shaders")
    restore = step(shader, "Restore compatible main dependency and compiler caches")
    assert "id: shader_cache" in restore
    assert "profile: native-shaders-v1" in restore
    assert "dependency-profile: native-tests-v1" in restore
    assert "compiler-storage: registry" in restore
    publication = step(shader, "Publish main shader compiler checkpoint")
    assert "github.ref == 'refs/heads/main'" in publication
    assert "github.repository == 'horocore/horo-engine'" in publication
    assert "github.event_name == 'push' || github.event_name == 'workflow_dispatch'" in publication
    assert "steps.shader_cache.outputs.compiler-key" in publication
    assert "continue-on-error: true" in publication
    assert "timeout-minutes: 4" in publication
    assert "uses: ./.github/actions/publish-compiler-checkpoint" in publication
    assert shader.index("name: Build Vulkan command smoke") < shader.index("name: Publish main shader compiler checkpoint")
    assert shader.index("name: Publish main shader compiler checkpoint") < shader.index("name: Run Vulkan native-call")
    assert "continue-on-error" not in shader.replace(publication, "")
    main = job(source, "shaders-main")
    for platform in ("ubuntu-26.04", "windows-2022"):
        assert concurrency_key(main, ci_context("refs/heads/main", "101", platform)) == concurrency_key(
            main, ci_context("refs/heads/main", "102", platform))
    assert "cancel-in-progress: false" in main


def test_sonar_publishes_compilation_before_coverage_without_relaxing_analysis() -> None:
    source = (ROOT / ".github/workflows/sonar.yml").read_text(encoding="utf-8")
    assert source.index("name: Build instrumented targets") < source.index("name: Publish main instrumented compiler checkpoint")
    assert source.index("name: Publish main instrumented compiler checkpoint") < source.index("name: Run C++ coverage tests")
    assert "Save compiler cache" not in source
    assert "steps.compiler.outputs" not in source
    assert "-Dsonar.qualitygate.wait=true" in source
    assert "-Dsonar.cfamily.analysisCache.mode=server" in source
    assert "continue-on-error" not in step(source, "Analyze code and wait for Quality Gate")


def test_registry_storage_skips_actions_while_focused_profiles_keep_the_default() -> None:
    source = (ROOT / ".github/actions/restore-native-cache/action.yml").read_text(encoding="utf-8")
    assert "compiler-storage:" in source
    assert "default: actions" in source
    assert "if: inputs.compiler-storage == 'actions'" in source
    assert "Unknown compiler storage" in source
    assert "native-tests-v1:Debug" in source
    assert "native-variants-v1:Mixed" in source
    assert "value: ${{ format('native-v1-" in source
    ci = (ROOT / ".github/workflows/ci.yml").read_text(encoding="utf-8")
    for block in ci.split("uses: ./.github/actions/restore-native-cache")[1:]:
        settings = block.split("\n      - name:", 1)[0]
        if "profile: native-tests-v1" in settings:
            assert "compiler-storage: registry" in settings


def test_macos_publishers_retain_complete_native_pipeline_and_bundle_outputs() -> None:
    source = (ROOT / ".github/workflows/ci.yml").read_text(encoding="utf-8")
    reader, main = job(source, "macos-build"), job(source, "macos-build-main")
    assert "    name: Build · ${{ matrix.name }}\n" in reader
    assert "    name: Build · ${{ matrix.name }} (main)\n" in main
    for field, anchor in (("steps", "native_steps"), ("env", "native_environment"), ("defaults", "native_defaults")):
        assert f"    {field}: *{anchor}\n" in reader
        assert f"    {field}: *{anchor}\n" in main
        assert f"    {field}: &{anchor}\n" in source
    for field in ("strategy", "outputs"):
        match = re.search(rf"^    {field}: &(\w+)", reader, re.MULTILINE)
        assert match is not None
        assert f"    {field}: *{match[1]}\n" in main
    assert "packages: read" in reader
    assert "packages: write" not in reader
    assert "packages: write" in main
    assert reader.count("          - name:") == 1
    assert "- name: macOS / Clang" in reader
    assert "max-parallel: 1" in reader
    assert "runs-on: macos-latest" in reader
    for output in ("artifact-id", "artifact-digest", "payload-digest", "producer-attempt"):
        assert f"      {output}:" in reader


@pytest.mark.parametrize("event,head_repo,reader_result,main_result,label,expected", [
    ("workflow_dispatch", "horocore/horo-engine", "success", "skipped", "horo-macos-metal", "horo-macos-metal"),
    ("push", "horocore/horo-engine", "success", "skipped", "horo-macos-metal", "horo-macos-metal"),
    ("pull_request", "fork/horo-engine", "success", "skipped", "horo-macos-metal", "macos-latest"),
    ("pull_request", "horocore/horo-engine", "success", "skipped", "horo-macos-metal", "horo-macos-metal"),
    ("push", "horocore/horo-engine", "skipped", "success", "horo-macos-metal", "horo-macos-metal"),
    ("workflow_dispatch", "horocore/horo-engine", "failure", "skipped", "horo-macos-metal", "ubuntu-slim"),
    ("workflow_dispatch", "horocore/horo-engine", "success", "skipped", "", "macos-latest"),
    ("pull_request", "fork/horo-engine", "failure", "skipped", "horo-macos-metal", "ubuntu-slim"),
])
def test_actual_metal_runner_expression_preserves_admission(
    event: str, head_repo: str, reader_result: str, main_result: str, label: str, expected: str,
) -> None:
    source = (ROOT / ".github/workflows/ci.yml").read_text(encoding="utf-8")
    hardware = job(source, "macos-metal")
    routing = re.search(r"^    runs-on: (.+)$", hardware, re.MULTILINE)
    assert routing is not None
    context = {"github.event_name": event,
               "github.event.pull_request.head.repo.full_name": head_repo,
               "github.repository": "horocore/horo-engine", "vars.HORO_MACOS_METAL_RUNNER": label,
               "needs.macos-build.result": reader_result, "needs.macos-build-main.result": main_result,
               "true": True}
    # Reuse the existing restricted Actions interpreter on the actual field value.
    assert concurrency_key("    group: " + routing[1], context) == expected
