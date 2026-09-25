import { useState } from 'react';
import { WorkspaceShell } from '../design-system/WorkspaceShell';
import fixtures from '../fixtures/remainingDesigns.json';
import './semanticDesign.css';

type Block =
  | { type: 'heading'; text: string }
  | { type: 'text'; text: string }
  | { type: 'row'; text: string }
  | { type: 'action'; label: string }
  | { type: 'field'; label: string; value: string; placeholder?: string; options?: string[] }
  | { type: 'toggle'; label: string; checked: boolean }
  | { type: 'table'; headers: string[]; rows: string[][] }
  | { type: 'visual'; title: string; kind: string; labels: string[] }
  | { type: 'card'; title: string; blocks: Block[]; selectable?: boolean }
  | { type: 'group'; layout: 'grid'; blocks: Block[] };

type Design = {
  title: string;
  subtitle: string;
  tabs: { title: string; blocks: Block[] }[];
  leftTitle: string;
  left: Block[];
  rightTitle: string;
  right: Block[];
  status: string[];
  actions: string[];
};

const designs = fixtures as Record<string, Design>;
export const semanticDesignIds = Object.keys(designs);

function useBlockState(initialSelected = '') {
  const [values, setValues] = useState<Record<string, string | boolean>>({});
  const [selected, setSelected] = useState(initialSelected);
  const [notice, setNotice] = useState('');
  return { values, setValues, selected, setSelected, notice, setNotice };
}

type BlockState = ReturnType<typeof useBlockState>;

function BlockList({ blocks, path, state }: { blocks: Block[]; path: string; state: BlockState }) {
  return <div className="semantic-blocks">{blocks.map((block, index) => {
    const key = `${path}.${index}`;
    if (block.type === 'heading') return <h3 className="semantic-heading" key={key}>{block.text}</h3>;
    if (block.type === 'text') return <p className="semantic-text" key={key}>{block.text}</p>;
    if (block.type === 'row') return <button type="button" key={key} className={`semantic-row${state.selected === block.text ? ' selected' : ''}`} onClick={() => state.setSelected(block.text)}>{block.text}</button>;
    if (block.type === 'action') return <button type="button" className="semantic-action" key={key} onClick={() => state.setNotice(`${block.label} selected in mock`)}>{block.label}</button>;
    if (block.type === 'field') return <label className="semantic-field" key={key}><span>{block.label}</span>{block.options?.length ? <select value={String(state.values[key] ?? block.value)} onChange={event => state.setValues({ ...state.values, [key]: event.target.value })}>{block.options.map((option, optionIndex) => <option key={`${option}-${optionIndex}`}>{option}</option>)}</select> : <input value={String(state.values[key] ?? block.value)} placeholder={block.placeholder} onChange={event => state.setValues({ ...state.values, [key]: event.target.value })} />}</label>;
    if (block.type === 'toggle') return <label className="semantic-toggle" key={key}><input type="checkbox" checked={Boolean(state.values[key] ?? block.checked)} onChange={event => state.setValues({ ...state.values, [key]: event.target.checked })} /><span>{block.label}</span></label>;
    if (block.type === 'table') return <div className="semantic-table-scroll" key={key}><table><thead>{block.headers.length > 0 && <tr>{block.headers.map((header, column) => <th key={column} scope="col">{header}</th>)}</tr>}</thead><tbody>{block.rows.map((row, rowIndex) => <tr key={rowIndex}>{row.map((cell, column) => <td key={column}>{cell}</td>)}</tr>)}</tbody></table></div>;
    if (block.type === 'visual') {
      const graph = /graph|map|flow|routing|node-canvas/.test(block.kind);
      const timeline = /timeline|curve/.test(block.kind);
      return <section className={`semantic-visual ${block.kind}${graph ? ' is-graph' : ''}${timeline ? ' is-timeline' : ''}`} key={key} aria-label={block.title || block.kind}><div className="semantic-visual-top">{block.title || block.kind.replaceAll('-', ' ')}</div>
        {graph ? <div className="semantic-graph-nodes">{block.labels.slice(0, 12).map((label, labelIndex) => <button type="button" key={`${label}-${labelIndex}`} className={state.selected === label ? 'selected' : ''} onClick={() => state.setSelected(label)}><small>NODE {String(labelIndex + 1).padStart(2, '0')}</small>{label}</button>)}</div>
          : timeline ? <div className="semantic-timeline-tracks">{block.labels.slice(0, 12).map((label, labelIndex) => <div key={`${label}-${labelIndex}`}><span>{label}</span><i style={{ marginLeft: `${(labelIndex * 17) % 55}%`, width: `${20 + (labelIndex * 9) % 24}%` }} /></div>)}</div>
            : <div className="semantic-visual-art"><span>◇</span></div>}
        {!graph && !timeline && <div className="semantic-visual-labels">{block.labels.map((label, labelIndex) => <span key={`${label}-${labelIndex}`}>{label}</span>)}</div>}
      </section>;
    }
    if (block.type === 'card') {
      if (block.selectable) return <button type="button" className={`semantic-card semantic-selectable${state.selected === block.title ? ' selected' : ''}`} key={key} onClick={() => state.setSelected(block.title)}><strong>{block.title}</strong>{block.blocks.map((item, itemIndex) => item.type === 'text' ? <span key={itemIndex}>{item.text}</span> : null)}</button>;
      const pairs = block.blocks.length >= 4 && block.blocks.length % 2 === 0 && block.blocks.every(item => item.type === 'text' && item.text.length < 80);
      return <section className="semantic-card" key={key}>{block.title && <h3>{block.title}</h3>}{pairs ? <dl className="semantic-kv">{Array.from({ length: block.blocks.length / 2 }, (_, pairIndex) => <div key={pairIndex}><dt>{(block.blocks[pairIndex * 2] as { text: string }).text}</dt><dd>{(block.blocks[pairIndex * 2 + 1] as { text: string }).text}</dd></div>)}</dl> : <BlockList blocks={block.blocks} path={key} state={state} />}</section>;
    }
    return <div className="semantic-grid" key={key}><BlockList blocks={block.blocks} path={key} state={state} /></div>;
  })}</div>;
}

