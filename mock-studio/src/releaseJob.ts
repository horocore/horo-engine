export const releaseStages = [
  { label: 'Validate', activity: 'Checking project version, source tag, and release notes…', log: 'Project and release metadata validated' },
  { label: 'Configure', activity: 'Configuring target toolchain…', log: 'Target toolchain and signing profile configured' },
  { label: 'Build', activity: 'Compiling runtime and editor modules…', log: 'Runtime and editor modules built' },
  { label: 'Cook', activity: 'Cooking textures, meshes, and shaders…', log: 'Textures, meshes, and shader permutations cooked' },
  { label: 'Package', activity: 'Packaging assets.horo…', log: 'Assets and runtime files packaged' },
  { label: 'Pre-Verify', activity: 'Checking candidate contents…', log: 'Candidate contents checked' },
  { label: 'Sign', activity: 'Signing release artifacts…', log: 'Release artifacts signed' },
  { label: 'Finalize', activity: 'Promoting verified candidate…', log: 'Verified candidate promoted to output directory' },
  { label: 'Final Verify', activity: 'Verifying final checksums…', log: 'Final checksums verified' },
] as const;

export type ReleaseJob = {
  id: number;
  target: string;
  path: string;
  name: string;
  version: string;
  stageIndex: number;
  status: 'running' | 'success' | 'cancelled';
  background: boolean;
  startedAt: number;
  finishedAt?: number;
};

export type ReleaseRequest = Pick<ReleaseJob, 'target' | 'path' | 'name' | 'version'>;

export function releaseActivity(job: ReleaseJob): string {
  if (job.status === 'success') return 'Build completed';
  if (job.status === 'cancelled') return 'Build cancelled';
  return releaseStages[job.stageIndex].activity;
}
