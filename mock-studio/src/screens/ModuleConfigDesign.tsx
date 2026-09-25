import { useState } from 'react';
import './moduleConfigDesign.css';

type Row = { label: string; value: string; tone?: 'success' | 'warning' };
type Section = { id: string; title: string; rows: Row[] };

const sections: Section[] = [
  { id: 'modules', title: 'DesertRun.Core', rows: [
    { label: 'Kind', value: 'GameLibrary' },
    { label: 'Version', value: '0.4.2' },
    { label: 'Script Runtime', value: 'Lua 5.4 · embedded' },
    { label: 'Entry Point', value: 'init.lua' },
  ] },
  { id: 'services', title: 'Service Registry', rows: [
    { label: 'PlayerState', value: 'exported by DesertRun.Core' },
    { label: 'GameMode', value: 'exported by DesertRun.Core' },
    { label: 'EnemyAI', value: 'exported by DesertRun.AI' },
    { label: 'InputProvider', value: 'provided by engine' },
  ] },
  { id: 'dependencies', title: 'Dependency Graph', rows: [
    { label: 'com.example.doors', value: '≥ 2.1.0' },
    { label: 'com.example.guns', value: '≥ 1.2.0' },
    { label: 'horo.physics', value: 'engine built-in' },
    { label: 'horo.audio', value: 'engine built-in' },
  ] },
  { id: 'verification', title: 'Symbol Verification', rows: [
    { label: 'DesertRun.Core', value: '✓ all resolved', tone: 'success' },
    { label: 'DesertRun.AI', value: '✓ all resolved', tone: 'success' },
    { label: 'Package contracts', value: '1 unknown export', tone: 'warning' },
  ] },
];

const libraries = [
  { name: 'DesertRun.Core', role: 'primary' },
  { name: 'DesertRun.AI', role: 'secondary' },
  { name: 'Pkg: com.example.doors', role: 'services' },
  { name: 'Pkg: com.example.guns', role: 'assets' },
];

export function ModuleConfigDesign() {
  const [activeId, setActiveId] = useState(sections[0].id);
  const [selectedLibrary, setSelectedLibrary] = useState(libraries[0].name);
  const [notice, setNotice] = useState('');
  const active = sections.find(section => section.id === activeId) ?? sections[0];

  return <div className="module-config-design">
    <header className="mcd-header"><div><h1>Gameplay Integration Configuration</h1><p>Game libraries · package contributions · services · script runtime · verification</p></div></header>
    <nav className="mcd-tabs" aria-label="Gameplay integration views">
      {sections.map(section => <button key={section.id} type="button" className={activeId === section.id ? 'active' : ''} aria-current={activeId === section.id ? 'page' : undefined} onClick={() => setActiveId(section.id)}>{section.id[0].toUpperCase() + section.id.slice(1)}</button>)}
    </nav>
    <div className="mcd-layout">
      <aside className="mcd-libraries" aria-label="Game libraries"><h2>Game Libraries</h2>
        {libraries.map(library => <button key={library.name} type="button" className={selectedLibrary === library.name ? 'selected' : ''} onClick={() => setSelectedLibrary(library.name)}><span>{library.name}</span><small>{library.role}</small></button>)}
      </aside>
      <main className="mcd-main"><section aria-labelledby="mcd-section-heading"><h2 id="mcd-section-heading">{active.title}</h2>
        <dl>{active.rows.map(row => <div key={row.label}><dt>{row.label}</dt><dd className={row.tone || ''}>{row.value}</dd></div>)}</dl>
      </section><div className="mcd-actions"><button type="button" className="primary" onClick={() => { setActiveId('verification'); setNotice('Symbol verification completed'); }}>Verify All</button><button type="button" onClick={() => setNotice(`Reload requested for ${selectedLibrary}`)}>Reload Modules</button><button type="button" onClick={() => setNotice(`Stub generation requested for ${selectedLibrary}`)}>Generate Stubs</button></div></main>
    </div>
    <footer className="mcd-status"><span>{notice || '2 game libs · 2 package contributions'}</span><span>Script: Lua 5.4</span><span>Verification: passed</span></footer>
  </div>;
}
