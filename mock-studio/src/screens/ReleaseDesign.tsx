import { useEffect, useState } from 'react';
import { SelectField, TextField } from '../design-system/Controls';
import { releaseActivity, releaseStages, type ReleaseJob, type ReleaseRequest } from '../releaseJob';
import './release.css';

const tabs = ['Setup', 'Build Options', 'Packages', 'Delivery', 'Review', 'Activity'] as const;
type Tab = typeof tabs[number];

export function ReleaseDesign({ job, onStart, onBackground, onCancel, onNew, onClose }: { job: ReleaseJob | null; onStart: (request: ReleaseRequest) => void; onBackground: () => void; onCancel: () => void; onNew: () => void; onClose: () => void }) {
  const [tab, setTab] = useState<Tab>(job ? 'Activity' : 'Setup');
  const [profile, setProfile] = useState('Game Runtime');
  const [platform, setPlatform] = useState('Windows');
  const [architecture, setArchitecture] = useState('x86_64');
  const [configuration, setConfiguration] = useState('Shipping');
  const [releaseName, setReleaseName] = useState('DesertRun v0.4.2');
  const [version, setVersion] = useState('0.4.2');
  const [notes, setNotes] = useState('docs/releases/0.4.2.md');
  const [minimumEngine, setMinimumEngine] = useState('0.4.2');
  const [outputRoot, setOutputRoot] = useState('releases');
  const [signing, setSigning] = useState('Production Authenticode');
  const [protection, setProtection] = useState('AES-256-GCM + KDF');
  const [toolchain, setToolchain] = useState('MSVC 2022');
  const [jobs, setJobs] = useState('16');

  useEffect(() => {
    if (job) setTab('Activity');
  }, [job?.status, job?.id]);

  const target = job?.target ?? `${platform} · ${architecture} · ${configuration}`;
  const path = job?.path ?? `${outputRoot.replace(/[/\\]+$/, '')}/${version}_${platform.toLowerCase()}_${architecture}_${configuration.toLowerCase()}/`;
  const running = job?.status === 'running';
  const completed = job?.status === 'success';
  const elapsedSeconds = job ? Math.floor(((job.status === 'running' ? Date.now() : job.finishedAt ?? job.startedAt + releaseStages.length * 2400) - job.startedAt) / 1000) : 0;
  const flow: Tab[] = ['Setup', 'Delivery', 'Review'];
  const flowIndex = flow.indexOf(tab);

  function continueFlow() { setTab(flow[Math.min(2, flowIndex < 0 ? 1 : flowIndex + 1)]); }
  function back() { setTab(flow[Math.max(0, flowIndex < 0 ? 0 : flowIndex - 1)]); }
  function start() {
    if (!/^\d+\.\d+\.\d+(?:-[\da-z.-]+)?(?:\+[\da-z.-]+)?$/i.test(version) || !outputRoot.trim()) {
      window.alert('Enter a semantic version and an output root before starting.');
      setTab('Setup');
      return;
    }
    onStart({ target, path, name: releaseName, version });
    setTab('Activity');
  }

  return <div className="release-design">
    <header className="release-header"><strong>Build &amp; Release</strong></header>
    {!job ? <div className="release-journey">
      {['Configure', 'Review request', 'Build & verify', 'Publish separately'].map((label, index) => <div key={label} className={index === (tab === 'Review' ? 1 : 0) ? 'current' : ''}><b>{index + 1}</b> {label}</div>)}
    </div> : <div className="release-stages">{releaseStages.map((stage, index) => <div key={stage.label} className={completed || index < job.stageIndex ? 'done' : index === job.stageIndex && running ? 'current' : ''}><i /><span><em>{String(index + 1).padStart(2, '0')}</em> {stage.label}</span></div>)}</div>}
    <div className="release-summary">
      <span className={`release-pill ${completed ? 'success' : ''}`}><i />{completed ? 'COMPLETED' : running ? releaseStages[job.stageIndex].label.toUpperCase() : job?.status === 'cancelled' ? 'CANCELLED' : 'DRAFT'}</span>
      <div><small>Target</small><strong>{target}</strong></div>
      {job && <div><small>Elapsed</small><strong>{String(Math.floor(elapsedSeconds / 60)).padStart(2, '0')}:{String(elapsedSeconds % 60).padStart(2, '0')}</strong></div>}
      <div className="release-candidate"><small>Candidate path</small><strong>{path}</strong></div>
      {running && <span className="release-throughput">186 <small>MB/s</small></span>}
    </div>
    <nav className="release-tabs" aria-label="Release sections">{tabs.filter(item => job ? item === 'Activity' : item !== 'Activity').map(item => <button key={item} type="button" className={tab === item ? 'active' : ''} onClick={() => setTab(item)}>{item}{item === 'Packages' && <b>3</b>}</button>)}</nav>
    <main className="release-content">
      {tab === 'Setup' && <>
        <h2>Product Profile</h2>
        <SelectField label="What are you releasing?" hint="The profile determines which runtime content and tools belong in the candidate." value={profile} onChange={event => setProfile(event.target.value)}>
          {['Game Runtime', 'Game Dedicated Server', 'Engine Editor', 'Engine CLI', 'SDK', 'Developer Diagnostics'].map(item => <option key={item}>{item}</option>)}
        </SelectField>
        <h2>Target</h2>
        <div className="release-row">
          <SelectField label="Platform" value={platform} onChange={event => setPlatform(event.target.value)}>{['Windows', 'macOS', 'Linux'].map(item => <option key={item}>{item}</option>)}</SelectField>
          <SelectField label="Architecture" value={architecture} onChange={event => setArchitecture(event.target.value)}><option>x86_64</option><option>arm64</option></SelectField>
        </div>
        <SelectField label="Configuration" hint="Each job produces one platform, architecture, and configuration." value={configuration} onChange={event => setConfiguration(event.target.value)}><option>Shipping</option><option>Development</option><option>Test</option></SelectField>
        <h2>Release Details</h2>
        <div className="release-row">
          <TextField label="Release name" value={releaseName} onChange={event => setReleaseName(event.target.value)} />
          <TextField label="Version" hint="Must match the project, source tag, manifest, and release notes." value={version} onChange={event => setVersion(event.target.value)} />
        </div>
        <div className="release-row">
          <TextField label="Release notes file" hint="Project-relative Markdown path." value={notes} onChange={event => setNotes(event.target.value)} />
          <TextField label="Minimum engine version" value={minimumEngine} onChange={event => setMinimumEngine(event.target.value)} />
        </div>
        <p className="release-notice">Source revision comes from the selected project checkout. Validation confirms its version and tag before build work begins.</p>
      </>}
      {tab === 'Build Options' && <>
        <h2>Build Options</h2>
        <div className="release-row"><SelectField label="Toolchain" value={toolchain} onChange={event => setToolchain(event.target.value)}><option>MSVC 2022</option><option>Clang 17</option><option>MinGW</option></SelectField><TextField label="Parallel jobs" type="number" min="1" value={jobs} onChange={event => setJobs(event.target.value)} /></div>
        <p className="release-notice">These options are scoped to this candidate. The service validates that the toolchain supports the selected target.</p>
      </>}
      {tab === 'Packages' && <><h2>Packages</h2><p className="release-notice">Three sample package entries are shown. Package resolution is validated when the job begins.</p><div className="release-review"><div><span>horo.core.physics</span><strong>1.2.4 · Included</strong></div><div><span>vendor.fmod</span><strong>2.02.20 · Included</strong></div><div><span>horo.vfx.particles</span><strong>0.9.1 · Optional</strong></div></div></>}
      {tab === 'Delivery' && <><p className="release-notice">This job creates a verified local candidate. Storefront and CDN publication is a separate promotion after final verification.</p><h2>Local candidate</h2><TextField label="Output root" hint="The service stages privately and promotes the verified candidate atomically." value={outputRoot} onChange={event => setOutputRoot(event.target.value)} /><output className="release-path">{path}</output><h2>Security</h2><div className="release-row"><SelectField label="Signing profile" hint="Credentials remain in the configured profile." value={signing} onChange={event => setSigning(event.target.value)}><option>Production Authenticode</option><option>Test Certificate</option><option>None</option></SelectField><SelectField label="Archive protection" value={protection} onChange={event => setProtection(event.target.value)}><option>AES-256-GCM + KDF</option><option>None</option></SelectField></div></>}
      {tab === 'Review' && <><h2>Request summary</h2><div className="release-review">{[['Product', profile], ['Target', target], ['Release', releaseName], ['Version', version], ['Release notes', notes], ['Candidate path', path], ['Signing', signing]].map(([label, value]) => <div key={label}><span>{label}</span><strong>{value}</strong></div>)}</div><h2>Checks at start</h2><ul className="release-checks"><li>Project version, source tag, manifest identity, and release notes agree.</li><li>Toolchain supports the target and requested profile.</li><li>Packages, output root, signing profile, and credentials are valid.</li></ul></>}
      {tab === 'Activity' && job && <><p className={`release-notice ${completed ? 'release-notice-success' : ''}`}>{completed ? `Mock build completed. ${job.name} passed final verification; candidate: ${job.path}. No files were written.` : `Mock job #${job.id.toString().slice(-5)} ${running ? 'is running' : 'was cancelled'}. This preview does not submit an actual build; closing this window keeps its progress visible in Workspace.`}</p><div className="release-log">{releaseStages.slice(0, completed ? releaseStages.length : job.stageIndex).map((stage, index) => <div key={stage.label}><time>{String(index * 2).padStart(2, '0')}s</time><b>{stage.label.toUpperCase()}</b><span>{stage.log}</span></div>)}{running && <div className="release-log-current" key={job.stageIndex}><time>{String(job.stageIndex * 2).padStart(2, '0')}s</time><b>{releaseStages[job.stageIndex].label.toUpperCase()}</b><span><i aria-hidden="true" />{releaseActivity(job)}</span></div>}{completed && <div className="release-log-success"><time>{String(elapsedSeconds).padStart(2, '0')}s</time><b>SUCCESS</b><span>Build completed · {job.path}</span></div>}{job.status === 'cancelled' && <div><time>—</time><b>CANCELLED</b><span>The mock build was cancelled.</span></div>}</div></>}
    </main>
    <footer className="release-footer">
      {job && <div className={`release-operation ${completed ? 'completed' : ''}`} role="status" aria-live="polite">{running ? <i aria-hidden="true" /> : <span aria-hidden="true">{completed ? '✓' : '×'}</span>}{releaseActivity(job)}</div>}
      <div className="release-actions">
        {!job && <><button type="button" disabled={tab === 'Setup'} onClick={back}>Back</button>{tab === 'Review' ? <button type="button" className="primary" onClick={start}>Start build &amp; verify</button> : <button type="button" className="primary" onClick={continueFlow}>{tab === 'Delivery' ? 'Review request' : 'Continue'}</button>}</>}
        {running && <><button type="button" className="danger" onClick={() => { if (window.confirm(`Cancel mock job #${job.id.toString().slice(-5)}?`)) onCancel(); }}>Request cancellation</button><button type="button" className="primary" onClick={onBackground}>Keep running in background</button></>}
        {job && !running && <><button type="button" onClick={() => { onNew(); setTab('Setup'); }}>Start another build</button><button type="button" className="primary" onClick={onClose}>Close</button></>}
      </div>
    </footer>
  </div>;
}
