import './welcomeDesign.css';

const workspace = 'architecture/editor/editor-workspace.html';
const projects = [
  { name: 'Desert Run', path: '~/projects/desert-run', seen: '2h ago' },
  { name: 'Arena Prototype', path: '~/projects/arena-proto', seen: 'yesterday' },
  { name: 'Tech Demo', path: '~/projects/tech-demo', seen: '3 days ago' },
];

export function WelcomeDesign({ navigate, openDesign }: { navigate: (id: string) => void; openDesign: (id: string) => void }) {
  return <div className="welcome-design"><div className="welcome-frame">
    <aside className="welcome-side"><div className="welcome-logo"><img src={`${import.meta.env.BASE_URL}assets/launcher/logo.png`} alt="Horo Engine" /><strong>HORO</strong><span>Game Engine</span></div>
      <div className="welcome-actions"><button type="button" className="primary" onClick={() => openDesign('architecture/editor/new-project-wizard.html')}><span>＋</span> New Project</button><button type="button" onClick={() => navigate(workspace)}><span>▣</span> Open Project</button><button type="button" onClick={() => navigate(workspace)}><span>↶</span> Open Recent</button><button type="button" onClick={() => openDesign('architecture/editor/settings-modal.html')}><span>⚙</span> Open Settings</button></div>
    </aside>
    <main className="welcome-main"><div className="welcome-section-heading"><h2>Recent Projects</h2><button type="button" onClick={() => navigate(workspace)}>Browse all</button></div><div className="welcome-projects">{projects.map(project => <button type="button" key={project.name} onClick={() => navigate(workspace)}><span className="welcome-project-thumb">▣</span><span><strong>{project.name}</strong><small>{project.path}</small></span><time>{project.seen}</time></button>)}</div>
      <div className="welcome-section-heading"><h2>What's New</h2></div><div className="welcome-news"><article><small>Release Notes</small><h3>GPU-driven rendering preview</h3><p>Experimental render graph and bindless resource backend now available.</p></article><article><small>Documentation</small><h3>MCP workflow guide</h3><p>Author scenes and assets through the Model Context Protocol.</p></article></div>
    </main>
  </div></div>;
}
