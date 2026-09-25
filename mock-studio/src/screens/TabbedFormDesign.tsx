import { useState } from 'react';
import characterSetup from '../fixtures/characterSetup.json';
import physicsDebugger from '../fixtures/physicsDebugger.json';
import './tabbedFormDesign.css';

type Field = { label: string; kind: 'input' | 'select' | 'display'; value: string; options?: string[] };
type FormDesign = { title: string; subtitle: string; actions?: string[]; sidebarTitle?: string; sidebar: { label: string; value: string }[]; collisionLayers?: { labels: string[]; rows: { label: string; enabled: boolean[] }[] }; tabs: { title: string; cards: { title: string; fields: Field[] }[] }[]; status: string[] };
const designs: Record<string, FormDesign> = {
  'architecture/runtime/character-setup.html': characterSetup as FormDesign,
  'architecture/runtime/physics-debugger.html': physicsDebugger as FormDesign,
};
export const tabbedFormIds = Object.keys(designs);

export function TabbedFormDesign({ designId }: { designId: string }) {
  const design = designs[designId];
  const [tabIndex, setTabIndex] = useState(0);
  const [edits, setEdits] = useState<Record<string, string>>({});
  const [notice, setNotice] = useState('');
  if (!design) return null;

  return <div className="tabbed-form-design">
    <header className="tfd-header"><div><h1>{design.title}</h1><p>{design.subtitle}</p></div>{design.actions && <div className="tfd-actions"><button type="button" onClick={() => { setEdits({}); setNotice('Changes reverted'); }}>Revert</button><button type="button" className="primary" onClick={() => setNotice('Configuration applied in mock')}>Apply</button></div>}</header>
    <nav className="tfd-tabs" aria-label={`${design.title} sections`}>{design.tabs.map((tab, index) => <button key={tab.title} type="button" className={index === tabIndex ? 'active' : ''} aria-current={index === tabIndex ? 'page' : undefined} onClick={() => setTabIndex(index)}>{tab.title}</button>)}</nav>
    <div className="tfd-layout"><aside className="tfd-side">{design.collisionLayers && <><h2>Collision Layers</h2><table className="tfd-matrix"><thead><tr><th scope="col" /><>{design.collisionLayers.labels.map(label => <th key={label} scope="col">{label}</th>)}</></tr></thead><tbody>{design.collisionLayers.rows.map(row => <tr key={row.label}><th scope="row">{row.label}</th>{row.enabled.map((enabled, index) => <td key={index} className={enabled ? 'enabled' : ''}>{enabled ? '✓' : ''}</td>)}</tr>)}</tbody></table></>}
      <h2>{design.sidebarTitle || 'Characters'}</h2>{design.sidebar.map((item, index) => <div key={item.label} className={!design.collisionLayers && index === 0 ? 'selected' : ''}><span>{item.label}</span><small>{item.value}</small></div>)}</aside>
      <main className="tfd-main"><div className="tfd-grid">{design.tabs[tabIndex].cards.map((card, cardIndex) => <section key={card.title} className="tfd-card"><h2>{card.title}</h2>{card.fields.map((field, fieldIndex) => {
        const key = `${tabIndex}:${cardIndex}:${fieldIndex}`;
        const value = edits[key] ?? field.value;
        return <label key={key} className="tfd-field"><span>{field.label}</span>{field.kind === 'input' ? <input value={value} onChange={event => setEdits({ ...edits, [key]: event.target.value })} /> : field.kind === 'select' ? <select value={value} onChange={event => setEdits({ ...edits, [key]: event.target.value })}>{field.options?.map(option => <option key={option}>{option}</option>)}</select> : <output>{value}</output>}</label>;
      })}</section>)}</div></main></div>
    <footer className="tfd-status">{notice ? <span className="notice">{notice}</span> : design.status.map(part => <span key={part}>{part}</span>)}</footer>
  </div>;
}
