import { useState } from 'react';
import { SelectField, TextField } from '../design-system/Controls';
import { buildStages, type WorkflowJob, type WorkflowRequest } from '../workflowJob';
import { WorkflowActivity } from './WorkflowActivity';
import './release.css';

const hostPlatform = typeof navigator === 'undefined' ? 'Linux' : /win/i.test(navigator.platform) ? 'Windows' : /mac/i.test(navigator.platform) ? 'macOS' : 'Linux';

type BuildProfile = {
  name: string;
  product: string;
  platform: string;
  architecture: string;
  configuration: string;
  mode: string;
  content: string;
  output: string;
  toolchain: string;
  parallelJobs: string;
  testAfter: boolean;
  testProfile: string;
};

export function BuildDesign({ job, onStart, onBackground, onCancel, onClose }: { job: WorkflowJob | null; onStart: (request: WorkflowRequest) => void; onBackground: () => void; onCancel: () => void; onClose: () => void }) {
  const [newDraft, setNewDraft] = useState(false);
  const [profiles, setProfiles] = useState<BuildProfile[]>([
    { name: 'Local Development', product: 'Game Runtime', platform: hostPlatform, architecture: 'x86_64', configuration: 'Development', mode: 'Playable Build', content: 'All game content', output: 'builds/local', toolchain: 'Default toolchain', parallelJobs: '16', testAfter: false, testProfile: 'Fast' },
    { name: 'QA Package', product: 'Game Runtime', platform: hostPlatform, architecture: 'x86_64', configuration: 'Test', mode: 'Package for QA', content: 'All game content', output: 'builds/qa', toolchain: 'Default toolchain', parallelJobs: '16', testAfter: true, testProfile: 'Packaged Smoke' },
  ]);
  const [profile, setProfile] = useState('Local Development');
  const [profileName, setProfileName] = useState('');
  const [manageProfiles, setManageProfiles] = useState(false);
  const [product, setProduct] = useState('Game Runtime');
  const [platform, setPlatform] = useState(hostPlatform);
  const [architecture, setArchitecture] = useState('x86_64');
  const [configuration, setConfiguration] = useState('Development');
  const [mode, setMode] = useState('Playable Build');
  const [content, setContent] = useState('All game content');
  const [output, setOutput] = useState('builds/local');
  const [toolchain, setToolchain] = useState('Default toolchain');
  const [parallelJobs, setParallelJobs] = useState('16');
  const [runAfter, setRunAfter] = useState(false);
  const [testAfter, setTestAfter] = useState(false);
  const [testProfile, setTestProfile] = useState('Fast');
  const compileOnly = mode === 'Compile Only';
  const canRun = !compileOnly && product === 'Game Runtime' && platform === hostPlatform;

  function selectProfile(value: string) {
    const saved = profiles.find(item => item.name === value);
    if (!saved) return;
    setProfile(value);
    setProduct(saved.product);
    setPlatform(saved.platform);
    setArchitecture(saved.architecture);
    setConfiguration(saved.configuration);
    setMode(saved.mode);
    setContent(saved.content);
    setOutput(saved.output);
    setToolchain(saved.toolchain);
    setParallelJobs(saved.parallelJobs);
    setTestAfter(saved.testAfter);
    setTestProfile(saved.testProfile);
    setRunAfter(false);
  }

  function saveProfile() {
    const name = profileName.trim();
    if (!name || profiles.some(item => item.name === name)) return;
    setProfiles(current => [...current, { name, product, platform, architecture, configuration, mode, content, output, toolchain, parallelJobs, testAfter, testProfile }]);
    setProfile(name);
    setProfileName('');
  }

  function start() {
    if (!output.trim() || !Number.isInteger(Number(parallelJobs)) || Number(parallelJobs) < 1) {
      window.alert('Enter an output directory and a positive number of parallel jobs.');
      return;
    }
    setNewDraft(false);
    onStart({ kind: 'build', title: 'Build', target: `${product} · ${platform} · ${architecture} · ${configuration}`, output, stages: buildStages(testAfter, runAfter && canRun, compileOnly) });
  }

  if (job && !newDraft) return <WorkflowActivity job={job} onBackground={onBackground} onCancel={onCancel} onNew={() => setNewDraft(true)} onClose={onClose} />;

  return <div className="release-design release-draft"><header className="release-header"><strong>Build</strong></header><div className="release-summary"><span className="release-pill"><i />DRAFT</span><div><small>Workflow</small><strong>{mode}{testAfter ? ' + Tests' : ''}{runAfter && canRun ? ' + Run' : ''}</strong></div><div><small>Target</small><strong>{platform} · {architecture} · {configuration}</strong></div></div>
    <main className="release-content"><div className="release-quick-form">
      <div className="release-profile-row"><SelectField label="Build profile" value={profile} onChange={event => selectProfile(event.target.value)}>{profiles.map(item => <option key={item.name}>{item.name}</option>)}</SelectField><button type="button" onClick={() => setManageProfiles(value => !value)} aria-expanded={manageProfiles}>Manage profiles</button></div>
      {manageProfiles && <div className="release-profile-manager"><strong>Saved build profiles</strong><p>Profiles keep target and build preferences. This mock does not store credentials.</p><div className="release-inline"><input aria-label="New profile name" placeholder="Profile name" value={profileName} onChange={event => setProfileName(event.target.value)} /><button type="button" onClick={saveProfile} disabled={!profileName.trim() || profiles.some(item => item.name === profileName.trim())}>Save current settings</button></div></div>}
      <div className="release-row"><SelectField label="Product" value={product} onChange={event => { setProduct(event.target.value); setRunAfter(false); }}><option>Game Runtime</option><option>Game Dedicated Server</option><option>Engine Editor</option><option>Engine CLI</option><option>SDK</option></SelectField><SelectField label="Workflow" value={mode} onChange={event => { setMode(event.target.value); if (event.target.value === 'Compile Only') setRunAfter(false); }}><option>Playable Build</option><option>Compile Only</option><option>Package for QA</option></SelectField></div>
      <div className="release-row"><SelectField label="Platform" value={platform} onChange={event => { setPlatform(event.target.value); setRunAfter(false); }}><option>Windows</option><option>macOS</option><option>Linux</option></SelectField><SelectField label="Architecture" value={architecture} onChange={event => setArchitecture(event.target.value)}><option>x86_64</option><option>arm64</option></SelectField></div>
      <SelectField label="Configuration" value={configuration} onChange={event => setConfiguration(event.target.value)}><option>Development</option><option>Test</option><option>Shipping</option></SelectField>
      {!compileOnly && <SelectField label="Content" value={content} onChange={event => setContent(event.target.value)}><option>All game content</option><option>Selected scenes and dependencies</option></SelectField>}
      <div className="release-toggle-list"><label><input type="checkbox" checked={testAfter} onChange={event => setTestAfter(event.target.checked)} /> Test after build</label>{testAfter && <SelectField label="Test profile" value={testProfile} onChange={event => setTestProfile(event.target.value)}><option>Fast</option><option>Play Mode</option><option>Content Validation</option><option>Packaged Smoke</option></SelectField>}<label><input type="checkbox" checked={runAfter} disabled={!canRun} onChange={event => setRunAfter(event.target.checked)} /> Run after build</label>{!canRun && <small>Run is available for a playable Game Runtime build targeting this {hostPlatform} machine.</small>}</div>
      <details className="release-advanced"><summary>Advanced build options <span>Toolchain, parallel jobs, output</span></summary><div className="release-row"><SelectField label="Toolchain" value={toolchain} onChange={event => setToolchain(event.target.value)}><option>Default toolchain</option><option>Clang 17</option><option>MSVC 2022</option></SelectField><TextField label="Parallel jobs" type="number" min="1" value={parallelJobs} onChange={event => setParallelJobs(event.target.value)} /></div><TextField label="Output directory" value={output} onChange={event => setOutput(event.target.value)} /></details>
      <p className="release-notice">Local build preview. This action does not create a versioned release candidate or publish anything.</p>
    </div></main><footer className="release-footer"><div className="release-actions"><button type="button" className="primary" onClick={start}>Build{testAfter ? ' & Test' : ''}{runAfter && canRun ? ' & Run' : ''}</button></div></footer>
  </div>;
}
