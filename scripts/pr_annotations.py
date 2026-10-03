#!/usr/bin/env python3
"""Check and repair issue, project, and assignee annotations on pull requests.

Requires an authenticated GitHub CLI (gh). Checking is the default. --fix can
set the issue milestone, establish a closing issue reference, add labels and
projects, and assign the PR.
"""

from __future__ import annotations

import argparse
from concurrent.futures import ThreadPoolExecutor, as_completed
import json
import re
import subprocess
import sys
from typing import Any


PR_FIELDS = (
    "id,number,title,body,headRefName,author,assignees,closingIssuesReferences,"
    "milestone,labels,projectItems,url"
)
ISSUE_FIELDS = "id,number,title,body,assignees,milestone,labels,projectItems,url"
JIRA_KEY = re.compile(r"(?<![A-Z0-9])([A-Z][A-Z0-9]+-\d+)(?![A-Z0-9])")
CLOSING_REF = re.compile(
    r"\b(?:close[sd]?|fix(?:e[sd])?|resolve[sd]?)\s+"
    r"(?:horocore/horo-engine)?#(\d+)\b",
    re.IGNORECASE,
)
ISSUE_REF = re.compile(r"(?<!\w)#(\d+)\b")


class AnnotationError(Exception):
    """A PR cannot be checked safely or a GitHub operation failed."""


def gh(*args: str) -> Any:
    command = ["gh", *args]
    try:
        result = subprocess.run(command, capture_output=True, text=True, check=False)
    except OSError as error:
        raise AnnotationError(f"cannot run gh: {error}") from error
    if result.returncode:
        raise AnnotationError(
            f"{' '.join(command)}: {result.stderr.strip() or result.stdout.strip()}"
        )
    if "--json" in args or ("--format" in args and "json" in args) or (
        args[0] == "api" and "--jq" not in args and "-q" not in args
    ):
        return json.loads(result.stdout) if result.stdout.strip() else None
    return result.stdout.strip()


def jira_key(pr: dict[str, Any]) -> str | None:
    text = " ".join((pr.get("headRefName") or "", pr.get("title") or "", pr.get("body") or ""))
    match = JIRA_KEY.search(text)
    return match.group(1) if match else None


def issue_number(pr: dict[str, Any]) -> int | None:
    title_links = {int(number) for number in ISSUE_REF.findall(pr.get("title") or "")}
    title_links.discard(pr["number"])
    if len(title_links) == 1:
        return next(iter(title_links))
    linked = {item["number"] for item in pr.get("closingIssuesReferences") or []}
    linked.update(int(number) for number in CLOSING_REF.findall(pr.get("body") or ""))
    if not linked:
        linked = title_links
    linked.discard(pr["number"])
    if len(linked) > 1:
        raise AnnotationError(f"multiple development issues: {sorted(linked)}")
    return next(iter(linked)) if linked else None


def resolve_issue(pr: dict[str, Any], repo: str, override: int | None) -> dict[str, Any]:
    number = override or issue_number(pr)
    if number is None:
        key = jira_key(pr)
        if key is None:
            raise AnnotationError("no linked issue or Jira key in branch name")
        matches = gh(
            "api", "-X", "GET", "search/issues", "-f", f"q=repo:{repo} is:issue {key}",
            "-F", "per_page=100",
        )
        matches = [
            item for item in matches["items"]
            if key in JIRA_KEY.findall(f"{item['title']} {item.get('body') or ''}")
        ]
        if len(matches) != 1:
            raise AnnotationError(
                f"Jira key {key} matched {len(matches)} GitHub issues; "
                "pass --issue NUMBER"
            )
        number = matches[0]["number"]
    result = gh("api", f"repos/{repo}/issues/{number}")
    return {
        "id": result["node_id"],
        "number": result["number"],
        "title": result["title"],
        "body": result.get("body") or "",
        "milestone": result.get("milestone"),
        "labels": result.get("labels") or [],
        "assignees": result.get("assignees") or [],
        "projectItems": [],
    }


def names(items: list[dict[str, Any]] | None, field: str) -> set[str]:
    return {item[field] for item in items or []}


def has_development_link(
    pr: dict[str, Any], issue_number_value: int, repo: str, default_branch: str
) -> bool:
    if pr.get("baseRefName") == default_branch and issue_number_value in {
        int(value) for value in CLOSING_REF.findall(pr.get("body") or "")
    }:
        return True
    result = gh(
        "pr", "view", str(pr["number"]), "--repo", repo,
        "--json", "closingIssuesReferences",
    )
    return issue_number_value in {
        item["number"] for item in result.get("closingIssuesReferences") or []
    }


