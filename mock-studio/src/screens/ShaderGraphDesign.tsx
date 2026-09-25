import { useState } from 'react';
import { GraphCanvas } from '../design-system/GraphCanvas';
import { WorkspaceShell } from '../design-system/WorkspaceShell';
import data from '../fixtures/shaderGraph.json';
import './shaderGraphDesign.css';

export function ShaderGraphDesign() {
  const [tab, setTab] = useState(0);
  const [graph, setGraph] = useState(0);
  const [node, setNode] = useState(data.nodes[0].title);
  const [tool, setTool] = useState('Select');
  const [domain, setDomain] = useState('Surface');
  const [preview, setPreview] = useState('Lit + PBR');
  const [notice, setNotice] = useState('');
  const sampleAvailable = graph === 0;
  const current = data.graphs[graph];

  return <WorkspaceShell title={graph === 0 ? data.title : current.name} subtitle={data.subtitle} tabs={['Graph', 'Preview', 'Variants', 'Validation']} activeTab={tab} onTabChange={setTab}
    badges={<><span className="wb-badge">Graph valid</span><span className="wb-badge blue">12 variants</span></>}
    actions={<><button type="button" onClick={() => { setGraph(0); setTab(0); setNotice('Reference graph refreshed'); }}>Refresh</button><button type="button" className="primary" onClick={() => setNotice('Graph applied in mock')}>Apply</button></>}
    toolbar={<>{['Select', 'Pan', 'Zoom'].map(name => <button key={name} type="button" className={tool === name ? 'sg-tool-active' : ''} onClick={() => setTool(name)}>{name}</button>)}<label>Domain <select value={domain} onChange={event => setDomain(event.target.value)}><option>Surface</option><option>Decal</option><option>Post Process</option></select></label><label>Preview <select value={preview} onChange={event => setPreview(event.target.value)}><option>Lit + PBR</option><option>Unlit</option><option>Wireframe</option><option>Normals</option></select></label><button type="button" className="primary" onClick={() => setNotice(`Compiled ${current.name} in mock`)}>Compile</button></>}
    leftTitle="Shader Graphs" left={<div className="sg-graph-list">{data.graphs.map((item, index) => <button key={item.name} type="button" className={graph === index ? 'active' : ''} onClick={() => { setGraph(index); setNotice(index === 0 ? '' : `${item.name} has no authored node sample in this reference`); }}><small>{item.domain}</small><strong>{item.name}</strong><span>{item.summary}</span></button>)}</div>}
    rightTitle="Inspector" right={<><section className="sg-inspector"><h3>{sampleAvailable ? node : current.name}</h3>{sampleAvailable ? data.inspector.map(row => <div key={row.label}><span>{row.label}</span><strong>{row.value}</strong></div>) : <p>No property sample is defined for this graph.</p>}</section></>}
    status={notice ? <span className="sg-notice">{notice}</span> : data.status.map(item => <span key={item}>{item}</span>)}>
    {!sampleAvailable ? <div className="sg-empty">Only M_Rock_Master has authored node sample data in this mock.</div> : tab === 0 ? <GraphCanvas nodes={data.nodes} edges={data.edges} selected={node} onSelect={setNode} /> : tab === 1 ? <section className="sg-preview"><h2>Material Preview</h2><div className="sg-preview-shape" /><p>{preview} · PBR Surface</p><p>Preview renders with mesh sphere, cylinder, and plane under HDR environment light.</p></section> : tab === 2 ? <div className="sg-variants">{data.variants.map(variant => <section key={variant.title}><h2>{variant.title}<span>{variant.tier}</span></h2><p>{variant.summary}</p></section>)}</div> : <section className="sg-validation"><h2>Domain Validation <span>pass</span></h2>{data.validation.map(row => <div key={row.label}><span>{row.label}</span><strong>{row.value}</strong></div>)}</section>}
  </WorkspaceShell>;
}
