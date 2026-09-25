import { useState } from 'react';
import { SelectField, TextField } from '../design-system/Controls';
import type { ReleaseJob } from '../releaseJob';
import { publishStages, type WorkflowJob, type WorkflowRequest } from '../workflowJob';
import { WorkflowActivity } from './WorkflowActivity';
import './release.css';

export function PublishDesign({ candidates, job, onStart, onBackground, onCancel, onClose }: { candidates: ReleaseJob[]; job: WorkflowJob | null; onStart: (request: WorkflowRequest) => void; onBackground: () => void; onCancel: () => void; onClose: () => void }) {
  const [newDraft, setNewDraft] = useState(false);
  const [review, setReview] = useState(false);
  const [selectedCandidate, setSelectedCandidate] = useState<number | null>(null);
  const [channel, setChannel] = useState('Preview');
  const [destination, setDestination] = useState('Project CDN');
  const [visibility, setVisibility] = useState('Private');
  const [credential, setCredential] = useState('Project publishing profile');
  const [notes, setNotes] = useState('docs/releases/0.4.2.md');
  const candidate = candidates.find(item => item.id === selectedCandidate) ?? candidates.at(-1);
  const eligible = Boolean(candidate);

  if (job && !newDraft) return <WorkflowActivity job={job} onBackground={onBackground} onCancel={onCancel} onNew={() => { setNewDraft(true); setReview(false); }} onClose={onClose} />;

  function publish() {
    if (!eligible || !candidate || !notes.trim()) return;
    setNewDraft(false);
    onStart({ kind: 'publish', title: 'Publish candidate', target: `${candidate.name} · ${candidate.target}`, output: `${destination} · ${channel} · ${visibility}`, stages: publishStages });
  }

  return <div className="release-design release-draft">
    <header className="release-header"><strong>Publish Candidate</strong></header>
    <div className="release-summary"><span className={`release-pill ${eligible ? 'success' : ''}`}><i />{eligible ? 'FINAL VERIFIED' : 'NO ELIGIBLE CANDIDATE'}</span>{candidate && <><div><small>Version</small><strong>{candidate.version}</strong></div><div className="release-candidate"><small>Candidate</small><strong>{candidate.path}</strong></div></>}</div>
    <main className="release-content"><div className="release-quick-form">
      {!candidate ? <div className="release-empty"><h2>No verified candidate</h2><p>Prepare Release must finish all required stages, including packaged-player smoke verification, before publication becomes available.</p><p>A failed or cancelled job cannot be published. Publishing never starts a build automatically.</p></div> : !review ? <>
        <h2>Candidate and destination</h2>
        <SelectField label="Verified local candidate" value={candidate.id} onChange={event => { setSelectedCandidate(Number(event.target.value)); setReview(false); }}>{candidates.map(item => <option key={item.id} value={item.id}>{item.name} · {item.target} · {item.path}</option>)}</SelectField>
        <div className="release-review release-form-gap"><div><span>Product</span><strong>{candidate.name}</strong></div><div><span>Target</span><strong>{candidate.target}</strong></div><div><span>Verification</span><strong>Final verification and packaged smoke passed</strong></div></div>
        <div className="release-row release-form-gap"><SelectField label="Channel" value={channel} onChange={event => setChannel(event.target.value)}><option>Preview</option><option>Stable</option><option>Nightly</option></SelectField><SelectField label="Destination" value={destination} onChange={event => setDestination(event.target.value)}><option>Project CDN</option><option>Steam</option><option>itch.io</option><option>Custom storage</option></SelectField></div>
        <div className="release-row"><SelectField label="Visibility" value={visibility} onChange={event => setVisibility(event.target.value)}><option>Private</option><option>Public</option></SelectField><SelectField label="Publishing credentials" hint="A configured profile reference; no secret is stored in this mock." value={credential} onChange={event => setCredential(event.target.value)}><option>Project publishing profile</option><option>CI publishing profile</option></SelectField></div>
        <TextField label="Release notes" value={notes} onChange={event => setNotes(event.target.value)} />
        <p className="release-notice">Publication is a separate action. This screen will not rebuild or change candidate bytes.</p>
      </> : <>
        <h2>Confirm publication</h2>
        <div className="release-review">{[['Candidate', candidate.path], ['Version', candidate.version], ['Target', candidate.target], ['Channel', channel], ['Destination', destination], ['Visibility', visibility], ['Credential profile', credential], ['Release notes', notes]].map(([label, value]) => <div key={label}><span>{label}</span><strong>{value}</strong></div>)}</div>
        <p className="release-notice release-form-gap">Publish sends this exact verified candidate to the selected destination. This interactive mock will only show a sample result; it will not upload files.</p>
      </>}
    </div></main>
    <footer className="release-footer"><div className="release-actions">{eligible && (review ? <><button type="button" onClick={() => setReview(false)}>Back</button><button type="button" className="primary" onClick={publish}>Publish candidate</button></> : <button type="button" className="primary" onClick={() => setReview(true)}>Review publication</button>)}</div></footer>
  </div>;
}