def changes(
    pr: dict[str, Any], issue: dict[str, Any], project: str | None,
    repo: str, default_branch: str, include_development: bool,
) -> tuple[list[str], list[tuple[str, str]]]:
    notes: list[str] = []
    fixes: list[tuple[str, str]] = []
    expected = issue.get("milestone")
    actual = pr.get("milestone")
    issue_number_value = issue.get("number")
    if issue_number_value is None:
        if pr.get("baseRefName") == default_branch and CLOSING_REF.findall(pr.get("body") or ""):
            notes.append("multiple issues referenced; milestone is ambiguous; closing references are present")
        else:
            notes.append("no unambiguous issue; milestone and Development skipped")
    else:
        if expected is None:
            notes.append("issue has no milestone; milestone skipped")
        elif (actual or {}).get("number") != expected["number"]:
            fixes.append(("milestone", expected["title"]))
    for label in sorted(names(issue.get("labels"), "name") - names(pr.get("labels"), "name")):
        fixes.append(("label", label))
    current_projects = names(pr.get("projectItems"), "title")
    expected_projects = names(issue.get("projectItems"), "title")
    if project:
        expected_projects.add(project)
    for project_name in sorted(expected_projects - current_projects):
        fixes.append(("project", project_name))
    issue_assignees = names(issue.get("assignees"), "login")
    expected_assignees = issue_assignees or {pr["author"]["login"]}
    for assignee in sorted(expected_assignees - names(pr.get("assignees"), "login")):
        fixes.append(("assignee", assignee))
    if issue_number_value is not None and include_development and not has_development_link(
        pr, issue_number_value, repo, default_branch
    ):
        fixes.append(("development", str(issue_number_value)))
    return notes, fixes


def apply_fix(
    number: int,
    repo: str,
    kind: str,
    value: str,
    pr: dict[str, Any],
    issue: dict[str, Any],
    project_number: int | None,
) -> None:
    if kind == "development":
        query = (
            "mutation { addCloseIssueReferences(input: {issueId: \""
            f"{issue['id']}\", pullRequestIds: [\"{pr['id']}\"]"
            "}) { issue { id } } }"
        )
        gh("api", "graphql", "-f", f"query={query}")
        return
    if kind == "milestone":
        gh("api", "-X", "PATCH", f"repos/{repo}/issues/{number}", "-F", f"milestone={issue['milestone']['number']}")
    elif kind == "label":
        gh("api", "-X", "POST", f"repos/{repo}/issues/{number}/labels", "-f", f"labels[]={value}")
    elif kind == "assignee":
        gh("api", "-X", "POST", f"repos/{repo}/issues/{number}/assignees", "-f", f"assignees[]={value}")
    elif kind == "project":
        if project_number is None:
            raise AnnotationError("adding an item requires --project NUMBER")
        owner = repo.split("/", 1)[0]
        gh(
            "api", "-X", "POST", f"orgs/{owner}/projectsV2/{project_number}/items",
            "-f", "type=PullRequest", "-F", f"id={pr['databaseId']}",
        )


def check_pr(
    number: int, repo: str, issue_override: int | None, project: str | None,
    project_number: int | None, project_pr_numbers: set[int], default_branch: str,
    include_development: bool, fix: bool,
) -> bool:
    raw_pr = gh("api", f"repos/{repo}/pulls/{number}")
    pr = {
        "id": raw_pr["node_id"],
        "databaseId": raw_pr["id"],
        "number": raw_pr["number"],
        "title": raw_pr["title"],
        "body": raw_pr.get("body") or "",
        "headRefName": raw_pr["head"]["ref"],
        "baseRefName": raw_pr["base"]["ref"],
        "author": {"login": raw_pr["user"]["login"]},
        "assignees": raw_pr.get("assignees") or [],
        "closingIssuesReferences": [],
        "milestone": raw_pr.get("milestone"),
        "labels": raw_pr.get("labels") or [],
        "projectItems": ([{"title": project}] if number in project_pr_numbers and project else []),
        "url": raw_pr["html_url"],
    }
    try:
        issue = resolve_issue(pr, repo, issue_override)
        issue_label = f"issue #{issue['number']}"
    except AnnotationError as error:
        issue = {
            "id": "",
            "number": None,
            "title": "",
            "body": "",
            "milestone": None,
            "labels": [],
            "assignees": [],
            "projectItems": [],
        }
        issue_label = "no unambiguous issue"
        print(f"PR #{number}: note: {error}", flush=True)
    notes, fixes = changes(pr, issue, project, repo, default_branch, include_development)
    print(f"PR #{number} -> {issue_label} ({jira_key(pr) or 'no Jira key'})", flush=True)
    for note in notes:
        print(f"  note: {note}", flush=True)
    for kind, value in fixes:
        print(f"  {'fix' if fix else 'missing'}: {kind} {value}", flush=True)
        if fix:
            try:
                apply_fix(number, repo, kind, value, pr, issue, project_number)
            except AnnotationError as error:
                print(f"  ERROR: {kind}: {error}", file=sys.stderr, flush=True)
                return False
    if not fixes:
        print("  OK", flush=True)
    return not fixes or fix


