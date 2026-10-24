"""Protect write-token isolation and unchanged qualification work across main/PR jobs."""

from __future__ import annotations

import ast
import operator
from pathlib import Path
import re

import pytest


ROOT = Path(__file__).resolve().parents[2]


def job(workflow: str, name: str) -> str:
    match = re.search(rf"^  {re.escape(name)}:\n(.*?)(?=^  [\w-]+:|\Z)",
                      workflow, re.MULTILINE | re.DOTALL)
    assert match is not None
    return match[1]


def condition(block: str, context: dict[str, str]) -> bool:
    """Interpret only the equality/boolean subset used by these token guards."""
    match = re.search(r"^    if: \$\{\{ (.*?) \}\}$", block, re.MULTILINE)
    assert match is not None
    expression = match[1]
    for name, value in context.items():
        expression = expression.replace(name, repr(value))
    expression = expression.replace("&&", " and ").replace("||", " or ")
    expression = re.sub(r"!(?!=)", "not ", expression).strip()
    return bool(boolean_value(ast.parse(expression, mode="eval").body))


def boolean_value(node: ast.AST) -> str | bool:
    """Reject calls and unsupported syntax instead of evaluating workflow code."""
    if isinstance(node, ast.Constant):
        return node.value
    if isinstance(node, ast.BoolOp):
        values = (boolean_value(value) for value in node.values)
        return all(values) if isinstance(node.op, ast.And) else any(values)
    if isinstance(node, ast.UnaryOp) and isinstance(node.op, ast.Not):
        return not boolean_value(node.operand)
    if isinstance(node, ast.Compare) and len(node.ops) == 1:
        return comparison_value(node)
    raise ValueError("Unexpected token-guard expression")


def comparison_value(node: ast.Compare) -> bool:
    """Restrict comparisons to the two string equality operators used by Actions."""
    operation = {ast.Eq: operator.eq, ast.NotEq: operator.ne}.get(type(node.ops[0]))
    if operation is None:
        raise ValueError("Unexpected token-guard comparison")
    return operation(boolean_value(node.left), boolean_value(node.comparators[0]))


@pytest.mark.parametrize("workflow,readonly,writer", [
    ("ci", "test", "test-main"), ("sonar", "sonar", "sonar-main"),
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
    expected = (repository == "horocore/horo-engine" and ref == "refs/heads/main"
                and event in {"push", "workflow_dispatch"})
    assert condition(job(source, writer), context) is expected
    assert condition(job(source, readonly), context) is not expected


def test_foreign_fork_sonar_policy_is_preserved() -> None:
    source = (ROOT / ".github/workflows/sonar.yml").read_text(encoding="utf-8")
    context = {"github.repository": "horocore/horo-engine", "github.ref": "refs/pull/17/merge",
               "github.event_name": "pull_request",
               "github.event.pull_request.head.repo.full_name": "fork/horo-engine"}
    assert condition(job(source, "sonar"), context) is False
    assert condition(job(source, "sonar-main"), context) is False


@pytest.mark.parametrize("workflow,readonly,writer,fields", [
    ("ci", "test", "test-main", ("steps", "env", "strategy", "defaults")),
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
    assert "    timeout-minutes: 180\n" in reader
    assert "    timeout-minutes: 180\n" in main
    for field in fields:
        match = re.search(rf"^    {field}: &(\w+)", reader, re.MULTILINE)
        assert match is not None
        assert f"    {field}: *{match[1]}\n" in main
    if workflow == "ci":
        assert "max-parallel: 2" in reader
        assert reader.count("          - name:") == 3
    assert "cancel-in-progress: ${{ github.event_name == 'pull_request' }}" in source
    group = re.search(r"^  group: (.*)$", source, re.MULTILINE)
    assert group is not None
    assert "${{ github.workflow }}" in group[1]
    assert "${{ github.ref }}" in group[1]
    assert "run_id" not in group[1]


def step(source: str, name: str) -> str:
    return source.split(f"      - name: {name}\n", 1)[1].split("\n      - name:", 1)[0]


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
