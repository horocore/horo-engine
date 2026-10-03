#!/usr/bin/env python3
"""Check and repair issue, project, and assignee annotations on pull requests.

Requires GH_TOKEN, GITHUB_TOKEN, or an authenticated GitHub CLI (gh).
Checking is the default. --fix can set the issue milestone, establish a
closing issue reference, add labels and projects, and assign the PR.
"""

from __future__ import annotations

import argparse
import asyncio
from concurrent.futures import ThreadPoolExecutor, as_completed
from dataclasses import dataclass, field
import http.client
import json
import os
import re
import sys
from typing import Any
from urllib.parse import urlencode


MAX_RESPONSE_BYTES = 16 * 1024 * 1024
REPOSITORY = re.compile(r"[A-Za-z0-9][A-Za-z0-9_.-]{0,99}/[A-Za-z0-9][A-Za-z0-9_.-]{0,99}")
JIRA_KEY = re.compile(r"(?<![A-Z0-9])([A-Z][A-Z0-9]+-\d+)(?![A-Z0-9])")
CLOSING_REF = re.compile(
    r"\b(?:close[sd]?|fix(?:e[sd])?|resolve[sd]?)\s+" r"(?:horocore/horo-engine)?#(\d+)\b",
    re.IGNORECASE,
)
ISSUE_REF = re.compile(r"(?<!\w)#(\d+)\b")


class AnnotationError(Exception):
    """A PR cannot be checked safely or a GitHub operation failed."""


async def cli_token() -> str:
    """Read CLI credentials with fixed arguments; no PR or user data enters argv."""
    process = await asyncio.create_subprocess_exec(
        "gh",
        "auth",
        "token",
        "--hostname",
        "github.com",
        stdout=asyncio.subprocess.PIPE,
        stderr=asyncio.subprocess.DEVNULL,
    )
    try:
        output, _ = await asyncio.wait_for(process.communicate(), timeout=30)
    except TimeoutError:
        process.kill()
        await process.communicate()
        raise AnnotationError("GitHub CLI credential lookup timed out") from None
    if process.returncode:
        raise AnnotationError("authenticate gh for github.com or set GH_TOKEN")
    return output.decode("utf-8").strip()


def github_token() -> str:
    """Resolve credentials once before workers start; never print token output."""
    token = os.environ.get("GH_TOKEN") or os.environ.get("GITHUB_TOKEN")
    if not token:
        try:
            token = asyncio.run(cli_token())
        except (OSError, UnicodeError) as error:
            raise AnnotationError("cannot read GitHub CLI credentials; set GH_TOKEN") from error
    if (
        not token
        or not token.isascii()
        or any(not 33 <= ord(character) < 127 for character in token)
    ):
        raise AnnotationError("GitHub credentials are empty or malformed")
    return token


def repository(value: str) -> str:
    """Accept only OWNER/REPO, never options, URLs, traversal or query fragments."""
    if REPOSITORY.fullmatch(value) is None:
        raise argparse.ArgumentTypeError("--repo must be an ASCII OWNER/REPO name")
    return value


