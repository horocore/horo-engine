"""Offline regression coverage for additive annotation policy and CLI orchestration."""

from __future__ import annotations

import argparse
import sys
from unittest.mock import Mock
from urllib.parse import parse_qs, urlsplit

import pytest

from pr_annotations_test_support import annotations


@pytest.fixture
def options():
    return annotations.CheckOptions(Mock(spec=annotations.GitHub), "horocore/horo-engine", "main")


@pytest.fixture
def raw_pr():
    return {
        "node_id": "PR_node",
        "id": 123,
        "number": 3067,
        "title": "feat: add fracture #2005",
        "body": "Closes #2005",
        "head": {"ref": "feat/HORO-1959_fracture"},
        "base": {"ref": "main"},
        "user": {"login": "author"},
        "assignees": [{"login": "author"}],
        "milestone": None,
        "labels": [],
        "html_url": "https://github.com/horocore/horo-engine/pull/3067",
    }


@pytest.fixture
def raw_issue():
    return {
        "node_id": "ISSUE_node",
        "number": 2005,
        "title": "[DFR-002.3] Fracture",
        "body": "",
        "assignees": [],
        "milestone": {"number": 4, "title": "M3 — Alpha"},
        "labels": [],
    }


def wire_reads(options, raw_pr, raw_issue):
    """Unexpected reads or writes fail instead of reaching a live GitHub account."""

    def request(method, path, payload=None):
        assert method == "GET"
        assert payload is None
        if path.endswith("/pulls/3067"):
            return raw_pr
        assert path.endswith("/issues/2005")
        return raw_issue

    options.api.request.side_effect = request


def references(numbers=(), next_cursor=None):
    return {
        "node": {
            "closingIssuesReferences": {
                "nodes": [{"number": number} for number in numbers],
                "pageInfo": {
                    "hasNextPage": next_cursor is not None,
                    "endCursor": next_cursor,
                },
            }
        }
    }


def test_resolves_issue_from_closing_reference_before_title():
    pr = {
        "number": 3067,
        "title": "feat: implement #2005 [DFR-002.3]",
        "body": "Depends on #3066.\nCloses #2005",
        "closingIssuesReferences": [{"number": 2005}],
    }
    assert annotations.issue_number(pr) == 2005


@pytest.mark.parametrize(
    "pr, expected",
    [
        ({"number": 20, "title": "#20", "body": ""}, None),
        ({"number": 20, "title": "feature", "body": "Closes #2"}, 2),
        (
            {
                "number": 20,
                "title": "feature",
                "closingIssuesReferences": [{"number": 2}],
            },
            2,
        ),
        ({"number": 20, "title": "#2 #3", "body": "Fixes #4"}, 4),
        ({"number": 20, "title": "#2"}, 2),
    ],
)
def test_issue_resolution_precedence(pr, expected):
    assert annotations.issue_number(pr) == expected


def test_refuses_ambiguous_issue_links():
    with pytest.raises(annotations.AnnotationError, match="multiple development issues"):
        annotations.issue_number(
            {"number": 20, "title": "feature", "body": "Closes #10 and fixes #11"}
        )


def test_jira_search_reads_rest_items_without_live_github(options, raw_issue):
    raw_issue["title"] = "HORO-123 repair"
    options.api.request.side_effect = [
        {"total_count": 1, "items": [raw_issue]},
        raw_issue,
    ]
    pr = {
        "number": 20,
        "title": "fix: repair HORO-123",
        "headRefName": "fix/HORO-123_repair",
    }
    assert annotations.resolve_issue(pr, options)["number"] == 2005
    method, path = options.api.request.call_args_list[0].args
    assert method == "GET"
    assert parse_qs(urlsplit(path).query)["q"] == ["repo:horocore/horo-engine is:issue HORO-123"]


@pytest.mark.parametrize(
    "result",
    [
        {"total_count": 0, "items": []},
        {"total_count": 1, "items": [{"title": "HORO-1234", "number": 1}]},
        {"total_count": 2, "items": [{"title": "HORO-123", "number": 1}] * 2},
        {"total_count": 101, "items": []},
        {"total_count": 0, "items": [], "incomplete_results": True},
    ],
)
def test_jira_search_refuses_ambiguous_or_incomplete_results(options, result):
    options.api.request.return_value = result
    with pytest.raises(annotations.AnnotationError):
        annotations.resolve_issue({"number": 20, "title": "HORO-123"}, options)
    assert options.api.request.call_count == 1


