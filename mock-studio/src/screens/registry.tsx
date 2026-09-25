import type { ComponentType } from 'react';
import type { ReleaseJob, ReleaseRequest } from '../releaseJob';
import { ConsoleDesign } from './ConsoleDesign';
import { AssetBrowserDesign } from './AssetBrowserDesign';
import { AudioMixerDesign } from './AudioMixerDesign';
import { GameplayBehaviorDesign } from './GameplayBehaviorDesign';
import { LoadingDesign } from './LoadingDesign';
import { ModuleConfigDesign } from './ModuleConfigDesign';
import { PcgWorkbenchDesign } from './PcgWorkbenchDesign';
import { SemanticDesign, semanticDesignIds } from './SemanticDesign';
import { ReleaseDesign } from './ReleaseDesign';
import { SettingsDesign, settingsIds } from './SettingsDesign';
import { ShaderGraphDesign } from './ShaderGraphDesign';
import { SimpleWorkbench, simpleWorkbenchIds } from './SimpleWorkbench';
import { TabbedFormDesign, tabbedFormIds } from './TabbedFormDesign';
import { ThreeColumnPanel, threeColumnIds } from './ThreeColumnPanel';
import { WorkspaceDesign } from './WorkspaceDesign';
import { WelcomeDesign } from './WelcomeDesign';

export const workspaceId = 'architecture/editor/editor-workspace.html';
export const releaseId = 'architecture/release/release-modal-design.html';
export const loadingId = 'ui-prototypes/loading-modal.html';

export type ScreenServices = {
  openDesign: (id: string) => void;
  navigate: (id: string) => void;
  closeModal?: () => void;
  releaseJob: ReleaseJob | null;
  startRelease: (request: ReleaseRequest) => void;
  backgroundRelease: () => void;
  cancelRelease: () => void;
  newRelease: () => void;
};

type ScreenProps = ScreenServices & { designId: string };

const screens: Record<string, ComponentType<ScreenProps>> = {
  [workspaceId]: ({ openDesign, releaseJob }) => <WorkspaceDesign openDesign={openDesign} releaseJob={releaseJob} />,
  [releaseId]: ({ releaseJob, startRelease, backgroundRelease, cancelRelease, newRelease, closeModal, navigate }) => <ReleaseDesign job={releaseJob} onStart={startRelease} onBackground={backgroundRelease} onCancel={cancelRelease} onNew={newRelease} onClose={closeModal ?? (() => navigate(workspaceId))} />,
  [loadingId]: ({ navigate, closeModal }) => <LoadingDesign onOpenWorkspace={() => { closeModal?.(); navigate(workspaceId); }} />,
  'architecture/extensions/module-config.html': ModuleConfigDesign,
  'architecture/runtime/console-panel.html': ConsoleDesign,
  'architecture/runtime/shader-graph-editor.html': ShaderGraphDesign,
  'architecture/runtime/pcg-graph-editor.html': PcgWorkbenchDesign,
};

for (const id of threeColumnIds) {
  screens[id] = ({ designId }) => <ThreeColumnPanel designId={designId} />;
}

for (const id of tabbedFormIds) {
  screens[id] = ({ designId }) => <TabbedFormDesign designId={designId} />;
}

for (const id of settingsIds) {
  screens[id] = ({ designId }) => <SettingsDesign designId={designId} />;
}

for (const id of simpleWorkbenchIds) {
  screens[id] = ({ designId }) => <SimpleWorkbench designId={designId} />;
}

for (const id of semanticDesignIds) {
  screens[id] = ({ designId }) => <SemanticDesign designId={designId} />;
}

screens['architecture/editor/asset-browser.html'] = ({ navigate, openDesign }) => <AssetBrowserDesign navigate={navigate} openDesign={openDesign} />;
screens['architecture/editor/welcome-screen.html'] = ({ navigate, openDesign }) => <WelcomeDesign navigate={navigate} openDesign={openDesign} />;
screens['architecture/runtime/audio-mixer.html'] = AudioMixerDesign;
screens['architecture/extensions/gameplay-behavior-editor.html'] = GameplayBehaviorDesign;

export const nativeScreenIds = new Set(Object.keys(screens));

export function NativeScreen({ designId, ...services }: ScreenProps) {
  const Screen = screens[designId];
  return Screen ? <Screen designId={designId} {...services} /> : null;
}
