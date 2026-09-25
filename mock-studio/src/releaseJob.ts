export const releaseStages = [
  { label: 'Validate', activity: 'Checking version, source, packages, and target…', log: 'Version, source revision, locked packages, and target validated' },
  { label: 'Configure', activity: 'Configuring target toolchain…', log: 'Target toolchain configured' },
  { label: 'Build', activity: 'Compiling game runtime…', log: 'Game runtime built' },
  { label: 'Cook', activity: 'Cooking textures, meshes, and shaders…', log: 'Textures, meshes, and shader permutations cooked' },
  { label: 'Package', activity: 'Packaging assets.horo…', log: 'Assets and runtime files packaged' },
  { label: 'Pre-Verify', activity: 'Checking staged package contents…', log: 'Staged package and archive verified' },
  { label: 'Sign', activity: 'Signing release artifacts…', log: 'Release artifacts signed using configured profile' },
  { label: 'Finalize', activity: 'Finalizing candidate metadata…', log: 'Candidate manifest and checksums finalized' },
  { label: 'Final Verify', activity: 'Running packaged smoke and final verification…', log: 'Final checksums and packaged-player smoke verified' },
] as const;

export type ReleaseJob = {
  id: number;
  target: string;
  path: string;
  name: string;
  version: string;
  profile: string;
  sourceRevision: string;
  notes: string;
  content: string;
  testProfile: string;
  signingProfile: string;
  packageFormat: string;
  stageIndex: number;
  status: 'running' | 'success' | 'cancelled';
  background: boolean;
  startedAt: number;
  finishedAt?: number;
};

export type ReleaseRequest = Pick<ReleaseJob, 'target' | 'path' | 'name' | 'version' | 'profile' | 'sourceRevision' | 'notes' | 'content' | 'testProfile' | 'signingProfile' | 'packageFormat'>;

export function releaseActivity(job: ReleaseJob): string {
  if (job.status === 'success') return 'Release candidate ready';
  if (job.status === 'cancelled') return 'Release preparation cancelled';
  return releaseStages[job.stageIndex].activity;
}
