---
name: codacy-pr-review
description: Review GitHub pull requests with Codacy's public v3 API, resolving changed-file IDs, duplication clones, file metrics, and actionable quality findings; use when PR readiness depends on Codacy.
---

# Codacy PR review

Use this skill for repository PR audits where Codacy metrics, duplication details, AI review comments, or the Codacy check must be verified. The workflow is read-only until the user explicitly authorizes code changes or GitHub comment mutations. Do not use a browser: use `gh api` for GitHub data and the Codacy v3 HTTP API for Codacy data.

## Workflow

1. Resolve the PR head SHA and changed paths with `gh api repos/{owner}/{repo}/pulls/{number}` and `pulls/{number}/files?per_page=100`.
2. Run `scripts/codacy_pr_review.py --owner {owner} --repo {repo} --pr {number}`. The script resolves the PR head branch, paginates Codacy's branch-specific file list, maps each changed path to its exact `fileId`, and queries `/files/{fileId}/issues` and `/files/{fileId}/duplication`.
3. Treat a missing file mapping as unknown, never as zero duplication. Query `/files/{fileId}` for the analyzed commit and authoritative `quality` block, `/files/{fileId}/issues` for every Codacy issue, and `/files/{fileId}/duplication` for clone groups. Treat `pagination.total` as the number of returned duplication occurrences, not as the number of clone groups; inspect each `data[]` group and its line ranges.
4. Record file-level `totalIssues`, the complete `issueDetails.data[]`, `complexity`, `duplication`, `numberOfClones`, and coverage when available. Use `totalIssues == 0` and `duplication <= 2` as default targets; treat complexity `< 150` as advisory unless the user sets different thresholds.
5. Verify the current head's `Codacy Static Code Analysis` check with `gh api repos/{owner}/{repo}/commits/{sha}/check-runs`. Do not use a stale check from an earlier head.
6. Inspect Codacy/GitHub review threads with GraphQL. Every actionable thread needs either the requested change or a concrete explanation that the suggestion is invalid/stale, followed by a reply and `resolveReviewThread`; never resolve silently.
7. Implement applicable Codacy test suggestions, run the narrowest relevant tests, and then re-query the PR head, checks, review threads, and Codacy file/duplication data.

## API and mutation boundaries

- The Codacy file list is paginated (`limit` is at most 100 and the response returns a cursor). Follow cursors until every requested path is found or the list is exhausted.
- File IDs are Codacy-generated and may differ by branch/analysis. Resolve them from the branch-specific API response for the current review; do not hard-code an ID copied from a prior PR. Codacy's file-list filter is `branch={url-encoded branch name}`.
- If Codacy returns 404 for the PR branch, report Codacy branch analysis as unavailable and keep the metrics unknown; do not silently fall back to the default branch and call that a PR result.
- The file endpoint is `/api/v3/organizations/gh/{owner}/repositories/{repo}/files/{fileId}`; it identifies the analyzed commit and returns `quality` metrics. The issues endpoint is `/api/v3/organizations/gh/{owner}/repositories/{repo}/files/{fileId}/issues`; the duplication endpoint is `/api/v3/organizations/gh/{owner}/repositories/{repo}/files/{fileId}/duplication`. Preserve issue IDs/messages, clone-group IDs, and every occurrence path/line range in the report.
- Use `gh api graphql` for review-thread queries and mutations. Reply before resolving. Keep replies factual and scoped to the actual finding.
- Do not merge, close, force-push, or alter branch bases unless the user explicitly asks. Code commits and pushes are allowed only when the surrounding task explicitly authorizes PR fixes; amend/fix the existing delivery commit when repository policy requires it.
- Report Codacy server metrics separately from local Codacy/Lizard or Sonar results. A local metric is not evidence that the hosted Codacy gate is clean.

## Reusable helper

The bundled `scripts/codacy_pr_review.py` has no third-party dependencies. It uses `gh api` to obtain changed PR paths and Python's standard library for the public Codacy API. Use `--path` instead of `--pr` when auditing a known file, and keep the JSON output as evidence for the final review.
