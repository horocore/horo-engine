import { useState } from 'react';
import fixtures from '../fixtures/simpleWorkbenches.json';
import './simpleWorkbench.css';

type Block =
  | { type: 'metrics'; items: { label: string; value: string; unit: string }[] }
  | { type: 'table'; title: string; headers: string[]; rows: string[][]; searchable?: boolean }
  | { type: 'form'; title: string; fields: { label: string; value: string; options?: string[] }[] }
  | { type: 'actions'; labels: string[] }
  | { type: 'empty'; title: string };
type Workbench = { title: string; subtitle: string; sidebarTitle?: string; sidebar?: { label: string; value: string }[]; status: string[]; tabs: { title: string; blocks: Block[] }[] };
const designs = fixtures as Record<string, Workbench>;
export const simpleWorkbenchIds = Object.keys(designs);

export function SimpleWorkbench({ designId }: { designId: string }) {
  const design = designs[designId];
  const [tabIndex, setTabIndex] = useState(0);
  const [search, setSearch] = useState('');
  const [fields, setFields] = useState<Record<string, string>>({});
  const [notice, setNotice] = useState('');
  const [addingString, setAddingString] = useState(false);
  const [stringKey, setStringKey] = useState('');
  const [stringValue, setStringValue] = useState('');
  const [extraStrings, setExtraStrings] = useState<string[][]>([]);
  if (!design) return null;

  function action(label: string) {
    if (label === '+ Add String') { setAddingString(true); return; }
    setNotice(`${label} selected in mock`);
  }

  return <div className="simple-workbench">
    <header className="sw-header"><h1>{design.title}</h1><p>{design.subtitle}</p></header>
    <nav className="sw-tabs" aria-label={`${design.title} views`}>{design.tabs.map((tab, index) => <button key={tab.title} type="button" className={index === tabIndex ? 'active' : ''} aria-current={index === tabIndex ? 'page' : undefined} onClick={() => { setTabIndex(index); setSearch(''); }}>{tab.title}</button>)}</nav>
    <div className={design.sidebar ? 'sw-layout with-sidebar' : 'sw-layout'}>
      {design.sidebar && <aside className="sw-sidebar"><h2>{design.sidebarTitle}</h2>{design.sidebar.map(item => <div key={item.label}><span>{item.label}</span><small>{item.value}</small></div>)}</aside>}
      <main className="sw-content">{design.tabs[tabIndex].blocks.map((block, blockIndex) => {
        if (block.type === 'metrics') return <div className="sw-metrics" key={blockIndex}>{block.items.map(item => <section key={item.label}><span>{item.label}</span><strong>{item.value}</strong><small>{item.unit}</small></section>)}</div>;
        if (block.type === 'table') {
          const rows = block.rows.concat(designId.endsWith('localization-editor.html') && block.title === 'String Table' ? extraStrings : []);
          const visible = block.searchable ? rows.filter(row => row.join(' ').toLowerCase().includes(search.toLowerCase())) : rows;
          return <section className="sw-table-section" key={blockIndex}><div className="sw-section-heading"><h2>{block.title}</h2>{block.searchable && <input aria-label={`Search ${block.title}`} placeholder="Search keys or values…" value={search} onChange={event => setSearch(event.target.value)} />}</div><div className="sw-table-scroll"><table><thead><tr>{block.headers.map(header => <th key={header} scope="col">{header}</th>)}</tr></thead><tbody>{visible.map((row, index) => <tr key={`${row[0]}-${index}`}>{row.map((cell, column) => <td key={column}>{cell}</td>)}</tr>)}</tbody></table></div></section>;
        }
        if (block.type === 'form') return <section className="sw-form" key={blockIndex}><h2>{block.title}</h2><div>{block.fields.map((field, fieldIndex) => { const key = `${tabIndex}:${blockIndex}:${fieldIndex}`; return <label key={key}><span>{field.label}</span>{field.options ? <select value={fields[key] ?? field.value} onChange={event => setFields({ ...fields, [key]: event.target.value })}>{field.options.map(option => <option key={option}>{option}</option>)}</select> : <input value={fields[key] ?? field.value} onChange={event => setFields({ ...fields, [key]: event.target.value })} />}</label>; })}</div></section>;
        if (block.type === 'actions') return <div className="sw-actions" key={blockIndex}>{block.labels.map(label => <button type="button" key={label} onClick={() => action(label)}>{label}</button>)}{addingString && <form onSubmit={event => { event.preventDefault(); if (!stringKey.trim()) return; setExtraStrings([...extraStrings, [stringKey.trim(), stringValue.trim(), '—']]); setAddingString(false); setStringKey(''); setStringValue(''); setNotice('String added to the mock'); }}><input aria-label="String key" placeholder="Key" value={stringKey} onChange={event => setStringKey(event.target.value)} /><input aria-label="Source value" placeholder="en-US value" value={stringValue} onChange={event => setStringValue(event.target.value)} /><button type="submit">Add</button></form>}</div>;
        return <section className="sw-empty" key={blockIndex}><h2>{block.title}</h2><p>The reference mock does not define content for this view yet.</p></section>;
      })}</main>
    </div>
    <footer className="sw-status">{notice ? <span className="notice">{notice}</span> : design.status.map(part => <span key={part}>{part}</span>)}</footer>
  </div>;
}
