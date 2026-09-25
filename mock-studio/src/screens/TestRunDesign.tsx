import { useState } from 'react';
import { SelectField, TextField } from '../design-system/Controls';
import { testStages, type WorkflowJob, type WorkflowRequest } from '../workflowJob';
import { WorkflowActivity } from './WorkflowActivity';
import './release.css';

export function TestRunDesign({ job, onStart, onBackground, onCancel, onClose }: { job: WorkflowJob | null; onStart: (request: WorkflowRequest) => void; onBackground: () => void; onCancel: () => void; onClose: () => void }) {
  const [newDraft, setNewDraft] = useState(false);
  const [source, setSource] = useState<'Project' | 'Existing package'>('Project');
  const [packagePath, setPackagePath] = useState('builds/local/DesertRun');
  const [profile, setProfile] = useState('Fast');
  const [filter, setFilter] = useState('');
  const [platform, setPlatform] = useState('Linux');
  const [timeout, setTimeoutValue] = useState('120');

  function start() {
    if (source === 'Existing package' && !packagePath.trim()) { window.alert('Select a packaged build to test.'); return; }
    if (!Number.isInteger(Number(timeout)) || Number(timeout) < 1) { window.alert('Enter a positive timeout.'); return; }
    setNewDraft(false);
    onStart({ kind: 'test', title: 'Test run', target: `${source} · ${platform} · ${profile}${filter ? ` · ${filter}` : ''}`, output: source === 'Project' ? 'Project test report' : packagePath, stages: testStages(source, profile) });
  }

  if (job && !newDraft) return <WorkflowActivity job={job} onBackground={onBackground} onCancel={onCancel} onNew={() => setNewDraft(true)} onClose={onClose} />;

  return <div className="release-design release-draft"><header className="release-header"><strong>Run Tests</strong></header><div className="release-summary"><span className="release-pill"><i />READY</span><div><small>Source</small><strong>{source}</strong></div><div><small>Profile</small><strong>{profile}</strong></div></div><main className="release-content"><div className="release-quick-form"><h2>Test selection</h2><div className="release-row"><SelectField label="Source" value={source} onChange={event => setSource(event.target.value as 'Project' | 'Existing package')}><option>Project</option><option>Existing package</option></SelectField><SelectField label="Test profile" value={profile} onChange={event => setProfile(event.target.value)}><option>Fast</option><option>Play Mode</option><option>Content Validation</option><option>Visual</option><option>Packaged Smoke</option></SelectField></div>{source === 'Existing package' && <TextField label="Packaged build" hint="Tests run against this existing package; no build is started." value={packagePath} onChange={event => setPackagePath(event.target.value)} />}<TextField label="Test name or tag filter" placeholder="All tests in profile" value={filter} onChange={event => setFilter(event.target.value)} /><div className="release-row"><SelectField label="Target platform" value={platform} onChange={event => setPlatform(event.target.value)}><option>Linux</option><option>Windows</option><option>macOS</option></SelectField><TextField label="Timeout (seconds)" type="number" min="1" value={timeout} onChange={event => setTimeoutValue(event.target.value)} /></div><p className="release-notice">This mock discovers tests and shows a sample report. It never starts a game build.</p></div></main><footer className="release-footer"><div className="release-actions"><button type="button" className="primary" onClick={start}>Run tests</button></div></footer></div>;
}
