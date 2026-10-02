/* eslint-disable no-unused-vars -- TypeScript callback parameter names are checked by @typescript-eslint/no-unused-vars. */
import type { ComponentType } from 'react';
import type { ReleaseJob, ReleaseRequest } from '../releaseJob';
import type { WorkflowJob, WorkflowRequest } from '../workflowJob';
import { BuildDesign } from './BuildDesign';
import { ConsoleDesign } from './ConsoleDesign';
import { AssetBrowserDesign } from './AssetBrowserDesign';
import { AudioMixerDesign } from './AudioMixerDesign';
import { GameplayBehaviorDesign } from './GameplayBehaviorDesign';
import { LoadingDesign } from './LoadingDesign';
import { ModuleConfigDesign } from './ModuleConfigDesign';
import { PcgWorkbenchDesign } from './PcgWorkbenchDesign';
import { SemanticDesign, semanticDesignIds } from './SemanticDesign';
import { ReleaseDesign } from './ReleaseDesign';
import { PublishDesign } from './PublishDesign';
import { TestRunDesign } from './TestRunDesign';
import { SettingsDesign, settingsIds } from './SettingsDesign';
import { ShaderGraphDesign } from './ShaderGraphDesign';
import { SimpleWorkbench, simpleWorkbenchIds } from './SimpleWorkbench';
import { TabbedFormDesign, tabbedFormIds } from './TabbedFormDesign';
import { ThreeColumnPanel, threeColumnIds } from './ThreeColumnPanel';
import { WorkspaceDesign } from './WorkspaceDesign';
import { WelcomeDesign } from './WelcomeDesign';

export const workspaceId = 'architecture/editor/editor-workspace.html';
export const releaseId = 'architecture/release/release-modal-design.html';
export const buildId = 'architecture/build/build-job.html';
export const testId = 'architecture/build/test-run.html';
export const publishId = 'architecture/release/publish-candidate.html';
export const loadingId = 'ui-prototypes/loading-modal.html';

export type ScreenServices = {
  openDesign: (id: string) => void;
  navigate: (id: string) => void;
  closeModal?: () => void;
  releaseJob: ReleaseJob | null;
  releaseCandidates: ReleaseJob[];
  workflowJobs: WorkflowJob[];
  startRelease: (request: ReleaseRequest) => void;
  backgroundRelease: () => void;
  cancelRelease: () => void;
  newRelease: () => void;
  startWorkflow: (request: WorkflowRequest) => void;
  backgroundWorkflow: (id: number) => void;
  cancelWorkflow: (id: number) => void;
};

type ScreenProps = ScreenServices & { designId: string };

const staticScreens: Record<string, ComponentType<ScreenProps>> = {
  [workspaceId]: ({ openDesign, releaseJob, releaseCandidates, workflowJobs }) => <WorkspaceDesign openDesign={openDesign} releaseJob={releaseJob} releaseCandidates={releaseCandidates} workflowJobs={workflowJobs} />,
  [buildId]: ({ workflowJobs, startWorkflow, backgroundWorkflow, cancelWorkflow, closeModal, navigate }) => { const job = workflowJobs.filter(item => item.kind === 'build').at(-1) ?? null; return <BuildDesign job={job} onStart={startWorkflow} onBackground={() => { if (job) backgroundWorkflow(job.id); }} onCancel={() => { if (job) cancelWorkflow(job.id); }} onClose={closeModal ?? (() => { navigate(workspaceId); })} />; },
  [testId]: ({ workflowJobs, startWorkflow, backgroundWorkflow, cancelWorkflow, closeModal, navigate }) => { const job = workflowJobs.filter(item => item.kind === 'test').at(-1) ?? null; return <TestRunDesign job={job} onStart={startWorkflow} onBackground={() => { if (job) backgroundWorkflow(job.id); }} onCancel={() => { if (job) cancelWorkflow(job.id); }} onClose={closeModal ?? (() => { navigate(workspaceId); })} />; },
  [releaseId]: ({ releaseJob, startRelease, backgroundRelease, cancelRelease, newRelease, closeModal, navigate }) => <ReleaseDesign job={releaseJob} onStart={startRelease} onBackground={backgroundRelease} onCancel={cancelRelease} onNew={newRelease} onClose={closeModal ?? (() => { navigate(workspaceId); })} />,
  [publishId]: ({ releaseJob, releaseCandidates, workflowJobs, startWorkflow, backgroundWorkflow, cancelWorkflow, closeModal, navigate }) => { const job = workflowJobs.filter(item => item.kind === 'publish').at(-1) ?? null; const candidates = [...releaseCandidates, ...(releaseJob?.status === 'success' ? [releaseJob] : [])]; return <PublishDesign candidates={candidates} job={job} onStart={startWorkflow} onBackground={() => { if (job) backgroundWorkflow(job.id); }} onCancel={() => { if (job) cancelWorkflow(job.id); }} onClose={closeModal ?? (() => { navigate(workspaceId); })} />; },
  [loadingId]: ({ navigate, closeModal }) => <LoadingDesign onOpenWorkspace={() => { closeModal?.(); navigate(workspaceId); }} />,
  'architecture/extensions/module-config.html': ModuleConfigDesign,
  'architecture/runtime/console-panel.html': ConsoleDesign,
  'architecture/runtime/shader-graph-editor.html': ShaderGraphDesign,
  'architecture/runtime/pcg-graph-editor.html': PcgWorkbenchDesign,
};
const screens = new Map(Object.entries(staticScreens));

for (const id of threeColumnIds) {
  screens.set(id, ({ designId }) => <ThreeColumnPanel designId={designId} />);
}

for (const id of tabbedFormIds) {
  screens.set(id, ({ designId }) => <TabbedFormDesign designId={designId} />);
}

for (const id of settingsIds) {
  screens.set(id, ({ designId }) => <SettingsDesign designId={designId} />);
}

for (const id of simpleWorkbenchIds) {
  screens.set(id, ({ designId }) => <SimpleWorkbench designId={designId} />);
}

for (const id of semanticDesignIds) {
  screens.set(id, ({ designId }) => <SemanticDesign designId={designId} />);
}

screens.set('architecture/editor/asset-browser.html', ({ navigate, openDesign }) => <AssetBrowserDesign navigate={navigate} openDesign={openDesign} />);
screens.set('architecture/editor/welcome-screen.html', ({ navigate, openDesign }) => <WelcomeDesign navigate={navigate} openDesign={openDesign} />);
screens.set('architecture/runtime/audio-mixer.html', AudioMixerDesign);
screens.set('architecture/extensions/gameplay-behavior-editor.html', GameplayBehaviorDesign);

export const nativeScreenIds = new Set(screens.keys());

const ignorePreviewAction = () => { /* Catalog previews do not run workflow actions. */ };

const previewServices: Omit<ScreenServices, 'openDesign' | 'navigate'> = {
  releaseJob: null,
  releaseCandidates: [],
  workflowJobs: [],
  startRelease: ignorePreviewAction,
  backgroundRelease: ignorePreviewAction,
  cancelRelease: ignorePreviewAction,
  newRelease: ignorePreviewAction,
  startWorkflow: ignorePreviewAction,
  backgroundWorkflow: ignorePreviewAction,
  cancelWorkflow: ignorePreviewAction,
};

export function NativeScreen({ designId, ...services }: ScreenProps) {
  const Screen = screens.get(designId);
  return Screen ? <Screen designId={designId} {...previewServices} {...services} /> : null;
}