class GitHub:
    """Fixed-host, bounded JSON transport; each worker owns its connection."""

    def __init__(self, token: str) -> None:
        self.token = token

    def request(self, method: str, path: str, payload: dict[str, Any] | None = None) -> Any:
        connection = http.client.HTTPSConnection("api.github.com", timeout=30)
        try:
            connection.request(
                method,
                path,
                body=json.dumps(payload) if payload is not None else None,
                headers={
                    "Accept": "application/vnd.github+json",
                    "Authorization": f"Bearer {self.token}",
                    "Content-Type": "application/json",
                    "User-Agent": "horo-pr-annotations",
                },
            )
            response = connection.getresponse()
            if not 200 <= response.status < 300:
                raise AnnotationError(f"GitHub request failed with HTTP {response.status}")
            body = response.read(MAX_RESPONSE_BYTES + 1)
            if len(body) > MAX_RESPONSE_BYTES:
                raise AnnotationError("GitHub response exceeds 16 MiB")
            return json.loads(body) if body else None
        except (OSError, http.client.HTTPException, ValueError) as error:
            raise AnnotationError("cannot read GitHub JSON response") from error
        finally:
            connection.close()

    def graphql(self, query: str, variables: dict[str, Any]) -> Any:
        result = self.request("POST", "/graphql", {"query": query, "variables": variables})
        if (
            not isinstance(result, dict)
            or result.get("errors")
            or not isinstance(result.get("data"), dict)
        ):
            raise AnnotationError("GitHub GraphQL request failed")
        return result["data"]

    def pages(self, path: str, params: dict[str, Any] | None = None) -> list[dict[str, Any]]:
        items: list[dict[str, Any]] = []
        for page in range(1, 1001):
            batch = self.request(
                "GET",
                path + "?" + urlencode({**(params or {}), "per_page": 100, "page": page}),
            )
            if not isinstance(batch, list):
                raise AnnotationError("GitHub paginated response must be an array")
            items.extend(batch)
            if len(batch) < 100:
                return items
        raise AnnotationError("GitHub pagination exceeds 1000 pages")


@dataclass
class CheckOptions:
    """Explicit shared check policy; transport creates a connection per request."""

    api: GitHub
    repo: str
    default_branch: str
    issue_override: int | None = None
    project: str | None = None
    project_number: int | None = None
    project_pr_numbers: set[int] = field(default_factory=set)
    include_development: bool = True
    fix: bool = False


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


def resolve_issue(pr: dict[str, Any], options: CheckOptions) -> dict[str, Any]:
    number = options.issue_override or issue_number(pr)
    if number is None:
        key = jira_key(pr)
        if key is None:
            raise AnnotationError("no linked issue or Jira key in branch name")
        matches = options.api.request(
            "GET",
            "/search/issues?"
            + urlencode({"q": f"repo:{options.repo} is:issue {key}", "per_page": 100}),
        )
        if matches.get("incomplete_results") or matches["total_count"] > len(matches["items"]):
            raise AnnotationError("Jira issue search is incomplete; pass --issue NUMBER")
        matches = [
            item
            for item in matches["items"]
            if key in JIRA_KEY.findall(f"{item['title']} {item.get('body') or ''}")
        ]
        if len(matches) != 1:
            raise AnnotationError(
                f"Jira key {key} matched {len(matches)} GitHub issues; " "pass --issue NUMBER"
            )
        number = matches[0]["number"]
    result = options.api.request("GET", f"/repos/{options.repo}/issues/{number}")
    if "pull_request" in result:
        raise AnnotationError("development reference points to a pull request, not an issue")
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
    pr: dict[str, Any], issue_number_value: int, options: CheckOptions
) -> bool:
    if pr.get("baseRefName") == options.default_branch and issue_number_value in {
        int(value) for value in CLOSING_REF.findall(pr.get("body") or "")
    }:
        return True
    cursor = None
    seen_cursors: set[str] = set()
    for _ in range(1000):
        data = options.api.graphql(
            "query($id:ID!,$after:String){ node(id:$id){ ... on PullRequest { "
            "closingIssuesReferences(first:100,after:$after){ nodes{number} pageInfo{hasNextPage endCursor} } } } }",
            {"id": pr["id"], "after": cursor},
        )
        node = data.get("node")
        if not node or not node.get("closingIssuesReferences"):
            raise AnnotationError("GitHub did not return PR closing references")
        result = node["closingIssuesReferences"]
        if issue_number_value in {item["number"] for item in result["nodes"]}:
            return True
        if not result["pageInfo"]["hasNextPage"]:
            return False
        next_cursor = result["pageInfo"]["endCursor"]
        if not next_cursor or next_cursor in seen_cursors:
            raise AnnotationError("GitHub closing-reference pagination did not advance")
        seen_cursors.add(next_cursor)
        cursor = next_cursor
    raise AnnotationError("GitHub closing-reference pagination exceeds 1000 pages")


