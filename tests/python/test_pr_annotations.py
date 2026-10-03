"""Regression coverage for pull request annotation checks."""

from __future__ import annotations

import importlib.util
from pathlib import Path

import pytest


SCRIPT = Path(__file__).resolve().parents[2] / "scripts" / "pr_annotations.py"
SPEC = importlib.util.spec_from_file_location("pr_annotations", SCRIPT)
assert SPEC and SPEC.loader
annotations = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(annotations)


def test_resolves_issue_from_closing_reference_before_title() -> None:
    pr = {
        "number": 3067,
        "title": "feat: implement #2005 [DFR-002.3]",
        "body": "Depends on #3066.\nCloses #2005",
        "closingIssuesReferences": [{"number": 2005}],
    }
    assert annotations.issue_number(pr) == 2005


def test_refuses_ambiguous_issue_links() -> None:
    pr = {
        "id": "PR_node",
        "number": 20,
        "title": "feature",
        "body": "Closes #10 and fixes #11",
        "closingIssuesReferences": [],
    }
    with pytest.raises(annotations.AnnotationError, match="multiple development issues"):
        annotations.issue_number(pr)


def test_jira_search_reads_rest_items_without_live_github(monkeypatch: pytest.MonkeyPatch) -> None:
    pr = {"number": 20, "title": "fix: repair HORO-123", "headRefName": "fix/HORO-123_repair"}
    issue = {"node_id": "ISSUE_node", "number": 2, "title": "HORO-123 repair", "body": ""}

    def fake_gh(*args: str) -> dict:
        if args[:4] == ("api", "-X", "GET", "search/issues"):
            return {"total_count": 1, "items": [issue]}
        assert args == ("api", "repos/horocore/horo-engine/issues/2")
        return issue

    monkeypatch.setattr(annotations, "gh", fake_gh)
    assert annotations.resolve_issue(pr, "horocore/horo-engine", None)["number"] == 2


def test_missing_annotations_are_additive(monkeypatch: pytest.MonkeyPatch) -> None:
    pr = {
        "number": 20,
        "author": {"login": "me"},
        "assignees": [{"login": "me"}],
        "closingIssuesReferences": [{"number": 2}],
        "milestone": {"number": 1, "title": "Old"},
        "labels": [{"name": "pr-only"}],
        "projectItems": [],
    }
    issue = {
        "id": "ISSUE_node",
        "number": 2,
        "assignees": [],
        "milestone": {"number": 4, "title": "M3 — Alpha"},
        "labels": [{"name": "ready"}],
        "projectItems": [{"title": "Roadmap"}],
    }
    def fake_gh(*args: str) -> dict:
        assert args == ("pr", "view", "20", "--repo", "horocore/horo-engine", "--json", "closingIssuesReferences")
        return {"closingIssuesReferences": [{"number": 2}]}

    monkeypatch.setattr(annotations, "gh", fake_gh)
    assert annotations.changes(
        pr, issue, None, "horocore/horo-engine", "main", True
    ) == (
        [],
        [("milestone", "M3 — Alpha"), ("label", "ready"), ("project", "Roadmap")],
    )


def test_check_mode_does_not_write(monkeypatch: pytest.MonkeyPatch, capsys: pytest.CaptureFixture[str]) -> None:
    pr = {
        "node_id": "PR_node",
        "id": 123,
        "number": 3067,
        "title": "feat: add fracture #2005",
        "body": "Closes #2005",
        "head": {"ref": "feat/HORO-1959_fracture"},
        "base": {"ref": "main"},
        "user": {"login": "abdullahbodur"},
        "assignees": [{"login": "abdullahbodur"}],
        "milestone": None,
        "labels": [],
        "html_url": "https://github.com/horocore/horo-engine/pull/3067",
    }
    issue = {
        "node_id": "ISSUE_node",
        "number": 2005,
        "title": "[DFR-002.3] Fracture",
        "body": "",
        "assignees": [],
        "milestone": {"number": 4, "title": "M3 — Alpha"},
        "labels": [],
    }
    calls: list[tuple[str, ...]] = []

    def fake_gh(*args: str) -> dict:
        calls.append(args)
        if args == ("api", "repos/horocore/horo-engine/pulls/3067"):
            return pr
        if args == ("api", "repos/horocore/horo-engine/issues/2005"):
            return issue
        raise AssertionError(f"unexpected write: {args}")

    monkeypatch.setattr(annotations, "gh", fake_gh)
    assert not annotations.check_pr(
        3067, "horocore/horo-engine", None, None, None, set(), "main", True, False
    )
    assert len(calls) == 2
    assert "missing: milestone M3 — Alpha" in capsys.readouterr().out


def test_fix_mode_writes_only_missing_fields(monkeypatch: pytest.MonkeyPatch) -> None:
    pr = {
        "node_id": "PR_node",
        "id": 123,
        "number": 3067,
        "title": "feat: add fracture #2005",
        "body": "Closes #2005",
        "head": {"ref": "feat/HORO-1959_fracture"},
        "base": {"ref": "release/base"},
        "user": {"login": "abdullahbodur"},
        "assignees": [{"login": "abdullahbodur"}],
        "milestone": None,
        "labels": [],
        "html_url": "https://github.com/horocore/horo-engine/pull/3067",
    }
    issue = {
        "node_id": "ISSUE_node",
        "number": 2005,
        "title": "[DFR-002.3] Fracture",
        "body": "",
        "assignees": [],
        "milestone": {"number": 4, "title": "M3 — Alpha"},
        "labels": [{"name": "ready"}],
    }
    writes: list[tuple[str, ...]] = []

    def fake_gh(*args: str) -> dict:
        if args == ("api", "repos/horocore/horo-engine/pulls/3067"):
            return pr
        if args == ("api", "repos/horocore/horo-engine/issues/2005"):
            return issue
        if args[:2] == ("pr", "view"):
            return {"closingIssuesReferences": []}
        writes.append(args)
        return {}

    monkeypatch.setattr(annotations, "gh", fake_gh)
    assert annotations.check_pr(
        3067, "horocore/horo-engine", None, None, None, set(), "main", True, True
    )
    assert writes == [
        ("api", "-X", "PATCH", "repos/horocore/horo-engine/issues/3067", "-F", "milestone=4"),
        ("api", "-X", "POST", "repos/horocore/horo-engine/issues/3067/labels", "-f", "labels[]=ready"),
        (
            "api", "graphql", "-f",
            'query=mutation { addCloseIssueReferences(input: {issueId: "ISSUE_node", '
            'pullRequestIds: ["PR_node"]}) { issue { id } } }',
        ),
    ]