def test_explicit_issue_override_and_optional_fields(options, raw_issue):
    options.issue_override = 2005
    raw_issue.update(body=None, labels=None, assignees=None)
    options.api.request.return_value = raw_issue
    issue = annotations.resolve_issue({"number": 20, "title": "#2 #3"}, options)
    assert (issue["body"], issue["labels"], issue["assignees"]) == ("", [], [])
    options.api.request.assert_called_once_with("GET", "/repos/horocore/horo-engine/issues/2005")


def test_missing_issue_or_pull_request_reference_is_not_an_issue(options, raw_issue):
    with pytest.raises(annotations.AnnotationError, match="no linked issue"):
        annotations.resolve_issue({"number": 20, "title": "feature"}, options)
    raw_issue["pull_request"] = {}
    options.api.request.return_value = raw_issue
    with pytest.raises(annotations.AnnotationError, match="not an issue"):
        annotations.resolve_issue({"number": 20, "title": "#2005"}, options)


def test_missing_annotations_are_additive(options):
    pr = {
        "id": "PR_node",
        "number": 20,
        "author": {"login": "me"},
        "assignees": [{"login": "me"}],
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
    options.api.graphql.return_value = references([2])
    assert annotations.changes(pr, issue, options) == (
        [],
        [("milestone", "M3 — Alpha"), ("label", "ready"), ("project", "Roadmap")],
    )
    assert pr["labels"] == [{"name": "pr-only"}]


def test_check_mode_does_not_write(options, raw_pr, raw_issue, capsys):
    wire_reads(options, raw_pr, raw_issue)
    assert not annotations.check_pr(3067, options)
    assert options.api.request.call_count == 2
    options.api.graphql.assert_not_called()
    assert "missing: milestone M3 — Alpha" in capsys.readouterr().out


def test_fix_mode_writes_only_missing_fields(options, raw_pr, raw_issue):
    options.fix = True
    raw_pr["base"]["ref"] = "release/base"
    raw_issue["labels"] = [{"name": "ready"}]
    options.api.request.side_effect = [raw_pr, raw_issue, {}, {}]
    options.api.graphql.side_effect = [references(), {}]
    assert annotations.check_pr(3067, options)
    assert [call.args for call in options.api.request.call_args_list[2:]] == [
        ("PATCH", "/repos/horocore/horo-engine/issues/3067", {"milestone": 4}),
        (
            "POST",
            "/repos/horocore/horo-engine/issues/3067/labels",
            {"labels": ["ready"]},
        ),
    ]
    mutation, variables = options.api.graphql.call_args.args
    assert "addCloseIssueReferences" in mutation
    assert variables == {"issue": "ISSUE_node", "prs": ["PR_node"]}


@pytest.mark.parametrize("fix", [False, True])
def test_unresolved_issue_cannot_report_success(options, raw_pr, fix):
    options.fix = fix
    raw_pr.update(title="feature", body="", head={"ref": "feature"})
    options.api.request.return_value = raw_pr
    assert not annotations.check_pr(3067, options)


def test_complete_annotations_are_success_without_writes(options, raw_pr, raw_issue, capsys):
    raw_pr["milestone"] = raw_issue["milestone"]
    wire_reads(options, raw_pr, raw_issue)
    assert annotations.check_pr(3067, options)
    assert options.api.request.call_count == 2
    assert "OK" in capsys.readouterr().out


def test_failed_write_is_reported_and_not_success(options, raw_pr, raw_issue, capsys):
    options.fix = True
    options.api.request.side_effect = [
        raw_pr,
        raw_issue,
        annotations.AnnotationError("denied"),
    ]
    assert not annotations.check_pr(3067, options)
    assert "ERROR: milestone: denied" in capsys.readouterr().err


@pytest.mark.parametrize(
    "issue, body, note",
    [
        ({"number": None}, "Closes #1 fixes #2", "milestone is ambiguous"),
        ({"number": None}, "", "no unambiguous issue"),
        ({"number": 2, "milestone": None}, "", "issue has no milestone"),
    ],
)
def test_milestone_skip_never_clears_pr(issue, body, note):
    notes, fixes = annotations.milestone_changes(
        {"baseRefName": "main", "body": body}, issue, "main"
    )
    assert note in notes[0]
    assert fixes == []


def test_expected_assignees_projects_and_skip_development(options):
    options.project = "Roadmap"
    options.include_development = False
    pr = {
        "number": 20,
        "author": {"login": "author"},
        "assignees": [{"login": "pr-only"}],
        "projectItems": [{"title": "Existing"}],
    }
    issue = {"number": 2, "assignees": [{"login": "owner"}]}
    notes, fixes = annotations.changes(pr, issue, options)
    assert notes == ["issue has no milestone; milestone skipped"]
    assert fixes == [("project", "Roadmap"), ("assignee", "owner")]
    options.api.graphql.assert_not_called()


def test_development_link_uses_pagination_and_variables(options):
    options.api.graphql.side_effect = [references([1], "cursor"), references([2])]
    pr = {"id": 'PR_"injection', "baseRefName": "stacked", "body": "Closes #2"}
    assert annotations.has_development_link(pr, 2, options)
    first, second = options.api.graphql.call_args_list
    assert first.args[1] == {"id": pr["id"], "after": None}
    assert second.args[1] == {"id": pr["id"], "after": "cursor"}
    assert pr["id"] not in first.args[0]


def test_absent_development_link(options):
    options.api.graphql.return_value = references()
    assert not annotations.has_development_link({"id": "PR_node"}, 2, options)


@pytest.mark.parametrize("data", [{"node": None}, {"node": {}}])
def test_unavailable_closing_references_fail_closed(options, data):
    options.api.graphql.return_value = data
    with pytest.raises(annotations.AnnotationError, match="did not return PR"):
        annotations.has_development_link({"id": "PR_node"}, 2, options)


def test_unresolved_issue_fix_mode_does_not_make_partial_writes(options, raw_pr):
    options.fix = True
    options.project = "Roadmap"
    options.project_number = 1
    raw_pr.update(title="feature", body="", head={"ref": "feature"}, assignees=[])
    options.api.request.return_value = raw_pr
    assert not annotations.check_pr(3067, options)
    options.api.request.assert_called_once_with("GET", "/repos/horocore/horo-engine/pulls/3067")


@pytest.mark.parametrize("cursor", ["", "repeated"])
def test_nonadvancing_development_pagination_fails(options, cursor):
    options.api.graphql.return_value = references([], cursor)
    with pytest.raises(annotations.AnnotationError, match="did not advance"):
        annotations.has_development_link({"id": "PR_node"}, 2, options)


def test_excessive_development_pagination_fails(options):
    options.api.graphql.side_effect = [references([], str(index)) for index in range(1000)]
    with pytest.raises(annotations.AnnotationError, match="1000 pages"):
        annotations.has_development_link({"id": "PR_node"}, 2, options)


@pytest.mark.parametrize(
    "kind, payload, endpoint",
    [
        ("label", {"labels": ["--method=DELETE"]}, "labels"),
        ("assignee", {"assignees": ["--method=DELETE"]}, "assignees"),
        ("project", {"type": "PullRequest", "id": 123}, None),
    ],
)
def test_fix_values_are_json_not_options(options, kind, payload, endpoint):
    options.project_number = 1
    annotations.apply_fix(20, kind, "--method=DELETE", {"databaseId": 123}, {}, options)
    path = (
        "/orgs/horocore/projectsV2/1/items"
        if endpoint is None
        else f"/repos/horocore/horo-engine/issues/20/{endpoint}"
    )
    options.api.request.assert_called_once_with("POST", path, payload)


@pytest.mark.parametrize(
    "kind, message",
    [("project", "requires --project NUMBER"), ("other", "unknown annotation")],
)
def test_unsupported_fixes_fail_without_writes(options, kind, message):
    with pytest.raises(annotations.AnnotationError, match=message):
        annotations.apply_fix(20, kind, "value", {}, {}, options)
    options.api.request.assert_not_called()


def test_load_pr_maps_project_membership_and_empty_fields(options, raw_pr):
    options.project = "Roadmap"
    options.project_pr_numbers = {3067}
    raw_pr.update(body=None, assignees=None, labels=None)
    options.api.request.return_value = raw_pr
    pr = annotations.load_pr(3067, options)
    assert pr["projectItems"] == [{"title": "Roadmap"}]
    assert (pr["body"], pr["assignees"], pr["labels"]) == ("", [], [])


@pytest.mark.parametrize(
    "args",
    [
        [],
        ["--all", "20"],
        ["20", "21", "--issue", "2"],
        ["20", "--limit", "0"],
        ["20", "--workers", "0"],
        ["20", "--workers", "17"],
        ["20", "--issue", "0"],
        ["0"],
        ["-1"],
        ["20", "--repo", "o/r?x=1"],
    ],
)
def test_invalid_cli_input_fails_before_credentials(monkeypatch, args):
    monkeypatch.setattr(sys, "argv", ["pr_annotations.py", *args])
    with pytest.raises(SystemExit) as failure:
        annotations.parse_args()
    assert failure.value.code == 2


def test_valid_cli_input_keeps_read_only_defaults(monkeypatch):
    monkeypatch.setattr(sys, "argv", ["pr_annotations.py", "20", "--repo", "Org/repo"])
    args = annotations.parse_args()
    assert args.prs == [20]
    assert args.repo == "Org/repo"
    assert args.workers == 8
    assert not args.fix


@pytest.mark.parametrize(
    "state, expected",
    [("open", [1, 2]), ("all", [1, 2]), ("merged", [2]), ("closed", [1])],
)
def test_pr_listing_preserves_state_filter(options, state, expected):
    options.api.pages.return_value = [
        {"number": 1, "merged_at": None},
        {"number": 2, "merged_at": "date"},
    ]
    assert annotations.list_pr_numbers(options, state, None) == expected
    assert annotations.list_pr_numbers(options, state, 1) == expected[:1]
    api_state = "closed" if state == "merged" else state
    options.api.pages.assert_called_with("/repos/horocore/horo-engine/pulls", {"state": api_state})


def arguments(project=None):
    return argparse.Namespace(
        repo="horocore/horo-engine",
        issue=None,
        project=project,
        skip_development=False,
        fix=False,
    )


@pytest.mark.parametrize("project", [None, "Roadmap", "1", "0"])
def test_option_loading_resolves_project_number_once(monkeypatch, project):
    api = Mock(spec=annotations.GitHub)
    api.request.side_effect = [{"default_branch": "main"}, {"title": "Roadmap"}]
    api.pages.return_value = [
        {"content": {"node_id": "PR_node", "number": 20}},
        {"content": {"node_id": "ISSUE_node", "number": 2}},
        {"content": None},
    ]
    monkeypatch.setattr(annotations, "github_token", lambda: "fixture-token")
    monkeypatch.setattr(annotations, "GitHub", Mock(return_value=api))
    if project == "0":
        with pytest.raises(annotations.AnnotationError, match="must be positive"):
            annotations.load_options(arguments(project))
    else:
        loaded = annotations.load_options(arguments(project))
        assert loaded.default_branch == "main"
        assert loaded.project == ("Roadmap" if project == "1" else project)
        assert loaded.project_pr_numbers == ({20} if project == "1" else set())


def test_process_pr_reports_transport_error(options, capsys):
    options.api.request.side_effect = annotations.AnnotationError("offline")
    assert not annotations.process_pr(20, options)
    assert "PR #20: ERROR: offline" in capsys.readouterr().err


@pytest.mark.parametrize(
    "all_prs, results, expected",
    [(False, [True, True], 0), (True, [True, False], 1), (True, [], 0)],
)
def test_main_aggregates_all_worker_results(monkeypatch, options, all_prs, results, expected):
    args = argparse.Namespace(
        prs=list(range(len(results))), all=all_prs, state="open", limit=None, workers=2
    )
    monkeypatch.setattr(annotations, "parse_args", lambda: args)
    monkeypatch.setattr(annotations, "load_options", lambda _: options)
    monkeypatch.setattr(annotations, "list_pr_numbers", lambda *_: args.prs)
    process = Mock(side_effect=lambda number, _: results[number])
    monkeypatch.setattr(annotations, "process_pr", process)
    assert annotations.main() == expected
    assert process.call_count == len(results)


def test_main_fails_on_setup_error(monkeypatch, capsys):
    monkeypatch.setattr(annotations, "parse_args", lambda: arguments())
    monkeypatch.setattr(
        annotations,
        "load_options",
        Mock(side_effect=annotations.AnnotationError("setup failed")),
    )
    assert annotations.main() == 1
    assert "ERROR: setup failed" in capsys.readouterr().err
