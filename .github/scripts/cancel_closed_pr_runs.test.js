const assert = require('node:assert/strict');
const test = require('node:test');
const cancelClosedPrRuns = require('./cancel_closed_pr_runs.js');

const context = {
  repo: {owner: 'owner', repo: 'repo'},
  payload: {pull_request: {
    number: 42, created_at: '2026-01-01T00:00:00Z',
    head: {repo: {id: 10}, ref: 'topic', sha: 'current'},
  }},
};
const core = {info() {}};

function workflowRun(overrides = {}) {
  return {
    id: 1, status: 'queued', event: 'pull_request', name: 'CI',
    head_repository: {id: 10}, head_branch: 'topic', head_sha: 'current',
    pull_requests: [{number: 42}], ...overrides,
  };
}

function api(pages, requested, raceStatus) {
  return {
    paginate: {async *iterator(endpoint, args) {
      assert.equal(args.event, 'pull_request');
      assert.equal(args.branch, 'topic');
      assert.equal(args.created, '>=2026-01-01T00:00:00Z');
      for (const data of pages) yield {data};
    }},
    rest: {actions: {
      listWorkflowRunsForRepo() {},
      async cancelWorkflowRun({run_id}) {
        requested.push(run_id);
        if (run_id === 10 && raceStatus) {
          throw Object.assign(new Error('Cancellation rejected'), {status: raceStatus});
        }
      },
    }},
  };
}

for (const status of ['queued', 'in_progress', 'waiting', 'pending', 'requested']) {
  test(`cancel only runs belonging to the closed PR (${status})`, async () => {
    const run = workflowRun({status});
    const cases = [
      run,
      {...run, id: 2, head_sha: 'old'},
      {...run, id: 3, pull_requests: []},
      {...run, id: 4, status: 'completed'},
      {...run, id: 5, event: 'push'},
      {...run, id: 6, pull_requests: [{number: 43}]},
      {...run, id: 7, pull_requests: [], head_repository: {id: 11}},
      {...run, id: 8, pull_requests: [], head_branch: 'main'},
      {...run, id: 9, pull_requests: [], head_sha: 'unrelated'},
      {...run, id: 10},
      {...run, id: 11},
    ];
    const requested = [];
    const github = api([cases.slice(0, 6), cases.slice(6)], requested, 409);
    await cancelClosedPrRuns({github, context, core});
    assert.deepEqual(requested, [1, 2, 3, 10, 11]);
  });
}

test('authorization errors remain failures', async () => {
  const github = api([[workflowRun({id: 10})]], [], 403);
  await assert.rejects(cancelClosedPrRuns({github, context, core}), {status: 403});
});

test('missing source repository cannot match an unrelated unlinked run', async () => {
  const deletedFork = {
    ...context,
    payload: {pull_request: {
      ...context.payload.pull_request,
      head: {...context.payload.pull_request.head, repo: null},
    }},
  };
  const requested = [];
  const github = api([[workflowRun({pull_requests: [], head_repository: null})]], requested);
  await cancelClosedPrRuns({github, context: deletedFork, core});
  assert.deepEqual(requested, []);
});