def list_pr_numbers(repo: str, state: str, limit: int | None) -> list[int]:
    api_state = "open" if state == "open" else "closed" if state in {"closed", "merged"} else "all"
    jq = ".[].number"
    if state == "merged":
        jq = '.[] | select(.merged_at != null) | .number'
    elif state == "closed":
        jq = '.[] | select(.merged_at == null) | .number'
    output = gh(
        "api", "--paginate", "-X", "GET", f"repos/{repo}/pulls", "-F", f"state={api_state}",
        "-F", "per_page=100", "--jq", jq,
    )
    numbers = [int(line) for line in output.splitlines() if line.strip()]
    return numbers[:limit] if limit is not None else numbers


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("prs", nargs="*", type=int, metavar="PR", help="PR numbers")
    parser.add_argument("--repo", default="horocore/horo-engine", help="OWNER/REPO")
    parser.add_argument("--project", help="also require this GitHub project number or title")
    parser.add_argument("--issue", type=int, help="issue number for one PR if automatic lookup is ambiguous")
    parser.add_argument("--all", action="store_true", help="check every PR, including merged PRs")
    parser.add_argument("--state", choices=("open", "closed", "merged", "all"), default="all")
    parser.add_argument("--limit", type=int, help="optional maximum number of PRs to process with --all")
    parser.add_argument("--workers", type=int, default=8, help="parallel PR checks (default: 8)")
    parser.add_argument("--fix", action="store_true", help="write missing/incorrect annotations")
    parser.add_argument(
        "--skip-development", action="store_true",
        help="skip Development links when GitHub GraphQL is unavailable",
    )
    args = parser.parse_args()
    if args.all == bool(args.prs) or (args.issue and len(args.prs) != 1):
        parser.error("provide PR numbers or --all; --issue requires exactly one PR")
    if args.limit is not None and args.limit < 1:
        parser.error("--limit must be positive")
    if args.workers < 1 or args.workers > 16:
        parser.error("--workers must be between 1 and 16")
    if args.issue is not None and args.issue < 1:
        parser.error("--issue must be positive")
    try:
        prs = args.prs
        project = args.project
        project_number: int | None = None
        project_pr_numbers: set[int] = set()
        if project and project.isdecimal():
            owner = args.repo.split("/", 1)[0]
            project_number = int(project)
            project_info = gh("api", f"orgs/{owner}/projectsV2/{project_number}")
            project = project_info["title"]
            item_nodes = gh(
                "api", "--paginate", f"orgs/{owner}/projectsV2/{project_number}/items",
                "--jq", '.[] | select((.content.node_id? // "") | startswith("PR_")) | .content.number',
            )
            project_pr_numbers = {int(line) for line in item_nodes.splitlines() if line.strip()}
        default_branch = gh("api", f"repos/{args.repo}", "--jq", ".default_branch")
        if args.all:
            prs = list_pr_numbers(args.repo, args.state, args.limit)
        def process(number: int) -> bool:
            try:
                return check_pr(
                    number, args.repo, args.issue, project, project_number,
                    project_pr_numbers, default_branch, not args.skip_development, args.fix,
                )
            except AnnotationError as error:
                print(f"PR #{number}: ERROR: {error}", file=sys.stderr)
                return False

        success = True
        with ThreadPoolExecutor(max_workers=args.workers) as executor:
            futures = [executor.submit(process, number) for number in prs]
            for future in as_completed(futures):
                success = future.result() and success
        return 0 if success else 1
    except (AnnotationError, json.JSONDecodeError) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
