import { useState } from 'react';
import { GraphCanvas } from '../design-system/GraphCanvas';
import { WorkspaceShell } from '../design-system/WorkspaceShell';
import data from '../fixtures/pcgWorkbench.json';
import './pcgWorkbenchDesign.css';

const tabs = ['Graph', 'Preview', 'Budgets', 'Validation', 'Runtime Handoff'];
type TableData = { headers: string[]; rows: string[][] };
type ViewData = { metrics: { title: string; value: string; summary: string }[]; problems: { title: string; summary: string; status: string }[]; tables: TableData[]; flow: { title: string; summary: string }[] };
const views = data.views as Record<string, ViewData>;

function DataTable({ table }: { table: TableData }) {
  return <div className="pcg-table-wrap"><table><thead><tr>{table.headers.map((heading, index) => <th scope="col" key={`${heading}-${index}`}>{heading}</th>)}</tr></thead><tbody>{table.rows.map((row, index) => <tr key={index}>{row.map((cell, column) => <td key={column}>{cell}</td>)}</tr>)}</tbody></table></div>;
}

export function PcgWorkbenchDesign() {
  const [tab, setTab] = useState(0);
  const [selectedNode, setSelectedNode] = useState(data.nodes[0].title);
  const [selectedSide, setSelectedSide] = useState('Forest_Procedural');
  const [mode, setMode] = useState('Mode: Offline Bake');
  const [preview, setPreview] = useState('Point Cloud + Instances');
  const [seed, setSeed] = useState('41832');
  const [notice, setNotice] = useState('');
  const view = views[['', 'preview', 'budgets', 'validation', 'runtime'][tab]];

  return <WorkspaceShell title={data.title} subtitle={data.subtitle} tabs={tabs} activeTab={tab} onTabChange={setTab}
    badges={<><span className="wb-badge">DAG valid</span><span className="wb-badge blue">seed {seed}</span><span className="wb-badge warning">2 warnings</span></>}
    actions={<><select aria-label="Generation mode" value={mode} onChange={event => setMode(event.target.value)}><option>Mode: Offline Bake</option><option>Mode: Hybrid Preview</option><option>Mode: Runtime Prototype</option></select><button type="button" onClick={() => { setSeed('41832'); setMode('Mode: Offline Bake'); setNotice('Mock values reverted'); }}>Revert</button><button type="button" className="primary" onClick={() => setNotice('Graph saved in mock')}>Save Graph</button></>}
    toolbar={<><button type="button" onClick={() => setNotice('Select tool active')}>Select</button><button type="button" onClick={() => setNotice('Pan tool active')}>Pan</button><button type="button" onClick={() => setNotice('Connect tool active')}>Connect</button><button type="button" onClick={() => setNotice('Node palette is shown at left')}>Add Node</button><label>Seed <input aria-label="PCG seed" value={seed} onChange={event => setSeed(event.target.value)} size={7} /></label><label>Preview <select value={preview} onChange={event => setPreview(event.target.value)}><option>Point Cloud + Instances</option><option>Point Cloud Only</option><option>Density Heatmap</option><option>Exclusion Masks</option></select></label><button type="button" onClick={() => { setTab(1); setNotice('Preview regenerated in mock'); }}>Regenerate Preview</button><button type="button" onClick={() => setTab(3)}>Validate Graph</button><button type="button" className="primary" onClick={() => setNotice('Bake To Scene is a mock action')}>Bake To Scene</button></>}
    leftTitle="PCG Assets" left={<div className="pcg-side">{data.sidebar.map((entry, index) => entry.type === 'heading' ? <h3 key={index}>{entry.label}</h3> : <button key={index} type="button" className={selectedSide === entry.label ? 'active' : ''} onClick={() => { setSelectedSide(entry.label); setNotice(`${entry.label} selected`); }}><span>{entry.label}</span><small>{entry.meta}</small></button>)}</div>}
    rightTitle="Inspector" right={<section className="pcg-inspector"><h3>{selectedNode}</h3>{data.inspector.map(item => <div key={item.label}><span>{item.label}</span><strong>{item.value}</strong></div>)}<p>Generated objects are committed only through Bake To Scene.</p></section>}
    status={notice ? <span className="pcg-notice">{notice}</span> : data.status.map(item => <span key={item}>{item}</span>)}>
    {tab === 0 ? <GraphCanvas nodes={data.nodes} edges={data.edges} selected={selectedNode} onSelect={setSelectedNode} width={1000} height={600} /> : <div className="pcg-data-view">
      {tab === 1 && <div className="pcg-terrain" aria-label="Density heatmap preview"><div className="pcg-terrain-cluster one" /><div className="pcg-terrain-cluster two" /><div className="pcg-terrain-cluster three" /><div className="pcg-terrain-road" /><span>{preview} · Forest_North_03</span></div>}
      {view?.metrics.length > 0 && <div className="pcg-metrics">{view.metrics.map(item => <section key={item.title}><h2>{item.title}</h2><strong>{item.value}</strong><p>{item.summary}</p></section>)}</div>}
      {view?.problems.length > 0 && <section className="pcg-problems"><h2>{tabs[tab]}</h2>{view.problems.map(item => <div key={item.title}><div><strong>{item.title}</strong><p>{item.summary}</p></div><span>{item.status}</span></div>)}</section>}
      {view?.flow.length > 0 && <div className="pcg-flow">{view.flow.map(item => <section key={item.title}><h2>{item.title}</h2><p>{item.summary}</p></section>)}</div>}
      {view?.tables.map((table, index) => <DataTable key={index} table={table} />)}
    </div>}
  </WorkspaceShell>;
}
