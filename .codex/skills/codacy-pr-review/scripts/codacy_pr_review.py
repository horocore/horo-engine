#!/usr/bin/env python3
"""Resolve changed PR files to Codacy IDs and report duplication details."""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
import urllib.error
import urllib.parse
import urllib.request
from typing import Any


CODACY_API = "https://app.codacy.com/api/v3"


class ReviewError(RuntimeError):
    """Raised when GitHub or Codacy data cannot be read."""


class CodacyNotFound(ReviewError):
    """Raised when Codacy has no analysis resource for a branch or file."""


def gh_json(endpoint: str) -> Any:
    """Read JSON through the GitHub CLI, including all paginated pages."""

    process = subprocess.run(
        ["gh", "api", endpoint, "--paginate", "--slurp"],
        check=False,
        capture_output=True,
        text=True,
    )
    if process.returncode != 0:
        raise ReviewError(process.stderr.strip() or f"gh api failed for {endpoint}")
    try:
        pages = json.loads(process.stdout)
    except json.JSONDecodeError as error:
        raise ReviewError(f"gh api returned invalid JSON for {endpoint}: {error}") from error
    if isinstance(pages, list) and pages and all(isinstance(page, list) for page in pages):
        return [item for page in pages for item in page]
    return pages[0] if isinstance(pages, list) and len(pages) == 1 else pages


def codacy_json(path: str, token: str | None) -> dict[str, Any]:
    """Read one Codacy v3 JSON resource."""

    request = urllib.request.Request(f"{CODACY_API}{path}", headers={"Accept": "application/json"})
    if token:
        request.add_header("Authorization", f"Bearer {token}")
    try:
        with urllib.request.urlopen(request, timeout=30) as response:
            payload = json.load(response)
    except urllib.error.HTTPError as error:
        if error.code == 404:
            raise CodacyNotFound(f"Codacy resource was not found for {path}") from error
        raise ReviewError(f"Codacy API request failed for {path}: {error}") from error
    except (urllib.error.URLError, TimeoutError) as error:
        raise ReviewError(f"Codacy API request failed for {path}: {error}") from error
    if not isinstance(payload, dict):
        raise ReviewError(f"Codacy API returned a non-object response for {path}")
    if payload.get("error") or payload.get("message") and "data" not in payload:
        raise ReviewError(f"Codacy API rejected {path}: {payload}")
    return payload


def changed_paths(owner: str, repo: str, pr: int) -> list[str]:
    """Return unique repository-relative paths changed by a PR."""

    files = gh_json(f"repos/{owner}/{repo}/pulls/{pr}/files?per_page=100")
    if not isinstance(files, list):
        raise ReviewError("GitHub PR files response was not a list")
    return sorted({item["filename"] for item in files if isinstance(item, dict) and item.get("filename")})


def pull_request_branch(owner: str, repo: str, pr: int) -> str:
    """Return the head branch used for Codacy's branch-specific file list."""

    pull_request = gh_json(f"repos/{owner}/{repo}/pulls/{pr}")
    if not isinstance(pull_request, dict) or not isinstance(pull_request.get("head"), dict):
        raise ReviewError(f"GitHub PR metadata did not contain a head branch for PR #{pr}")
    branch = pull_request["head"].get("ref")
    if not isinstance(branch, str) or not branch:
        raise ReviewError(f"GitHub PR #{pr} did not contain a usable head branch")
    return branch


def codacy_file_records(
    owner: str, repo: str, wanted: set[str], branch: str, token: str | None
) -> dict[str, dict[str, Any]]:
    """Find Codacy file records by exact path, following the v3 cursor."""

    found: dict[str, dict[str, Any]] = {}
    encoded_branch = urllib.parse.quote(branch, safe="")
    next_url = f"/organizations/gh/{owner}/repositories/{repo}/files?limit=100&branch={encoded_branch}"
    seen_cursors: set[str] = set()
    while next_url and len(found) < len(wanted):
        payload = codacy_json(next_url, token)
        for record in payload.get("data", []):
            if isinstance(record, dict) and record.get("path") in wanted:
                found[record["path"]] = record
        pagination = payload.get("pagination") or {}
        cursor = pagination.get("cursor")
        if not cursor or str(cursor) in seen_cursors:
            break
        seen_cursors.add(str(cursor))
        next_url = f"/organizations/gh/{owner}/repositories/{repo}/files?limit=100&cursor={cursor}&branch={encoded_branch}"
    return found


