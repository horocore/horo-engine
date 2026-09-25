import { useState } from 'react';
import { SelectField, TextField } from '../design-system/Controls';
import { releaseActivity, releaseStages, type ReleaseJob, type ReleaseRequest } from '../releaseJob';
import './release.css';

export function ReleaseDesign({ job, onStart, onBackground, onCancel, onNew, onClose }: { job: ReleaseJob | null; onStart: (request: ReleaseRequest) => void; onBackground: () => void; onCancel: () => void; onNew: () => void; onClose: () => void }) {
  const [review, setReview] = useState(false);
  const [profile, setProfile] = useState('Game Runtime');
  const [platform, setPlatform] = useState('Windows');
  const [architecture, setArchitecture] = useState('x86_64');
  const [configuration, setConfiguration] = useState('Shipping');
  const [version, setVersion] = useState('0.4.2');
  const [releaseName, setReleaseName] = useState('DesertRun');
  const [sourceRevision, setSourceRevision] = useState('v0.4.2');
  const [notes, setNotes] = useState('docs/releases/0.4.2.md');
  const [content, setContent] = useState('All game content');
  const [packageFormat, setPackageFormat] = useState('Portable archive');
  const [testProfile, setTestProfile] = useState('Release smoke only');
  const [signingProfile, setSigningProfile] = useState('Production signing profile');
  const [protectionProfile, setProtectionProfile] = useState('Project default');
  const [outputRoot, setOutputRoot] = useState('releases');
  const [toolchain, setToolchain] = useState('Default toolchain');
  const [jobs, setJobs] = useState('16');

  const target = job?.target ?? `${platform} · ${architecture} · ${configuration}`;
  const outputPath = job?.path ?? `${outputRoot.replace(/[/\\]+$/, '')}/${version}_${platform.toLowerCase()}_${architecture}_${configuration.toLowerCase()}/`;
  const running = job?.status === 'running';
  const completed = job?.status === 'success';
  const elapsedSeconds = job ? Math.floor(((running ? Date.now() : job.finishedAt ?? job.startedAt + releaseStages.length * 2400) - job.startedAt) / 1000) : 0;
  const request: ReleaseRequest = { target, path: outputPath, name: `${releaseName} v${version}`, version, profile, sourceRevision, notes, content, testProfile, signingProfile, packageFormat };

  function start() {
    if (!/^\d+\.\d+\.\d+(?:-[\da-z.-]+)?(?:\+[\da-z.-]+)?$/i.test(version) || !releaseName.trim() || !sourceRevision.trim() || !notes.trim() || !outputRoot.trim() || !Number.isInteger(Number(jobs)) || Number(jobs) < 1) {
      window.alert('Enter a release name, semantic version, source revision, release notes, output root, and positive parallel job count.');
      return;
    }
    setReview(false);
    onStart(request);
  }

  const reviewGroups = [
    { title: 'Release identity', fields: [['Product', request.name], ['Version', version], ['Source', sourceRevision], ['Release notes', notes]] },
    { title: 'Target and package', fields: [['Profile', profile], ['Target', target], ['Package', packageFormat], ['Content', content]] },
    { title: 'Verification and security', fields: [['Tests', testProfile], ['Signing', signingProfile], ['Archive protection', protectionProfile]] },
    { title: 'Local output', fields: [['Output root', outputRoot], ['Candidate path', outputPath], ['Result', 'Verified local candidate only']] },
  ];

  return <div className={`release-design ${job ? 'release-active' : 'release-draft'} ${!job && review ? 'release-review-mode' : ''}`}>
    <header className="release-header"><strong>Prepare Release</strong></header>
    {job && <div className="release-stages" style={{ gridTemplateColumns: `repeat(${releaseStages.length}, minmax(85px, 1fr))` }}>{releaseStages.map((stage, index) => <div key={stage.label} className={completed || index < job.stageIndex ? 'done' : index === job.stageIndex && running ? 'current' : ''}><i /><span><em>{String(index + 1).padStart(2, '0')}</em> {stage.label}</span></div>)}</div>}
    <div className="release-summary"><span className={`release-pill ${completed ? 'success' : ''}`}><i />{completed ? 'FINAL VERIFIED' : running ? releaseStages[job.stageIndex].label.toUpperCase() : job?.status === 'cancelled' ? 'CANCELLED' : 'DRAFT'}</span><div><small>Target</small><strong>{target}</strong></div>{job && <div><small>Elapsed</small><strong>{String(Math.floor(elapsedSeconds / 60)).padStart(2, '0')}:{String(elapsedSeconds % 60).padStart(2, '0')}</strong></div>}<div className="release-candidate"><small>Local candidate path</small><strong>{outputPath}</strong></div></div>
    <main className="release-content">{!job && !review && <div className="release-quick-form release-long-form"><h2>Identity</h2><div className="release-row"><TextField label="Product name" value={releaseName} onChange={event => setReleaseName(event.target.value)} /><TextField label="Version (SemVer)" value={version} onChange={event => setVersion(event.target.value)} /></div><div className="release-row"><TextField label="Source revision or tag" value={sourceRevision} onChange={event => setSourceRevision(event.target.value)} /><TextField label="Release notes (project-relative)" value={notes} onChange={event => setNotes(event.target.value)} /></div>
      <h2>Target and content</h2><SelectField label="Product release profile" value={profile} onChange={event => setProfile(event.target.value)}><option>Game Runtime</option><option>Game Dedicated Server</option><option>Engine Editor</option><option>Engine CLI</option><option>SDK</option><option>Developer Diagnostics</option></SelectField><div className="release-row"><SelectField label="Platform" value={platform} onChange={event => setPlatform(event.target.value)}><option>Windows</option><option>macOS</option><option>Linux</option></SelectField><SelectField label="Architecture" value={architecture} onChange={event => setArchitecture(event.target.value)}><option>x86_64</option><option>arm64</option></SelectField></div><div className="release-row"><SelectField label="Configuration" value={configuration} onChange={event => setConfiguration(event.target.value)}><option>Shipping</option><option>Development</option><option>Test</option></SelectField><SelectField label="Package format" value={packageFormat} onChange={event => setPackageFormat(event.target.value)}><option>Portable archive</option><option>Platform installer</option><option>Store package</option></SelectField></div><SelectField label="Content selection" value={content} onChange={event => setContent(event.target.value)}><option>All game content</option><option>Selected scenes and dependencies</option><option>Base game and required chunks</option></SelectField>
      <h2>Quality gates</h2><SelectField label="Project test profile" hint="Packaged-player smoke and final verification are always required." value={testProfile} onChange={event => setTestProfile(event.target.value)}><option>Release smoke only</option><option>Fast + release smoke</option><option>Full project tests + release smoke</option></SelectField><p className="release-notice">Version/tag/notes, frozen package lock, licenses, compatibility, signatures, checksums, and packaged-player startup are checked before the candidate becomes eligible to publish.</p>
      <h2>Security and output</h2><div className="release-row"><SelectField label="Signing profile reference" value={signingProfile} onChange={event => setSigningProfile(event.target.value)}><option>Production signing profile</option><option>Test signing profile</option></SelectField><SelectField label="Archive protection profile" value={protectionProfile} onChange={event => setProtectionProfile(event.target.value)}><option>Project default</option><option>Protected archive</option></SelectField></div><TextField label="Output root" value={outputRoot} onChange={event => setOutputRoot(event.target.value)} /><details className="release-advanced"><summary>Advanced build options <span>Toolchain and parallel jobs</span></summary><div className="release-row"><SelectField label="Toolchain" value={toolchain} onChange={event => setToolchain(event.target.value)}><option>Default toolchain</option><option>MSVC 2022</option><option>Clang 17</option></SelectField><TextField label="Parallel jobs" type="number" min="1" value={jobs} onChange={event => setJobs(event.target.value)} /></div></details><p className="release-notice">This action prepares a local candidate only. Publishing requires opening Publish Candidate separately.</p></div>}
      {!job && review && <div className="release-review-layout"><div className="release-review-heading"><h2>Review release request</h2><p>Confirm the source, target, and gates before preparing a local candidate.</p></div><div className="release-review-table-scroll" role="region" aria-label="Release request details" tabIndex={0}><table className="release-review-table">{reviewGroups.map(group => <tbody key={group.title}><tr className="release-review-section"><th colSpan={2} scope="colgroup">{group.title}</th></tr>{group.fields.map(([label, value]) => <tr key={label}><th scope="row">{label}</th><td>{value}</td></tr>)}</tbody>)}</table></div><p className="release-notice">The candidate must pass all required checks. This mock does not build files, sign artifacts, or publish to a destination.</p></div>}
      {job && <><p className={`release-notice ${completed ? 'release-notice-success' : ''}`}>{completed ? `Mock release candidate ${job.name} passed final verification and packaged-player smoke. Local candidate: ${job.path}. No files were written or published.` : `Mock release job #${String(job.id).slice(-5)} ${running ? 'is running' : 'was cancelled'}. Closing this modal keeps its progress in Workspace.`}</p><div className="release-log">{releaseStages.slice(0, completed ? releaseStages.length : job.stageIndex).map((stage, index) => <div key={stage.label}><time>{String(Math.round(index * 2.4)).padStart(2, '0')}s</time><b>{stage.label.toUpperCase()}</b><span>{stage.log}</span></div>)}{running && <div className="release-log-current"><time>{String(Math.round(job.stageIndex * 2.4)).padStart(2, '0')}s</time><b>{releaseStages[job.stageIndex].label.toUpperCase()}</b><span><i aria-hidden="true" />{releaseActivity(job)}</span></div>}{completed && <div className="release-log-success"><time>{String(elapsedSeconds).padStart(2, '0')}s</time><b>READY</b><span>Local release candidate final verified · {job.path}</span></div>}{job.status === 'cancelled' && <div><time>—</time><b>CANCELLED</b><span>Release preparation was cancelled.</span></div>}</div></>}
    </main><footer className="release-footer">{job && <div className={`release-operation ${completed ? 'completed' : ''}`} role="status" aria-live="polite">{running ? <i aria-hidden="true" /> : <span aria-hidden="true">{completed ? '✓' : '×'}</span>}{releaseActivity(job)}</div>}<div className="release-actions">{!job && (review ? <><button type="button" onClick={() => setReview(false)}>Back</button><button type="button" className="primary" onClick={start}>Prepare local candidate</button></> : <button type="button" className="primary" onClick={() => setReview(true)}>Review request</button>)}{running && <><button type="button" className="danger" onClick={onCancel}>Cancel job</button><button type="button" className="primary" onClick={onBackground}>Keep running in background</button></>}{job && !running && <><button type="button" onClick={() => { onNew(); setReview(false); }}>Prepare another release</button><button type="button" className="primary" onClick={onClose}>Close</button></>}</div></footer>
  </div>;
}
