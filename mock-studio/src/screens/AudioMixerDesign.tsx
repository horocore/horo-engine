import { useState } from 'react';
import { WorkspaceShell } from '../design-system/WorkspaceShell';
import './audioMixerDesign.css';

const buses = [
  { name: 'Music', level: '−6.3', gain: '−3.5 dB', height: 66, pan: 50 },
  { name: 'SFX', level: '−2.8', gain: '0.0 dB', height: 72, pan: 65 },
  { name: 'UI', level: '−18.5', gain: '0.0 dB', height: 30, pan: 50 },
  { name: 'Voice', level: '−0.4', gain: '−12.0 dB', height: 48, pan: 48 },
  { name: 'Ambient', level: '−8.2', gain: '−4.0 dB', height: 58, pan: 50 },
  { name: 'Master', level: '−14.2 LUFS', gain: '−3.0 dB', height: 72, pan: 50 },
];
const events = [
  ['enemy footstep → evt 0142', 'SFX · 2'], ['explosion large → evt 0089', 'SFX · 1'],
  ['ambient loop cave → evt 0203', 'Ambient · 1'], ['ui click → evt 0014', 'UI · 0'],
  ['dialogue greeting tr-TR → evt 0301', 'Voice · 1'], ['Event lookup failures', '0'],
];

export function AudioMixerDesign() {
  const [tab, setTab] = useState(0);
  const [selected, setSelected] = useState('Master');
  const [playing, setPlaying] = useState(false);
  const [controls, setControls] = useState<Record<string, boolean>>({});
  const [gain, setGain] = useState<Record<string, number>>({});
  const toggle = (key: string) => setControls({ ...controls, [key]: !controls[key] });
  const bus = buses.find(item => item.name === selected) || buses[5];
  return <WorkspaceShell title="Audio Mixer" subtitle="Transport · Faders · EQ · Compressor · Reverb Sends · VU Meters" tabs={['Mixer', 'Banks', 'Debug']} activeTab={tab} onTabChange={setTab}
    actions={<div className="audio-transport">{['⏮', playing ? '⏸' : '▶', '⏹', '⏺'].map((label, index) => <button type="button" key={index} onClick={() => setPlaying(index === 1 ? !playing : false)}>{label}</button>)}<span>00:01:42.844 · 84 BPM · 4/4 · bar 17</span></div>}
    leftTitle="Bus Tree" left={<div className="audio-bus-list">{[buses[5], ...buses.slice(0, 5)].map(item => <button type="button" key={item.name} className={selected === item.name ? 'active' : ''} onClick={() => setSelected(item.name)}><strong>{item.name}</strong><span>{item.level}</span></button>)}<div className="audio-budget">Voices: <strong>18 / 64</strong><small>Virtualized: 3 · Rejected: 0</small><small>Shared native + middleware budget</small></div><p>Solo is editor session state; mute and pause are bus parameters.</p></div>}
    rightTitle={`Bus Controls — ${selected}`} right={<div className="audio-inspector"><h3>Loudness</h3><div className="audio-loudness">{[['−14.2', 'LUFS', 72], ['−1.2', 'TP', 88], ['−18.4', 'RMS', 52], ['0', 'Clip', 3]].map(([value, label, height]) => <div key={label}><strong>{value}</strong><i><b style={{ height: `${height}%` }} /></i><small>{label}</small></div>)}</div><h3>Bus Controls — {selected}</h3><div className="audio-inspector-row"><span>Volume</span><strong>{gain[selected] !== undefined ? `${gain[selected]} dB` : bus.gain}</strong></div>{['Mute', 'Pause'].map(label => <label key={label} className="audio-check"><input type="checkbox" checked={Boolean(controls[`${selected}:${label}`])} onChange={() => toggle(`${selected}:${label}`)} />{label}</label>)}<div className="audio-inspector-row"><span>Route</span><strong>Output Device</strong></div><h3>Core Effects — {selected} Bus</h3>{[['Gain', '+1.0 dB'], ['High Pass', '80 Hz'], ['Low Pass', '18.0 kHz'], ['Reverb Send', 'Active']].map(([label, value]) => <div className="audio-inspector-row" key={label}><span>{label}</span><strong>{value}</strong></div>)}<h3>Reverb Sends</h3>{buses.slice(0, 5).map(item => <label className="audio-send" key={item.name}><span>{item.name}</span><input type="range" defaultValue={item.pan} /><small>{item.pan}%</small></label>)}</div>}
    status={<><span>Device: Stereo 48 kHz · 10.7 ms</span><span>Scene: 4 zones active</span><span>Playback: 2D+3D</span><span>Snapshot: combat</span></>}>
    {tab === 0 ? <div className="audio-strips">{buses.map(item => <section className={`audio-strip${selected === item.name ? ' selected' : ''}`} key={item.name} onClick={() => setSelected(item.name)}><h2>{item.name}</h2><small>{item.level}</small><div className="audio-strip-meter"><i style={{ height: `${item.height}%` }} /></div><input aria-label={`${item.name} gain`} type="range" min="-48" max="6" value={gain[item.name] ?? Number(item.gain.replace(/[− dB]/g, '')) * (item.gain.startsWith('−') ? -1 : 1)} onChange={event => setGain({ ...gain, [item.name]: Number(event.target.value) })} /><span className="audio-gain">{gain[item.name] !== undefined ? `${gain[item.name]} dB` : item.gain}</span>{item.name !== 'Master' ? <div className="audio-strip-controls">{['M', 'P', 'S'].map(label => <button type="button" key={label} className={controls[`${item.name}:${label}`] ? 'active' : ''} onClick={event => { event.stopPropagation(); toggle(`${item.name}:${label}`); }}>{label}</button>)}</div> : <strong className="audio-final">FINAL OUTPUT</strong>}<label> L <input aria-label={`${item.name} pan`} type="range" defaultValue={item.pan} disabled={item.name === 'Master'} /> R </label></section>)}</div>
      : tab === 1 ? <section className="audio-data-card"><h2>Middleware Event Bank <span>FMOD 2.4 — event bridge</span></h2><div className="audio-data-rows">{events.map(([name, value]) => <div key={name}><span>{name}</span><strong>{value}</strong></div>)}</div><p>Event names resolve to stable numeric IDs at cook time. Native and middleware voices share the same 18 / 64 budget.</p></section>
        : <div className="audio-debug"><div>{[['2.1 ms', 'Callback / 10.7 ms budget'], ['0.6 ms', 'Mixer CPU time'], ['0', 'Underrun count'], ['4 / 256', 'Command queue depth']].map(([value, label]) => <section key={label}><strong>{value}</strong><span>{label}</span></section>)}</div><section className="audio-data-card"><h2>Audio Debug</h2><p>Playback device: Stereo 48 kHz · Middleware: FMOD 2.4 · Reverb: Convolution IR — Concrete Hall</p></section></div>}
  </WorkspaceShell>;
}
