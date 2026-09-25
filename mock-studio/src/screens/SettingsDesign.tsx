import { useState } from 'react';
import fixtures from '../fixtures/settingsPanels.json';
import './settingsDesign.css';

type Field = { label: string; kind: 'display' | 'select'; value: string; options?: string[] };
type Card = { title: string; fields: Field[]; tags?: string[]; matrix?: string[][] };
type Design = { title: string; subtitle: string; actions: string[]; topTabs: boolean; sidebar: string[]; tabs: { title: string; cards: Card[] }[]; status: string[] };
const designs = fixtures as Record<string, Design>;
export const settingsIds = Object.keys(designs);

export function SettingsDesign({ designId }: { designId: string }) {
  const design = designs[designId];
  const [tabIndex, setTabIndex] = useState(0);
  const [values, setValues] = useState<Record<string, string>>({});
  const [matrix, setMatrix] = useState<Record<string, string>>({});
  const [addedTags, setAddedTags] = useState<string[]>([]);
  const [tagDraft, setTagDraft] = useState('');
  const [addingTag, setAddingTag] = useState(false);
  const [notice, setNotice] = useState('');
  if (!design) return null;

  function revert() { setValues({}); setMatrix({}); setAddedTags([]); setTagDraft(''); setAddingTag(false); setNotice('Changes reverted'); }
  function addTag() { const tag = tagDraft.trim(); if (!tag) return; setAddedTags([...addedTags, tag]); setTagDraft(''); setAddingTag(false); setNotice(`Added ${tag} to the mock`); }
  const selected = design.tabs[tabIndex];

  return <div className="settings-design">
    <header className="sd-header"><div><h1>{design.title}</h1><p>{design.subtitle}</p></div><div className="sd-actions"><button type="button" onClick={revert}>Revert</button><button type="button" className="primary" onClick={() => setNotice('Settings applied in mock')}>Apply</button></div></header>
    {design.topTabs && <nav className="sd-tabs" aria-label={`${design.title} sections`}>{design.tabs.map((tab, index) => <button type="button" key={tab.title} className={index === tabIndex ? 'active' : ''} aria-current={index === tabIndex ? 'page' : undefined} onClick={() => setTabIndex(index)}>{tab.title}</button>)}</nav>}
    <div className="sd-layout"><nav className="sd-sidebar" aria-label={`${design.title} categories`}>{design.sidebar.map((label, index) => <button type="button" key={label} className={index === tabIndex ? 'active' : ''} onClick={() => setTabIndex(index)}>{label}</button>)}</nav>
      <main className="sd-main" aria-label={selected.title}><div className="sd-cards">{selected.cards.map((card, cardIndex) => <section className={card.matrix ? 'sd-card wide' : 'sd-card'} key={card.title}><h2>{card.title}</h2>
        {card.fields.map((field, fieldIndex) => { const key = `${tabIndex}:${cardIndex}:${fieldIndex}`; return <label className="sd-field" key={key}><span>{field.label}</span>{field.kind === 'select' ? <select value={values[key] ?? field.value} onChange={event => setValues({ ...values, [key]: event.target.value })}>{field.options?.map(option => <option key={option}>{option}</option>)}</select> : <output>{field.value}</output>}</label>; })}
        {card.tags && <div className="sd-tags">{[...card.tags, ...addedTags].map(tag => <span key={tag}>{tag}</span>)}{addingTag ? <form onSubmit={event => { event.preventDefault(); addTag(); }}><input aria-label="New tag" autoFocus value={tagDraft} onChange={event => setTagDraft(event.target.value)} /><button type="submit">Add</button></form> : <button type="button" onClick={() => setAddingTag(true)}>+ New Tag</button>}</div>}
        {card.matrix && <div className="sd-table-wrap"><table className="sd-matrix"><thead><tr>{card.matrix[0].map((column, index) => <th scope="col" key={`${column}-${index}`}>{column}</th>)}</tr></thead><tbody>{card.matrix.slice(1).map((row, rowIndex) => <tr key={row[0]}><th scope="row">{row[0]}</th>{row.slice(1).map((cell, columnIndex) => { const key = `${rowIndex}:${columnIndex}`; const mark = matrix[key] ?? cell; return <td key={key}><button type="button" aria-label={`${row[0]} × ${card.matrix?.[0][columnIndex + 1]}`} aria-pressed={mark === '✓'} onClick={() => setMatrix({ ...matrix, [key]: mark === '✓' ? '—' : '✓' })}>{mark}</button></td>; })}</tr>)}</tbody></table></div>}
      </section>)}</div></main></div>
    <footer className="sd-status">{notice ? <span className="notice">{notice}</span> : design.status.map(part => <span key={part}>{part}</span>)}</footer>
  </div>;
}