def milestone_changes(
    pr: dict[str, Any], issue: dict[str, Any], default_branch: str
) -> tuple[list[str], list[tuple[str, str]]]:
    if issue.get("number") is None:
        if pr.get("baseRefName") == default_branch and CLOSING_REF.findall(pr.get("body") or ""):
            return [
                "multiple issues referenced; milestone is ambiguous; closing references are present"
            ], []
        return ["no unambiguous issue; milestone and Development skipped"], []
    expected = issue.get("milestone")
    if expected is None:
        return ["issue has no milestone; milestone skipped"], []
    if (pr.get("milestone") or {}).get("number") != expected["number"]:
        return [], [("milestone", expected["title"])]
    return [], []


def changes(
    pr: dict[str, Any],
    issue: dict[str, Any],
    options: CheckOptions,
) -> tuple[list[str], list[tuple[str, str]]]:
    notes, fixes = milestone_changes(pr, issue, options.default_branch)
    issue_number_value = issue.get("number")
    for label in sorted(names(issue.get("labels"), "name") - names(pr.get("labels"), "name")):
        fixes.append(("label", label))
    current_projects = names(pr.get("projectItems"), "title")
    expected_projects = names(issue.get("projectItems"), "title")
    if options.project:
        expected_projects.add(options.project)
    for project_name in sorted(expected_projects - current_projects):
        fixes.append(("project", project_name))
    issue_assignees = names(issue.get("assignees"), "login")
    expected_assignees = issue_assignees or {pr["author"]["login"]}
    for assignee in sorted(expected_assignees - names(pr.get("assignees"), "login")):
        fixes.append(("assignee", assignee))
    if (
        issue_number_value is not None
        and options.include_development
        and not has_development_link(pr, issue_number_value, options)
    ):
        fixes.append(("development", str(issue_number_value)))
    return notes, fixes


def apply_fix(
    number: int,
    kind: str,
    value: str,
    pr: dict[str, Any],
    issue: dict[str, Any],
    options: CheckOptions,
) -> None:
    if kind == "development":
        options.api.graphql(
            "mutation($issue:ID!,$prs:[ID!]!){ addCloseIssueReferences(input:{issueId:$issue,pullRequestIds:$prs}){issue{id}} }",
            {"issue": issue["id"], "prs": [pr["id"]]},
        )
        return
    if kind == "milestone":
        options.api.request(
            "PATCH",
            f"/repos/{options.repo}/issues/{number}",
            {"milestone": issue["milestone"]["number"]},
        )
    elif kind == "label":
        options.api.request(
            "POST", f"/repos/{options.repo}/issues/{number}/labels", {"labels": [value]}
        )
    elif kind == "assignee":
        options.api.request(
            "POST",
            f"/repos/{options.repo}/issues/{number}/assignees",
            {"assignees": [value]},
        )
    elif kind == "project":
        if options.project_number is None:
            raise AnnotationError("adding an item requires --project NUMBER")
        owner = options.repo.split("/", 1)[0]
        options.api.request(
            "POST",
            f"/orgs/{owner}/projectsV2/{options.project_number}/items",
            {"type": "PullRequest", "id": pr["databaseId"]},
        )
    else:
        raise AnnotationError(f"unknown annotation kind: {kind}")


def load_pr(number: int, options: CheckOptions) -> dict[str, Any]:
    raw_pr = options.api.request("GET", f"/repos/{options.repo}/pulls/{number}")
    return {
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
        "projectItems": (
            [{"title": options.project}]
            if number in options.project_pr_numbers and options.project
            else []
        ),
        "url": raw_pr["html_url"],
    }


