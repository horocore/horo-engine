import { useEffect, useMemo, useState } from 'react';
import catalogData from './catalog.json';
import { NativeScreen, workspaceId } from './screens/registry';
import type { ReleaseJob, ReleaseRequest } from './releaseJob';

type Design = { id: string; title: string; group: string };
const catalog = catalogData as Design[];
const initialId = () => decodeURIComponent(new URLSearchParams(location.hash.slice(1)).get('design') || workspaceId);

type ReleaseServices = {
  releaseJob: ReleaseJob | null;
  startRelease: (request: ReleaseRequest) => void;
  backgroundRelease: () => void;
  cancelRelease: () => void;
  newRelease: () => void;
};

function Canvas({ design, label, openDesign, navigate, releaseServices }: { design: Design; label: string; openDesign: (id: string) => void; navigate: (id: string) => void; releaseServices: ReleaseServices }) {
  return <section className="canvas">
    <div className="canvas-heading"><span>{label}</span><strong>{design.title}</strong></div>
    <NativeScreen key={design.id} designId={design.id} openDesign={openDesign} navigate={navigate} {...releaseServices} />
  </section>;
}

export function App() {
  const [selected, setSelected] = useState(initialId);
  const [compare, setCompare] = useState<string | null>(null);
  const [filter, setFilter] = useState('');
  const [modal, setModal] = useState<string | null>(null);
  const [releaseJob, setReleaseJob] = useState<ReleaseJob | null>(null);
  const [releaseToast, setReleaseToast] = useState(false);
  const active = catalog.find(item => item.id === selected) || catalog[0];
  const compared = catalog.find(item => item.id === compare);
  const modalDesign = catalog.find(item => item.id === modal);
  const visible = useMemo(() => catalog.filter(item => `${item.title} ${item.group}`.toLowerCase().includes(filter.toLowerCase())), [filter]);

  useEffect(() => {
    const onHashChange = () => setSelected(initialId());
    window.addEventListener('hashchange', onHashChange);
    return () => window.removeEventListener('hashchange', onHashChange);
  }, []);

  useEffect(() => {
    if (releaseJob?.status !== 'running') return;
    const timer = window.setInterval(() => setReleaseJob(current => {
      if (!current || current.status !== 'running') return current;
      if (current.stageIndex === 8) return { ...current, status: 'success', finishedAt: Date.now() };
      return { ...current, stageIndex: current.stageIndex + 1 };
    }), 2400);
    return () => window.clearInterval(timer);
  }, [releaseJob?.status]);

  useEffect(() => {
    if (releaseJob?.status === 'success' && releaseJob.background) setReleaseToast(true);
  }, [releaseJob?.status, releaseJob?.background]);

  function open(id: string) {
    location.hash = `design=${encodeURIComponent(id)}`;
    setSelected(id);
  }

  function startRelease(request: ReleaseRequest) {
    setReleaseToast(false);
    const startedAt = Date.now();
    setReleaseJob({ ...request, id: startedAt, stageIndex: 0, status: 'running', background: false, startedAt });
  }

  function backgroundRelease() {
    setReleaseJob(current => current?.status === 'running' ? { ...current, background: true } : current);
    setModal(null);
    open(workspaceId);
  }

  function closeModal() {
    if (modal === 'architecture/release/release-modal-design.html' && releaseJob?.status === 'running') {
      backgroundRelease();
      return;
    }
    setModal(null);
  }

  const releaseServices: ReleaseServices = {
    releaseJob,
    startRelease,
    backgroundRelease,
    cancelRelease: () => setReleaseJob(current => current?.status === 'running' ? { ...current, status: 'cancelled', finishedAt: Date.now() } : current),
    newRelease: () => { setReleaseJob(null); setReleaseToast(false); },
  };

  return <div className="studio">
    <aside className="sidebar">
      <header className="brand"><span className="brand-mark">H</span><div><strong>Horo Mock Studio</strong><small>Independent design workspace</small></div></header>
      <label className="search-label" htmlFor="design-search">Find a design</label>
      <input id="design-search" value={filter} onChange={event => setFilter(event.target.value)} placeholder={`Search ${catalog.length} designs…`} />
      <nav aria-label="Mock designs">
        {visible.map((design, index) => <div key={design.id}>
          {(index === 0 || visible[index - 1].group !== design.group) && <h2>{design.group}</h2>}
          <button type="button" className={active.id === design.id ? 'design-link selected' : 'design-link'} onClick={() => open(design.id)}>{design.title}</button>
        </div>)}
      </nav>
      <footer>{catalog.length} designs · React studio</footer>
    </aside>
    <main className="main">
      <header className="toolbar">
        <div><small>MOCK DESIGN</small><h1>{active.title}</h1></div>
        <div className="toolbar-actions">
          <select aria-label="Compare with" value={compare || ''} onChange={event => setCompare(event.target.value || null)}>
            <option value="">Compare with…</option>
            {catalog.filter(item => item.id !== active.id).map(item => <option key={item.id} value={item.id}>{item.title}</option>)}
          </select>
          <button type="button" onClick={() => setModal(active.id)}>Open as modal</button>
        </div>
      </header>
      <div className={compared ? 'canvases comparing' : 'canvases'}>
        <Canvas design={active} label="Primary" openDesign={setModal} navigate={open} releaseServices={releaseServices} />
        {compared && <Canvas design={compared} label="Comparison" openDesign={setModal} navigate={open} releaseServices={releaseServices} />}
      </div>
    </main>
    {releaseToast && <div className="studio-snackbar" role="status"><span className="studio-snackbar-icon">✓</span><span><strong>Build completed</strong><small>{releaseJob?.path}</small></span><button type="button" onClick={() => { setReleaseToast(false); setModal('architecture/release/release-modal-design.html'); }}>View logs</button><button type="button" aria-label="Dismiss notification" onClick={() => setReleaseToast(false)}>×</button></div>}
    {modalDesign && <div className="overlay" role="presentation" onMouseDown={event => { if (event.target === event.currentTarget) closeModal(); }}>
      <section className="dialog" role="dialog" aria-modal="true" aria-label={modalDesign.title}>
        <header><strong>{modalDesign.title}</strong><button type="button" aria-label="Close modal" onClick={closeModal}>×</button></header>
        <NativeScreen key={modalDesign.id} designId={modalDesign.id} openDesign={setModal} navigate={open} closeModal={closeModal} {...releaseServices} />
      </section>
    </div>}
  </div>;
}
