import { useState } from 'react';
import './assetBrowserDesign.css';

const assets = [
  { name: 'SteelSword LOD0', type: 'mesh', detail: 'Static Mesh', tris: '3,420', size: '1.2 MB' },
  { name: 'RoundShield', type: 'mesh', detail: 'Static Mesh', tris: '1,840', size: '940 KB' },
  { name: 'LongBow', type: 'mesh', detail: 'Static Mesh', tris: '2,180', size: '1.1 MB' },
  { name: 'Revolver', type: 'mesh', detail: 'Static Mesh', tris: '4,620', size: '1.8 MB' },
  { name: 'Sword Albedo', type: 'texture', detail: 'Texture 2D', tris: '—', size: '4.3 MB' },
  { name: 'Sword Normal', type: 'texture', detail: 'Texture 2D', tris: '—', size: '4.1 MB' },
  { name: 'Sword Material', type: 'material', detail: 'Material', tris: '—', size: '27 KB' },
];
const folders = [{ name: 'Assets', depth: 0 }, { name: 'Props', depth: 1 }, { name: 'Weapons', depth: 2 }, { name: 'Furniture', depth: 2 }, { name: 'Characters', depth: 1 }, { name: 'Scenes', depth: 1 }, { name: 'Packages', depth: 0 }];

export function AssetBrowserDesign({ navigate, openDesign }: { navigate: (id: string) => void; openDesign: (id: string) => void }) {
  const [selected, setSelected] = useState(assets[0].name);
  const [folder, setFolder] = useState('Weapons');
  const [search, setSearch] = useState('');
  const [view, setView] = useState<'grid' | 'list'>('grid');
  const asset = assets.find(item => item.name === selected) || assets[0];
  const visible = assets.filter(item => item.name.toLowerCase().includes(search.toLowerCase()));
  return <div className="asset-design">
    <header className="asset-design-header"><h1>Asset Browser</h1><span>Project Assets</span></header>
    <div className="asset-design-toolbar"><button type="button" aria-label="Back" onClick={() => setFolder('Props')}>←</button><button type="button" aria-label="Forward" onClick={() => setFolder('Weapons')}>→</button><button type="button" aria-label="Up" onClick={() => setFolder('Assets')}>↑</button><span className="asset-crumb">Assets / Props / {folder}</span><input aria-label="Search assets" placeholder="Search assets…" value={search} onChange={event => setSearch(event.target.value)} /><div className="asset-view"><button type="button" aria-pressed={view === 'grid'} onClick={() => setView('grid')}>▦</button><button type="button" aria-pressed={view === 'list'} onClick={() => setView('list')}>☷</button></div><button type="button" className="primary" onClick={() => openDesign('architecture/runtime/asset-import-modal.html')}>＋ Import</button></div>
    <div className="asset-design-body"><aside className="asset-tree" aria-label="Asset folders">{folders.map(item => <button type="button" key={item.name} className={folder === item.name ? 'active' : ''} style={{ paddingLeft: 12 + item.depth * 17 }} onClick={() => setFolder(item.name)}><span>{item.name === 'Props' || item.name === 'Assets' ? '▾' : '▸'}</span> ▱ {item.name}</button>)}</aside>
      <main className="asset-list"><div className="asset-list-heading"><span>{folder} / {visible.length} items</span><span>Selected: {asset.name}.{asset.type}</span></div><div className={view === 'grid' ? 'asset-cards' : 'asset-cards list'}>{visible.map(item => <button key={item.name} type="button" className={selected === item.name ? 'active' : ''} onClick={() => setSelected(item.name)}><span className={`asset-icon ${item.type}`}>{item.type === 'mesh' ? '⬡' : item.type === 'texture' ? '▧' : '◈'}</span><strong>{item.name}</strong><small>{item.type}</small></button>)}</div></main>
      <aside className="asset-preview"><h2>Preview</h2><div className="asset-preview-art">{asset.type === 'mesh' ? '⬡' : asset.type === 'texture' ? '▧' : '◈'}</div><dl>{[['Name', asset.name], ['Type', asset.detail], ['GUID', 'a1b2..9f'], ['Tris', asset.tris], ['LODs', asset.type === 'mesh' ? '3' : '—'], ['Size', asset.size], ['Imported', 'today']].map(([label, value]) => <div key={label}><dt>{label}</dt><dd>{value}</dd></div>)}</dl><div className="asset-preview-actions"><button type="button" onClick={() => navigate('architecture/editor/editor-workspace.html')}>Open</button><button type="button" onClick={() => openDesign('architecture/runtime/asset-import-modal.html')}>Import Settings</button></div></aside>
    </div>
    <footer className="asset-design-status"><span>{visible.length} assets · 1 selected</span><span>Last import: 2 min ago</span></footer>
  </div>;
}
