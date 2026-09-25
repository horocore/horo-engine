import { useState } from 'react';
import fixtures from '../fixtures/threeColumnPanels.json';
import './threeColumnPanel.css';

type Tone = 'success' | 'warning' | 'error' | null;
type Row = { label: string; value: string; tone: Tone };
type Card = { title: string; rows: Row[]; headline?: string; summary?: string };
type Block = { type: 'card'; card: Card } | { type: 'grid'; cards: Card[] } | { type: 'preview'; kind: string };
type ColumnItem = { type: 'heading'; text: string } | { type: 'row'; label: string; value: string; tone: Tone } | { type: 'item'; title: string; subtitle: string };
type Panel = { title: string; subtitle: string; context: string; actions: string[]; tabs: { title: string; blocks: Block[] }[]; sidebar: ColumnItem[]; details: ColumnItem[]; footer: string[] };
const panels = fixtures as Record<string, Panel>;

export const threeColumnIds = Object.keys(panels).map(name => `architecture/runtime/${name}.html`);

function Value({ value, tone }: { value: string; tone: Tone }) {
  return tone ? <span className={`tp-pill ${tone}`}>{value}</span> : <span>{value}</span>;
}

function KeyValue({ label, value, tone }: Row) {
  return <div className="tp-kv"><span>{label}</span><Value value={value} tone={tone} /></div>;
}

function CardView({ card }: { card: Card }) {
  return <section className="tp-card"><h3>{card.title}</h3>{card.headline && <strong className="tp-card-headline">{card.headline}</strong>}{card.summary && <p className="tp-card-summary">{card.summary}</p>}{card.rows.map((row, index) => <KeyValue key={`${row.label}-${index}`} {...row} />)}</section>;
}

function Preview({ kind }: { kind: string }) {
  if (kind === 'xr-setup') return <div className="tp-preview xr" aria-label="XR view configuration preview"><div>RUNTIME VIEW SET<i /></div><div>PER-VIEW STATUS<i /></div></div>;
  if (kind === 'lod-debugger' || kind === 'navigation-bake') return <div className="tp-preview flow" aria-label={`${kind} preview`}><div>{kind === 'lod-debugger' ? 'LOD0' : 'Source geometry'}</div><div>{kind === 'lod-debugger' ? 'LOD1' : 'Bake tiles'}</div><div>{kind === 'lod-debugger' ? 'LOD2 / Impostor' : 'NavMesh ready'}</div></div>;
  if (kind === 'destruction-setup') return <div className="tp-preview fracture" aria-label="Fracture support preview">{Array.from({ length: 6 }, (_, index) => <i key={index} />)}</div>;
  if (kind === 'decal-placement') return <div className="tp-preview decal" aria-label="Decal projection preview"><i /></div>;
  return <div className="tp-preview primitive" aria-label="Primitive placement preview"><i /></div>;
}

function Column({ items, selectable, selected, onSelect }: { items: ColumnItem[]; selectable?: boolean; selected?: string; onSelect?: (value: string) => void }) {
  return <>{items.map((item, index) => {
    if (item.type === 'heading') return <h2 key={index}>{item.text}</h2>;
    if (item.type === 'row') return <KeyValue key={index} {...item} />;
    return <button key={index} type="button" className={selectable && selected === item.title ? 'tp-item selected' : 'tp-item'} onClick={() => onSelect?.(item.title)}><strong>{item.title}</strong>{item.subtitle && <small>{item.subtitle}</small>}</button>;
  })}</>;
}

export function ThreeColumnPanel({ designId }: { designId: string }) {
  const key = designId.split('/').at(-1)?.replace(/\.html$/, '') || '';
  const panel = panels[key];
  const [tabIndex, setTabIndex] = useState(0);
  const firstItem = panel?.sidebar.find(item => item.type === 'item');
  const [selected, setSelected] = useState(firstItem?.type === 'item' ? firstItem.title : '');
  const [message, setMessage] = useState('');
  const [frozen, setFrozen] = useState(false);
  if (!panel) return <div className="tp-missing">Unknown panel: {designId}</div>;

  function act(label: string) {
    if (label === 'Reset') { setTabIndex(0); setSelected(firstItem?.type === 'item' ? firstItem.title : ''); setMessage('Selection reset'); return; }
    if (label === 'Freeze') { setFrozen(!frozen); setMessage(frozen ? 'Live updates resumed' : 'View frozen'); return; }
    setMessage(`${label} preview requested for ${selected || panel.title}`);
  }

  return <div className="three-column-panel">
    <header className="tp-header"><div><h1>{panel.title}</h1><div className="tp-meta"><span>{panel.subtitle}</span>{panel.context && <small>{panel.context}</small>}</div></div><div className="tp-actions">{panel.actions.map((label, index) => <button key={label} type="button" className={index === panel.actions.length - 1 ? 'primary' : ''} onClick={() => act(label)}>{label === 'Freeze' && frozen ? 'Resume' : label}</button>)}</div></header>
    <nav className="tp-tabs" aria-label={`${panel.title} views`}>{panel.tabs.map((tab, index) => <button key={tab.title} type="button" aria-current={tabIndex === index ? 'page' : undefined} className={tabIndex === index ? 'active' : ''} onClick={() => setTabIndex(index)}>{tab.title}</button>)}</nav>
    <div className="tp-layout"><aside className="tp-side"><Column items={panel.sidebar} selectable selected={selected} onSelect={setSelected} /></aside><main className="tp-main">{panel.tabs[tabIndex].blocks.map((block, index) => block.type === 'grid' ? <div className="tp-grid" key={index}>{block.cards.map(card => <CardView key={card.title} card={card} />)}</div> : block.type === 'card' ? <CardView key={index} card={block.card} /> : <Preview key={index} kind={block.kind} />)}</main><aside className="tp-detail"><Column items={panel.details} /></aside></div>
    <footer className="tp-status">{message ? <span className="tp-message">{message}</span> : panel.footer.map((part, index) => <span key={index}>{part}</span>)}</footer>
  </div>;
}