def duplication(owner: str, repo: str, file_id: int, token: str | None) -> dict[str, Any]:
    """Return Codacy clone groups for one generated file ID."""

    return codacy_json(f"/organizations/gh/{owner}/repositories/{repo}/files/{file_id}/duplication", token)


def file_details(owner: str, repo: str, file_id: int, token: str | None) -> dict[str, Any]:
    """Return Codacy's commit identity and authoritative quality block."""

    return codacy_json(f"/organizations/gh/{owner}/repositories/{repo}/files/{file_id}", token)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--owner", required=True, help="GitHub/Codacy organization, for example horocore")
    parser.add_argument("--repo", required=True, help="Repository name, for example horo-engine")
    parser.add_argument("--pr", type=int, help="PR number; changed paths are read with gh api")
    parser.add_argument("--branch", help="Codacy branch name; defaults to the PR head branch")
    parser.add_argument("--path", action="append", default=[], help="Exact repository-relative path; repeatable")
    parser.add_argument("--paths-file", help="Text file containing one exact repository-relative path per line")
    parser.add_argument(
        "--token-env",
        default="CODACY_API_TOKEN",
        help="Environment variable containing an optional Codacy bearer token",
    )
    args = parser.parse_args()
    if args.pr is None and not args.path and not args.paths_file:
        parser.error("provide --pr, --path, or --paths-file")
    return args


def main() -> int:
    args = parse_args()
    paths = set(args.path)
    if args.paths_file:
        with open(args.paths_file, encoding="utf-8") as source:
            paths.update(line.strip() for line in source if line.strip() and not line.lstrip().startswith("#"))
    if args.pr is not None:
        paths.update(changed_paths(args.owner, args.repo, args.pr))
    branch = args.branch or (pull_request_branch(args.owner, args.repo, args.pr) if args.pr is not None else None)
    if not branch:
        raise ReviewError("provide --branch when auditing paths without --pr")
    paths.discard("")
    token = os.environ.get(args.token_env)
    try:
        records = codacy_file_records(args.owner, args.repo, paths, branch, token)
    except CodacyNotFound as error:
        output = {
            "owner": args.owner,
            "repo": args.repo,
            "pr": args.pr,
            "branch": branch,
            "status": "codacy_branch_not_found",
            "error": str(error),
            "paths": sorted(paths),
            "files": [{"path": path, "status": "codacy_branch_not_found"} for path in sorted(paths)],
            "unresolvedPaths": sorted(paths),
        }
        json.dump(output, sys.stdout, indent=2, sort_keys=True)
        sys.stdout.write("\n")
        return 0
    files: list[dict[str, Any]] = []
    for path in sorted(paths):
        record = records.get(path)
        if record is None:
            files.append({"path": path, "status": "codacy_file_not_found"})
            continue
        file_id = record.get("fileId")
        result: dict[str, Any] = {
            "path": path,
            "status": "ok",
            "metrics": {key: record.get(key) for key in (
                "fileId", "branchId", "totalIssues", "grade", "gradeLetter", "complexity",
                "duplication", "numberOfClones", "linesOfCode",
            )},
        }
        if not isinstance(file_id, int):
            result["status"] = "codacy_file_id_missing"
        else:
            try:
                result["fileDetails"] = file_details(args.owner, args.repo, file_id, token)
                result["duplicationDetails"] = duplication(args.owner, args.repo, file_id, token)
            except CodacyNotFound as error:
                result["status"] = "codacy_file_not_found"
                result["error"] = str(error)
            except ReviewError as error:
                result["status"] = "duplication_api_error"
                result["error"] = str(error)
        files.append(result)
    output = {
        "owner": args.owner,
        "repo": args.repo,
        "pr": args.pr,
        "branch": branch,
        "status": "ok",
        "paths": sorted(paths),
        "files": files,
        "unresolvedPaths": sorted(paths - records.keys()),
    }
    json.dump(output, sys.stdout, indent=2, sort_keys=True)
    sys.stdout.write("\n")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except ReviewError as error:
        print(f"error: {error}", file=sys.stderr)
        raise SystemExit(2)
