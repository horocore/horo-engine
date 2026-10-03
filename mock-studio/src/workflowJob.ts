export type WorkflowStage = { label: string; activity: string; log: string };

export type WorkflowJob = {
  id: number;
  kind: 'build' | 'test' | 'publish';
  title: string;
  target: string;
  output: string;
  stages: WorkflowStage[];
  stageIndex: number;
  status: 'running' | 'success' | 'cancelled';
  background: boolean;
  startedAt: number;
  finishedAt?: number;
};

export type WorkflowRequest = Pick<WorkflowJob, 'kind' | 'title' | 'target' | 'output' | 'stages'>;

export function workflowActivity(job: WorkflowJob): string {
  if (job.status === 'success') return `${job.title} completed`;
  if (job.status === 'cancelled') return `${job.title} cancelled`;
  return job.stages[job.stageIndex].activity;
}

export function buildStages(testAfter: boolean, runAfter: boolean, compileOnly: boolean): WorkflowStage[] {
  const stages: WorkflowStage[] = [
    { label: 'Validate', activity: 'Checking target and toolchain…', log: 'Target and toolchain validated' },
    { label: 'Compile', activity: 'Compiling game modules…', log: 'Game modules compiled' },
  ];
  if (!compileOnly) stages.push(
    { label: 'Cook', activity: 'Cooking game assets…', log: 'Runtime assets cooked' },
    { label: 'Package', activity: 'Packaging playable build…', log: 'Playable build packaged' },
  );
  if (testAfter) stages.push({ label: 'Test', activity: 'Running selected game tests…', log: 'Selected game tests passed' });
  if (runAfter && !compileOnly) stages.push({ label: 'Run', activity: 'Launching packaged game…', log: 'Packaged game launched' });
  return stages;
}

export function testStages(source: 'Project' | 'Existing package', profile: string): WorkflowStage[] {
  return [
    { label: 'Discover', activity: `Discovering ${profile} tests…`, log: `${profile} test descriptors discovered` },
    { label: 'Run', activity: `Running ${profile} tests against ${source.toLowerCase()}…`, log: `${profile} tests passed` },
    { label: 'Report', activity: 'Preparing test report…', log: 'Test report ready · 12 passed · 0 failed · 1 skipped' },
  ];
}

export const publishStages: WorkflowStage[] = [
  { label: 'Validate', activity: 'Checking candidate and destination…', log: 'Verified candidate and destination authorized' },
  { label: 'Upload', activity: 'Uploading immutable candidate…', log: 'Candidate uploaded without rebuilding' },
  { label: 'Verify', activity: 'Verifying uploaded hashes…', log: 'Published bytes match candidate checksums' },
];
