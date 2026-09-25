import { useMemo, useState } from 'react';
import fixture from '../fixtures/consolePanel.json';
import './consoleDesign.css';

type RecordRow = { time: string; level: string; source: string; message: string };
type RecordGroup = 'console' | 'commands' | 'history';
const initialRecords = fixture.records as Record<RecordGroup, RecordRow[]>;
const tabs = fixture.tabs as string[];

export function ConsoleDesign() {
  const [tabIndex, setTabIndex] = useState(0);
  const [records, setRecords] = useState(initialRecords);
  const [filter, setFilter] = useState('All');
  const [search, setSearch] = useState('');
  const [command, setCommand] = useState('');
  const [paused, setPaused] = useState(false);
  const [selected, setSelected] = useState<RecordRow>(initialRecords.console[3]);
  const [notice, setNotice] = useState('');
  const group = tabs[tabIndex].toLowerCase() as RecordGroup | 'variables';
  const visible = useMemo(() => group === 'variables' ? [] : records[group].filter(row => {
    const level = filter === 'All' || (filter === 'Warnings' && row.level === 'WARN') || (filter === 'Errors' && row.level === 'ERROR') || (filter === 'Info' && row.level === 'INFO') || (filter === 'Muted' && false);
    return level && `${row.time} ${row.level} ${row.source} ${row.message}`.toLowerCase().includes(search.toLowerCase());
  }), [group, records, filter, search]);

  function runCommand() {
    if (!command.trim() || paused) return;
    const row: RecordRow = { time: new Date().toLocaleTimeString('en-GB', { hour12: false }), level: 'INFO', source: 'command', message: `> ${command.trim()} (preview only)` };
    setRecords({ ...records, console: [...records.console, row], commands: [...records.commands, row], history: [row, ...records.history] });
    setSelected(row);
    setCommand('');
    setNotice('Command added to mock history');
  }

  function clearVisible() {
    if (group === 'variables') return;
    const toRemove = new Set(visible);
    setRecords({ ...records, [group]: records[group].filter(row => !toRemove.has(row)) });
    setNotice(`Cleared ${toRemove.size} visible records`);
  }

  function exportVisible() {
    const body = visible.map(row => [row.time, row.level, row.source, row.message].join('\t')).join('\n');
    const url = URL.createObjectURL(new Blob([`Time\tLevel\tSource\tMessage\n${body}\n`], { type: 'text/tab-separated-values' }));
    const link = document.createElement('a');
    link.href = url;
    link.download = 'console-mock.tsv';
    link.click();
    URL.revokeObjectURL(url);
    setNotice('Visible records exported');
  }

  return <div className="console-design">
    <header className="cd-header"><div><h1>{fixture.title}</h1><p>{fixture.subtitle}</p></div><div className="cd-actions"><button type="button" onClick={clearVisible} disabled={group === 'variables'}>Clear Visible</button><button type="button" onClick={() => setPaused(!paused)}>{paused ? 'Resume' : 'Pause'}</button><button type="button" className="primary" onClick={exportVisible} disabled={group === 'variables'}>Export</button></div></header>
    <nav className="cd-tabs" aria-label="Console views">{tabs.map((tab, index) => <button key={tab} type="button" className={tabIndex === index ? 'active' : ''} aria-current={tabIndex === index ? 'page' : undefined} onClick={() => setTabIndex(index)}>{tab}</button>)}</nav>
    <div className="cd-toolbar"><label htmlFor="cd-search">Filters</label><input id="cd-search" value={search} onChange={event => setSearch(event.target.value)} placeholder={fixture.search} />{['All', 'Info', 'Warnings', 'Errors', 'Muted'].map(level => <button key={level} type="button" className={filter === level ? 'active' : ''} aria-pressed={filter === level} onClick={() => setFilter(level)}>{level}</button>)}</div>
    <div className="cd-layout"><nav className="cd-rail" aria-label="Console shortcuts"><button type="button" title="Log Stream" onClick={() => setTabIndex(0)}>≡</button><button type="button" title="Commands" onClick={() => setTabIndex(1)}>›_</button><button type="button" title="Variables" onClick={() => setTabIndex(2)}>▥</button></nav>
      <main className="cd-main">{group === 'variables' ? <div className="cd-variables">{fixture.variables.map(item => <div key={item.label}><span>{item.label}</span><strong>{item.value}</strong></div>)}</div> : <div className="cd-records" role="table" aria-label={`${tabs[tabIndex]} records`}><div className="cd-row heading" role="row"><span>Time</span><span>Level</span><span>Source</span><span>Message</span></div>{visible.map((row, index) => <button type="button" role="row" key={`${row.time}-${row.source}-${index}`} className={`cd-row ${selected === row ? 'selected' : ''}`} onClick={() => setSelected(row)}><span>{row.time}</span><span className={`level ${row.level.toLowerCase()}`}>{row.level}</span><span className="source">{row.source}</span><span>{row.message}</span></button>)}</div>}
        {(group === 'console' || group === 'commands') && <form className="cd-command" onSubmit={event => { event.preventDefault(); runCommand(); }}><span>&gt;</span><input aria-label="Console command" value={command} onChange={event => setCommand(event.target.value)} placeholder="e.g. stat fps, log_level renderer debug" disabled={paused} /><button type="submit" disabled={paused || !command.trim()}>Run</button></form>}
      </main><aside className="cd-detail"><h2>Selected Record <span className={selected.level.toLowerCase()}>{selected.level}</span></h2><dl><div><dt>time</dt><dd>{selected.time}</dd></div><div><dt>category</dt><dd>{selected.source}</dd></div><div><dt>level</dt><dd>{selected.level}</dd></div>{selected.level === 'ERROR' && fixture.detail.slice(2).map(item => <div key={item.label}><dt>{item.label}</dt><dd>{item.value}</dd></div>)}</dl>{selected.level === 'ERROR' && <pre>{fixture.stack}</pre>}</aside></div>
    <footer className="cd-status">{notice ? <span className="notice">{notice}</span> : fixture.status.map(item => <span key={item}>{item}</span>)}<span className={paused ? 'paused' : 'live'}>● {paused ? 'paused' : 'live'}</span></footer>
  </div>;
}
