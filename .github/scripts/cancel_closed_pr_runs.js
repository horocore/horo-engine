/** Match API PR links, or the exact source identity when links are omitted. */
function belongsToClosedPr(run, pr) {
  const links = run.pull_requests || [];
  if (links.length > 0) return links.some(item => item.number === pr.number);
  return pr.head.repo?.id != null
    && run.head_repository?.id === pr.head.repo.id
    && run.head_branch === pr.head.ref && run.head_sha === pr.head.sha;
}

/** Cancel active workflow runs belonging to one closed PR; preserve main and other PRs. */
module.exports = async function cancelClosedPrRuns({github, context, core}) {
  const pr = context.payload.pull_request;
  const { owner, repo } = context.repo;
  const active = new Set(['queued', 'in_progress', 'waiting', 'pending', 'requested']);
  let cancelled = 0;
  for await (const page of github.paginate.iterator(github.rest.actions.listWorkflowRunsForRepo, {
    owner, repo, event: 'pull_request', branch: pr.head.ref,
    created: `>=${pr.created_at}`, per_page: 100,
  })) {
    for (const run of page.data) {
      if (!active.has(run.status) || run.event !== 'pull_request') continue;
      if (!belongsToClosedPr(run, pr)) continue;
      try {
        await github.rest.actions.cancelWorkflowRun({ owner, repo, run_id: run.id });
        core.info(`Requested cancellation: ${run.name} (${run.id})`);
        cancelled++;
      } catch (error) {
        if (error.status !== 409) throw error;
        core.info(`Run ${run.id} changed state before cancellation.`);
      }
    }
  }
  core.info(`Requested cancellation for ${cancelled} runs belonging to closed PR #${pr.number}.`);
};