const compactIds = new Set([
  'architecture/editor/editor-modal-host-example.html',
  'architecture/editor/new-project-wizard.html',
  'architecture/editor/settings-modal.html',
  'architecture/extensions/plugin-manager.html',
  'architecture/runtime/asset-import-modal.html',
  'architecture/observability/observability-dashboard.html',
]);

export function SemanticDesign({ designId }: { designId: string }) {
  const design = designs[designId];
  const [activeTab, setActiveTab] = useState(0);
  const wizard = designId === 'architecture/editor/new-project-wizard.html';
  const state = useBlockState(wizard ? '3D Starter' : '');
  if (!design) return null;

  const tabs = design.tabs.map(tab => tab.title);
  const content = <BlockList blocks={design.tabs[activeTab].blocks} path={`${designId}.${activeTab}`} state={state} />;
  if (compactIds.has(designId)) return <div className={`semantic-modal${wizard ? ' wizard' : ''}`}>
    <header className="semantic-modal-header"><div><h1>{design.title}</h1>{design.subtitle && <p>{design.subtitle}</p>}</div><div>{design.actions.map(label => <button key={label} type="button" onClick={() => state.setNotice(`${label} selected in mock`)}>{label}</button>)}</div></header>
    <nav className="semantic-modal-tabs" aria-label={`${design.title} views`}>{tabs.map((tab, index) => <button key={`${tab}-${index}`} type="button" className={index === activeTab ? 'active' : ''} onClick={() => setActiveTab(index)}>{tab}</button>)}</nav>
    <main className="semantic-modal-content">{content}</main>
    <footer className="semantic-modal-footer">{wizard ? <><span>{state.notice || `Template: ${state.selected}`}</span><div><button type="button" disabled={activeTab === 0} onClick={() => setActiveTab(activeTab - 1)}>Back</button><button type="button" className="primary" onClick={() => activeTab < tabs.length - 1 ? setActiveTab(activeTab + 1) : state.setNotice('Project creation is a mock action')}>{activeTab === tabs.length - 1 ? 'Create Project' : 'Continue'}</button></div></> : state.notice || design.status.join(' · ') || `${design.title} design reference`}</footer>
  </div>;

  return <WorkspaceShell title={design.title} subtitle={design.subtitle} tabs={tabs} activeTab={activeTab} onTabChange={setActiveTab}
    actions={design.actions.map(label => <button key={label} type="button" onClick={() => state.setNotice(`${label} selected in mock`)}>{label}</button>)}
    leftTitle={design.leftTitle || 'Browser'} left={<BlockList blocks={design.left} path={`${designId}.left`} state={state} />}
    rightTitle={design.rightTitle || 'Inspector'} right={<BlockList blocks={design.right} path={`${designId}.right`} state={state} />}
    status={state.notice ? <span>{state.notice}</span> : design.status.map((item, index) => <span key={`${item}-${index}`}>{item}</span>)}>
    {content}
  </WorkspaceShell>;
}
