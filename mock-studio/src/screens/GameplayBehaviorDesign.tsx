import { useState } from 'react';
import { GraphCanvas } from '../design-system/GraphCanvas';
import { WorkspaceShell } from '../design-system/WorkspaceShell';
import './gameplayBehaviorDesign.css';

const nodes = [
  { title: 'On Fixed Update', summary: 'phase = SystemPhase::Gameplay', x: 30, y: 120 },
  { title: 'Was Pressed', summary: 'game.desert_run.interact', x: 300, y: 52 },
  { title: 'Sequence', summary: 'Flow · In → Out', x: 300, y: 210 },
  { title: 'Move To Waypoint', summary: 'reads engine.transform', x: 570, y: 25 },
  { title: 'Set Component', summary: 'undeclared write: door_state', x: 570, y: 175 },
  { title: 'Advance Waypoint Index', summary: 'patrol loop', x: 570, y: 330 },
];
const edges = [
  'M210 150 C255 150 255 85 300 85', 'M210 170 C255 170 255 245 300 245',
  'M480 245 C530 245 530 58 570 58', 'M480 255 C530 255 530 208 570 208',
  'M480 265 C530 265 530 362 570 362',
];
const palette = [
  ['Events', 'On Create', 'On Enable', 'On Fixed Update', 'On Presentation Update'],
  ['Flow', 'Branch', 'Sequence'],
  ['Conditions', 'Was Pressed (Input Action)', 'Cooldown Ready'],
  ['Component Access', 'Read Component', 'Set Component (Command)'],
  ['Actions', 'Move To Waypoint', 'Request Scene Transition'],
];

export function GameplayBehaviorDesign() {
  const [tab, setTab] = useState(0);
  const [selected, setSelected] = useState('Sequence');
  const [search, setSearch] = useState('');
  const [notice, setNotice] = useState('');
  const current = nodes.find(node => node.title === selected);
  return <WorkspaceShell title="Gameplay Behavior Editor" subtitle="Visual Behavior Graph Asset · assets/graphs/NPCPatrol.behaviorgraph" tabs={['Graph', 'Diagnostics']} activeTab={tab} onTabChange={setTab}
    badges={<span className="wb-badge warning">Not attachable · 1 diagnostic</span>}
    actions={<><button type="button" onClick={() => setNotice('Reference graph reverted')}>Revert</button><button type="button" className="primary" onClick={() => setNotice('Graph save requested in mock')}>Save Graph</button></>}
    toolbar={<><button type="button" onClick={() => setNotice('Selection framed')}>Frame Selection</button><button type="button" onClick={() => setNotice('Grid enabled')}>Grid</button><span className="behavior-toolbar-path">Assets / Graphs / NPC Patrol</span></>}
    leftTitle="Node Palette" left={<div className="behavior-palette"><input aria-label="Search nodes" placeholder="Search nodes…" value={search} onChange={event => setSearch(event.target.value)} />{palette.map(([group, ...items]) => { const visible = items.filter(item => item.toLowerCase().includes(search.toLowerCase())); return visible.length ? <section key={group}><h3>{group}</h3>{visible.map(item => <button type="button" key={item} onClick={() => { setSelected(item); setNotice(`${item} selected in palette`); }}>{item}</button>)}</section> : null; })}</div>}
    rightTitle="Inspector" right={<div className="behavior-inspector"><h3>Descriptor</h3><label>Type Id<input defaultValue="game.desert_run.npc_patrol" /></label><label>Display Name<input defaultValue="NPC Patrol" /></label><label>Category<input defaultValue="Gameplay/AI" /></label><label>Schema Ver.<input defaultValue="3" /></label><h3>Selected Node</h3><strong>{selected}</strong><p>{current?.summary || 'Choose a node from the graph to inspect its fields.'}</p><h3>Phase Access</h3><p>Gameplay · fixed tick</p><h3>Diagnostics</h3><p className="behavior-error">Set Component writes an undeclared component.</p></div>}
    status={notice ? <span>{notice}</span> : <><span>Nodes: 6</span><span>Connections: 5 (1 invalid)</span><span>Last Scan: 4s ago</span><span>Reload Safe Point: next fixed tick</span></>}>
    {tab === 1 ? <div className="behavior-diagnostics"><h2>Graph Diagnostics</h2><div><strong>1 error</strong><p>Set Component: write access not declared on graph for game.desert_run.door_state.</p></div></div> : <GraphCanvas nodes={nodes} edges={edges} selected={selected} onSelect={setSelected} width={790} height={475} />}
  </WorkspaceShell>;
}
