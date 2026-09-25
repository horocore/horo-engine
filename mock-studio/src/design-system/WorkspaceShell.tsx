import { useState, type ReactNode } from 'react';
import './workspaceShell.css';

type WorkspaceShellProps = {
  title: string;
  subtitle: string;
  badges?: ReactNode;
  actions?: ReactNode;
  tabs: string[];
  activeTab: number;
  onTabChange: (index: number) => void;
  toolbar?: ReactNode;
  leftTitle: string;
  left: ReactNode;
  rightTitle: string;
  right: ReactNode;
  status: ReactNode;
  children: ReactNode;
};

export function WorkspaceShell({ title, subtitle, badges, actions, tabs, activeTab, onTabChange, toolbar, leftTitle, left, rightTitle, right, status, children }: WorkspaceShellProps) {
  const [leftOpen, setLeftOpen] = useState(true);
  const [rightOpen, setRightOpen] = useState(true);
  return <div className={`wb-shell${leftOpen ? '' : ' left-closed'}${rightOpen ? '' : ' right-closed'}`}>
    <header className="wb-header"><div className="wb-title"><h1>{title}</h1><p>{subtitle}</p></div><div className="wb-header-actions">{badges}{actions}</div></header>
    <nav className="wb-tabs" aria-label={`${title} views`}>{tabs.map((tab, index) => <button key={tab} type="button" className={activeTab === index ? 'active' : ''} aria-current={activeTab === index ? 'page' : undefined} onClick={() => onTabChange(index)}>{tab}</button>)}</nav>
    <div className="wb-layout"><nav className="wb-rail" aria-label="Panel visibility"><button type="button" title={`${leftOpen ? 'Hide' : 'Show'} ${leftTitle}`} aria-pressed={leftOpen} onClick={() => setLeftOpen(!leftOpen)}>▤</button><button type="button" title="Show first view" onClick={() => onTabChange(0)}>◇</button></nav>
      {leftOpen && <aside className="wb-side"><h2>{leftTitle}</h2><div>{left}</div></aside>}
      <main className="wb-center">{toolbar && <div className="wb-toolbar">{toolbar}</div>}<div className="wb-content">{children}</div></main>
      {rightOpen && <aside className="wb-detail"><h2>{rightTitle}</h2><div>{right}</div></aside>}
      <nav className="wb-rail right" aria-label="Inspector visibility"><button type="button" title={`${rightOpen ? 'Hide' : 'Show'} ${rightTitle}`} aria-pressed={rightOpen} onClick={() => setRightOpen(!rightOpen)}>☷</button></nav>
    </div>
    <footer className="wb-status">{status}</footer>
  </div>;
}