def check_pr(number: int, options: CheckOptions) -> bool:
    pr = load_pr(number, options)
    try:
        issue = resolve_issue(pr, options)
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
    notes, fixes = changes(pr, issue, options)
    print(f"PR #{number} -> {issue_label} ({jira_key(pr) or 'no Jira key'})", flush=True)
    for note in notes:
        print(f"  note: {note}", flush=True)
    if options.fix and issue["number"] is None:
        return False
    for kind, value in fixes:
        print(f"  {'fix' if options.fix else 'missing'}: {kind} {value}", flush=True)
        if options.fix:
            try:
                apply_fix(number, kind, value, pr, issue, options)
            except AnnotationError as error:
                print(f"  ERROR: {kind}: {error}", file=sys.stderr, flush=True)
                return False
    if not fixes and issue["number"] is not None:
        print("  OK", flush=True)
    return issue["number"] is not None and (not fixes or options.fix)


def list_pr_numbers(options: CheckOptions, state: str, limit: int | None) -> list[int]:
    api_state = state
    if state == "merged":
        api_state = "closed"
    pulls = options.api.pages(f"/repos/{options.repo}/pulls", {"state": api_state})
    if state in {"merged", "closed"}:
        pulls = [pr for pr in pulls if bool(pr.get("merged_at")) == (state == "merged")]
    numbers = [pr["number"] for pr in pulls]
    return numbers[:limit] if limit is not None else numbers


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("prs", nargs="*", type=int, metavar="PR", help="PR numbers")
    parser.add_argument(
        "--repo", type=repository, default="horocore/horo-engine", help="OWNER/REPO"
    )
    parser.add_argument("--project", help="also require this GitHub project number or title")
    parser.add_argument(
        "--issue",
        type=int,
        help="issue number for one PR if automatic lookup is ambiguous",
    )
    parser.add_argument("--all", action="store_true", help="check every PR, including merged PRs")
    parser.add_argument("--state", choices=("open", "closed", "merged", "all"), default="all")
    parser.add_argument(
        "--limit", type=int, help="optional maximum number of PRs to process with --all"
    )
    parser.add_argument("--workers", type=int, default=8, help="parallel PR checks (default: 8)")
    parser.add_argument("--fix", action="store_true", help="write missing/incorrect annotations")
    parser.add_argument(
        "--skip-development",
        action="store_true",
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
    if any(number < 1 for number in args.prs):
        parser.error("PR numbers must be positive")
    return args


def load_options(args: argparse.Namespace) -> CheckOptions:
    api = GitHub(github_token())
    default_branch = api.request("GET", f"/repos/{args.repo}")["default_branch"]
    options = CheckOptions(
        api,
        args.repo,
        default_branch,
        issue_override=args.issue,
        project=args.project,
        include_development=not args.skip_development,
        fix=args.fix,
    )
    if options.project and options.project.isdecimal():
        options.project_number = int(options.project)
        if options.project_number < 1:
            raise AnnotationError("--project number must be positive")
        owner = options.repo.split("/", 1)[0]
        path = f"/orgs/{owner}/projectsV2/{options.project_number}"
        options.project = api.request("GET", path)["title"]
        options.project_pr_numbers = {
            item["content"]["number"]
            for item in api.pages(path + "/items")
            if (item.get("content") or {}).get("node_id", "").startswith("PR_")
        }
    return options


def process_pr(number: int, options: CheckOptions) -> bool:
    try:
        return check_pr(number, options)
    except AnnotationError as error:
        print(f"PR #{number}: ERROR: {error}", file=sys.stderr)
        return False


def main() -> int:
    args = parse_args()
    try:
        options = load_options(args)
        prs = args.prs
        if args.all:
            prs = list_pr_numbers(options, args.state, args.limit)
        success = True
        with ThreadPoolExecutor(max_workers=args.workers) as executor:
            futures = [executor.submit(process_pr, number, options) for number in prs]
            for future in as_completed(futures):
                success = future.result() and success
        return 0 if success else 1
    except (AnnotationError, json.JSONDecodeError) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
